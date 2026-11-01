//
// ALSA sound-effects backend for framebuffer Doom.
//

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deh_str.h"
#include "doomdef.h"
#include "doomtype.h"
#include "i_sound.h"
#include "i_system.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

#define ALSA_CHANNELS 16

typedef struct
{
    unsigned int sample_rate;
    unsigned int sample_count;
    byte *samples;
} cached_sound_t;

typedef struct
{
    cached_sound_t *sound;
    uint64_t position;
    uint64_t step;
    int volume;
    int separation;
} playing_sound_t;

static snd_pcm_t *pcm;
static snd_pcm_uframes_t frames_per_update;
static unsigned int output_rate;
static playing_sound_t channels[ALSA_CHANNELS];
static int use_sfx_prefix;
static int16_t *output_buffer;
static int32_t *mix_buffer;

// Mixing runs in its own thread so that playback keeps flowing when the game
// renders slower than TICRATE; channels[] is protected by channel_lock.
static pthread_t mixer_thread;
static pthread_mutex_t channel_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int mixer_running;
static int mixer_started;

static sfxinfo_t *ResolveSound(sfxinfo_t *sfxinfo)
{
    while (sfxinfo->link != NULL)
    {
        sfxinfo = sfxinfo->link;
    }

    return sfxinfo;
}

static void GetSfxLumpName(sfxinfo_t *sfxinfo, char *name, size_t name_size)
{
    sfxinfo = ResolveSound(sfxinfo);

    if (use_sfx_prefix)
    {
        snprintf(name, name_size, "ds%s", DEH_String(sfxinfo->name));
    }
    else
    {
        snprintf(name, name_size, "%s", DEH_String(sfxinfo->name));
    }
}

static cached_sound_t *CacheSound(sfxinfo_t *sfxinfo)
{
    byte *lump_data;
    unsigned int lump_length;
    unsigned int sample_count;
    unsigned int sample_rate;
    int lump_number;
    char name[16];
    cached_sound_t *sound;

    sfxinfo = ResolveSound(sfxinfo);

    if (sfxinfo->driver_data != NULL)
    {
        return sfxinfo->driver_data;
    }

    GetSfxLumpName(sfxinfo, name, sizeof(name));
    lump_number = W_GetNumForName(name);
    lump_length = W_LumpLength(lump_number);
    if (lump_length < 8)
    {
        fprintf(stderr, "ALSA: invalid sound lump %s\n", name);
        return NULL;
    }

    lump_data = W_CacheLumpNum(lump_number, PU_STATIC);
    sample_rate = lump_data[2] | ((unsigned int) lump_data[3] << 8);
    sample_count = lump_data[4]
                 | ((unsigned int) lump_data[5] << 8)
                 | ((unsigned int) lump_data[6] << 16)
                 | ((unsigned int) lump_data[7] << 24);

    if (lump_data[0] != 3 || lump_data[1] != 0
     || sample_rate == 0 || sample_count > lump_length - 8
     || sample_count <= 48)
    {
        fprintf(stderr, "ALSA: invalid sound data in lump %s\n", name);
        W_ReleaseLumpNum(lump_number);
        return NULL;
    }

    sound = calloc(1, sizeof(*sound));
    if (sound == NULL)
    {
        fprintf(stderr, "ALSA: out of memory caching sound lump %s\n", name);
        W_ReleaseLumpNum(lump_number);
        return NULL;
    }

    sound->sample_count = sample_count - 32;
    sound->sample_rate = sample_rate;
    sound->samples = malloc(sound->sample_count);
    if (sound->samples == NULL)
    {
        fprintf(stderr, "ALSA: out of memory caching sound lump %s\n", name);
        free(sound);
        W_ReleaseLumpNum(lump_number);
        return NULL;
    }

    memcpy(sound->samples, lump_data + 24, sound->sample_count);
    W_ReleaseLumpNum(lump_number);
    sfxinfo->driver_data = sound;
    return sound;
}

static int GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    char name[16];

    GetSfxLumpName(sfxinfo, name, sizeof(name));
    return W_GetNumForName(name);
}

static void *MixerMain(void *arg);

static boolean InitSound(boolean sfx_prefix)
{
    const char *device = getenv("ALSA_PCM_DEVICE");
    snd_pcm_hw_params_t *params;
    snd_pcm_uframes_t period_size;
    unsigned int periods = 2;
    snd_pcm_uframes_t buffer_size;
    int err;
    int i;

    use_sfx_prefix = sfx_prefix;
    memset(channels, 0, sizeof(channels));

    if (device == NULL || device[0] == '\0')
    {
        device = "default";
    }

    err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
    {
        fprintf(stderr, "ALSA: cannot open playback device '%s': %s\n",
                device, snd_strerror(err));
        return false;
    }

    err = snd_pcm_hw_params_malloc(&params);
    if (err < 0)
    {
        fprintf(stderr, "ALSA: cannot allocate hardware parameters: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        pcm = NULL;
        return false;
    }

    err = snd_pcm_hw_params_any(pcm, params);
    if (err >= 0)
    {
        err = snd_pcm_hw_params_set_access(pcm, params,
                                           SND_PCM_ACCESS_RW_INTERLEAVED);
    }
    if (err >= 0)
    {
        err = snd_pcm_hw_params_set_format(pcm, params,
                                           SND_PCM_FORMAT_S16);
    }
    if (err >= 0)
    {
        err = snd_pcm_hw_params_set_channels(pcm, params, 2);
    }

    output_rate = snd_samplerate > 0 ? (unsigned int) snd_samplerate : 44100;
    if (err >= 0)
    {
        err = snd_pcm_hw_params_set_rate_near(pcm, params, &output_rate, 0);
    }

    period_size = output_rate / 200;
    if (period_size == 0)
    {
        period_size = 1;
    }
    if (err >= 0)
    {
        err = snd_pcm_hw_params_set_period_size_near(pcm, params,
                                                    &period_size, 0);
    }
    if (err >= 0)
    {
        err = snd_pcm_hw_params_set_periods_near(pcm, params, &periods, 0);
    }
    if (err >= 0)
    {
        err = snd_pcm_hw_params(pcm, params);
    }
    if (err >= 0)
    {
        snd_pcm_hw_params_get_period_size(params, &period_size, NULL);
        snd_pcm_hw_params_get_buffer_size(params, &buffer_size);
        fprintf(stderr, "ALSA: period %lu frames, buffer %lu frames "
                        "(%lu ms)\n", (unsigned long) period_size,
                (unsigned long) buffer_size,
                (unsigned long) (buffer_size * 1000 / output_rate));
    }
    snd_pcm_hw_params_free(params);

    if (err < 0)
    {
        fprintf(stderr, "ALSA: cannot configure playback: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        pcm = NULL;
        return false;
    }

    // Mix in period-sized chunks so a write never waits for a partial period.
    frames_per_update = period_size;
    if (frames_per_update == 0)
    {
        frames_per_update = 1;
    }

    output_buffer = malloc(frames_per_update * 2 * sizeof(*output_buffer));
    mix_buffer = malloc(frames_per_update * 2 * sizeof(*mix_buffer));
    if (output_buffer == NULL || mix_buffer == NULL)
    {
        fprintf(stderr, "ALSA: cannot allocate audio mix buffers\n");
        free(output_buffer);
        free(mix_buffer);
        output_buffer = NULL;
        mix_buffer = NULL;
        snd_pcm_close(pcm);
        pcm = NULL;
        return false;
    }

    err = snd_pcm_prepare(pcm);
    if (err < 0)
    {
        fprintf(stderr, "ALSA: cannot prepare playback: %s\n",
                snd_strerror(err));
        free(output_buffer);
        free(mix_buffer);
        output_buffer = NULL;
        mix_buffer = NULL;
        snd_pcm_close(pcm);
        pcm = NULL;
        return false;
    }

    snd_samplerate = output_rate;
    for (i = 0; i < ALSA_CHANNELS; ++i)
    {
        channels[i].sound = NULL;
    }

    mixer_running = 1;
    if (pthread_create(&mixer_thread, NULL, MixerMain, NULL) != 0)
    {
        fprintf(stderr, "ALSA: cannot start mixer thread\n");
        mixer_running = 0;
        free(output_buffer);
        free(mix_buffer);
        output_buffer = NULL;
        mix_buffer = NULL;
        snd_pcm_close(pcm);
        pcm = NULL;
        return false;
    }
    mixer_started = 1;

    fprintf(stderr, "ALSA: sound effects at %u Hz on '%s'\n",
            output_rate, device);
    return true;
}

static void ShutdownSound(void)
{
    int i;

    if (mixer_started)
    {
        mixer_running = 0;
        pthread_join(mixer_thread, NULL);
        mixer_started = 0;
    }

    if (pcm != NULL)
    {
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
        pcm = NULL;
    }

    free(output_buffer);
    free(mix_buffer);
    output_buffer = NULL;
    mix_buffer = NULL;

    for (i = 0; i < NUMSFX; ++i)
    {
        cached_sound_t *sound = S_sfx[i].driver_data;

        if (sound != NULL)
        {
            free(sound->samples);
            free(sound);
            S_sfx[i].driver_data = NULL;
        }
    }
}

static int StartSound(sfxinfo_t *sfxinfo, int channel, int volume, int sep)
{
    cached_sound_t *sound;

    if (pcm == NULL || channel < 0 || channel >= ALSA_CHANNELS)
    {
        return -1;
    }

    sound = CacheSound(sfxinfo);
    if (sound == NULL)
    {
        return -1;
    }

    pthread_mutex_lock(&channel_lock);
    channels[channel].sound = sound;
    channels[channel].position = 0;
    channels[channel].step = ((uint64_t) sound->sample_rate << 16)
                           / output_rate;
    channels[channel].volume = volume;
    channels[channel].separation = sep;
    pthread_mutex_unlock(&channel_lock);
    return channel;
}

static void StopSound(int channel)
{
    if (channel >= 0 && channel < ALSA_CHANNELS)
    {
        pthread_mutex_lock(&channel_lock);
        channels[channel].sound = NULL;
        pthread_mutex_unlock(&channel_lock);
    }
}

static boolean SoundIsPlaying(int channel)
{
    boolean playing;

    if (channel < 0 || channel >= ALSA_CHANNELS)
    {
        return false;
    }

    pthread_mutex_lock(&channel_lock);
    playing = channels[channel].sound != NULL;
    pthread_mutex_unlock(&channel_lock);
    return playing;
}

static void UpdateSoundParams(int channel, int volume, int sep)
{
    if (channel >= 0 && channel < ALSA_CHANNELS)
    {
        pthread_mutex_lock(&channel_lock);
        channels[channel].volume = volume;
        channels[channel].separation = sep;
        pthread_mutex_unlock(&channel_lock);
    }
}

static void MixAndWrite(void)
{
    snd_pcm_uframes_t frame;
    int channel;

    memset(mix_buffer, 0,
           frames_per_update * 2 * sizeof(*mix_buffer));

    pthread_mutex_lock(&channel_lock);
    for (channel = 0; channel < ALSA_CHANNELS; ++channel)
    {
        playing_sound_t *playing = &channels[channel];
        cached_sound_t *sound = playing->sound;
        int left_volume, right_volume;

        if (sound == NULL)
        {
            continue;
        }

        left_volume = ((254 - playing->separation) * playing->volume) / 127;
        right_volume = (playing->separation * playing->volume) / 127;

        for (frame = 0; frame < frames_per_update; ++frame)
        {
            unsigned int sample_index = playing->position >> 16;
            int sample;

            if (sample_index >= sound->sample_count)
            {
                playing->sound = NULL;
                break;
            }

            sample = ((int) sound->samples[sample_index] - 128) << 8;
            mix_buffer[frame * 2] += sample * left_volume / 127;
            mix_buffer[frame * 2 + 1] += sample * right_volume / 127;
            playing->position += playing->step;
        }
    }
    pthread_mutex_unlock(&channel_lock);

    for (frame = 0; frame < frames_per_update * 2; ++frame)
    {
        int32_t sample = mix_buffer[frame];

        if (sample > INT16_MAX)
        {
            sample = INT16_MAX;
        }
        else if (sample < INT16_MIN)
        {
            sample = INT16_MIN;
        }

        output_buffer[frame] = (int16_t) sample;
    }
}

static void *MixerMain(void *arg)
{
    (void) arg;

    while (mixer_running)
    {
        snd_pcm_uframes_t frame = 0;

        MixAndWrite();

        while (frame < frames_per_update && mixer_running)
        {
            snd_pcm_sframes_t written =
                snd_pcm_writei(pcm, output_buffer + frame * 2,
                               frames_per_update - frame);

            if (written < 0)
            {
                if (snd_pcm_recover(pcm, (int) written, 1) < 0)
                {
                    fprintf(stderr, "ALSA: playback failed, sound stopped\n");
                    return NULL;
                }
            }
            else
            {
                frame += written;
            }
        }
    }

    return NULL;
}

// Mixing is done by the mixer thread; nothing to do per game tic.
static void UpdateSound(void)
{
}

static void CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    (void) sounds;
    (void) num_sounds;
}

static snddevice_t sound_devices[] =
{
    SNDDEVICE_SB
};

sound_module_t sound_alsa_module =
{
    sound_devices,
    1,
    InitSound,
    ShutdownSound,
    GetSfxLumpNum,
    UpdateSound,
    UpdateSoundParams,
    StartSound,
    StopSound,
    SoundIsPlaying,
    CacheSounds
};
