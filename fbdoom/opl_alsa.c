//
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//     OPL software emulation driver that renders into the ALSA mixer
//     thread of i_sound_alsa.c. Derived from opl_sdl.c, with SDL_mixer
//     replaced by I_OPL_Mix(), which the mixer calls once per period.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#include "opl2.h"

#include "m_argv.h"
#include "opl.h"
#include "opl_internal.h"
#include "opl_queue.h"

// Provided by i_sound_alsa.c: the mixer's output rate, or 0 if the
// mixer thread is not running.
extern int I_ALSA_MixerRate(void);
extern unsigned int I_ALSA_Underruns(void);

static int show_stats;
static double stat_busy, stat_wall;
static unsigned int stat_periods, stat_max_us;

static uint64_t NowUs(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

typedef struct
{
    unsigned int rate;        // Number of times the timer is advanced per sec.
    unsigned int enabled;     // Non-zero if timer is enabled.
    unsigned int value;       // Last value that was set.
    uint64_t expire_time;     // Calculated time that timer will expire.
} opl_timer_t;

// When the callback mutex is locked using OPL_Lock, callback functions
// are not invoked.

static pthread_mutex_t callback_mutex;

// Queue of callbacks waiting to be invoked.

static opl_callback_queue_t *callback_queue;

// Mutex used to control access to the callback queue.

static pthread_mutex_t callback_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

// Current time, in us since startup:

static uint64_t current_time;

// If non-zero, playback is currently paused.

static int opl_alsa_paused;

// Time offset (in us) due to the fact that callbacks
// were previously paused.

static uint64_t pause_offset;

// OPL software emulator structure.

static opl2_chip *opl_chip;
static int opl_opl3mode;

// Register number that was written.

static int register_num = 0;

// Timers; the emulator does not do timer stuff itself.

static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125, 0, 0, 0 };

static volatile int opl_ready;
static int mixer_rate;      // output rate of the ALSA mixer
static int emu_rate;        // rate the emulator runs at
static int rate_divider;    // mixer_rate / emu_rate (1 or 2)
static int16_t *emu_buffer;
static unsigned int emu_buffer_frames;
static int last_left, last_right;

// Number of emulated samples since a callback was last pending. Once the
// music has been silent for a while the emulator is no longer run, which
// keeps the CPU free while only sound effects are playing.
static unsigned int idle_samples;

// Advance time by the specified number of samples, invoking any
// callback functions as appropriate.

static void AdvanceTime(unsigned int nsamples)
{
    opl_callback_t callback;
    void *callback_data;
    uint64_t us;

    pthread_mutex_lock(&callback_queue_mutex);

    us = ((uint64_t) nsamples * OPL_SECOND) / emu_rate;
    current_time += us;

    if (opl_alsa_paused)
    {
        pause_offset += us;
    }

    // Keep invoking callbacks until there are none left that are due.

    while (!OPL_Queue_IsEmpty(callback_queue)
        && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset)
    {
        if (!OPL_Queue_Pop(callback_queue, &callback, &callback_data))
        {
            break;
        }

        // callback_mutex must be held while a callback runs (so OPL_Lock()
        // can stop callbacks), but callback_queue_mutex must not be, as the
        // callback may call OPL_SetCallback to schedule new callbacks.

        pthread_mutex_unlock(&callback_queue_mutex);

        pthread_mutex_lock(&callback_mutex);
        callback(callback_data);
        pthread_mutex_unlock(&callback_mutex);

        pthread_mutex_lock(&callback_queue_mutex);
    }

    pthread_mutex_unlock(&callback_queue_mutex);
}

// Fills buffer (stereo, 16-bit) with nsamples frames of emulator output,
// invoking music callbacks at the right points in time.

static void RenderEmulator(int16_t *buffer, unsigned int nsamples)
{
    unsigned int filled = 0;

    while (filled < nsamples)
    {
        uint64_t next_callback_time;
        uint64_t n;
        int skip;

        pthread_mutex_lock(&callback_queue_mutex);

        // Work out the time until the next callback waiting in the queue
        // must be invoked; the buffer is filled with that many samples.

        if (opl_alsa_paused || OPL_Queue_IsEmpty(callback_queue))
        {
            n = nsamples - filled;
        }
        else
        {
            next_callback_time = OPL_Queue_Peek(callback_queue) + pause_offset;

            if (next_callback_time <= current_time)
            {
                n = 0;
            }
            else
            {
                n = (next_callback_time - current_time) * emu_rate;
                n = (n + OPL_SECOND - 1) / OPL_SECOND;
            }

            if (n > nsamples - filled)
            {
                n = nsamples - filled;
            }
        }

        if (OPL_Queue_IsEmpty(callback_queue))
        {
            idle_samples += (unsigned int) n;
        }
        else
        {
            idle_samples = 0;
        }
        skip = idle_samples > (unsigned int) emu_rate * 2;

        pthread_mutex_unlock(&callback_queue_mutex);

        if (skip)
        {
            memset(buffer + filled * 2, 0, n * 2 * sizeof(int16_t));
        }
        else
        {
            OPL2_GenerateStream(opl_chip, buffer + filled * 2,
                                (uint32_t) n);
        }
        filled += (unsigned int) n;

        AdvanceTime((unsigned int) n);
    }
}

// Called by the ALSA mixer once per period: adds `frames` frames of music
// to the 32-bit stereo mix buffer.

// Music volume as set by the menu: 0-127, with 120 (slider maximum) as unity.
static volatile int mix_volume = 120;

void I_OPL_SetMixVolume(int volume)
{
    mix_volume = volume;
}

void I_OPL_Mix(int32_t *mix, unsigned int frames)
{
    unsigned int n, i;

    if (!opl_ready)
    {
        return;
    }

    n = frames / rate_divider;
    if (n == 0 || n > emu_buffer_frames)
    {
        return;
    }

    uint64_t t0 = show_stats ? NowUs() : 0;

    RenderEmulator(emu_buffer, n);

    if (show_stats)
    {
        unsigned int us = (unsigned int) (NowUs() - t0);

        stat_busy += us;
        stat_wall += (double) frames * 1000000 / mixer_rate;
        if (us > stat_max_us)
        {
            stat_max_us = us;
        }
        if (++stat_periods >= 200)
        {
            fprintf(stderr, "OPL: %.0f%% of real time, worst period %u us "
                    "(budget %u us), underruns %u\n",
                    stat_busy * 100 / stat_wall, stat_max_us,
                    (unsigned int) ((uint64_t) frames * 1000000 / mixer_rate),
                    I_ALSA_Underruns());
            stat_busy = stat_wall = 0;
            stat_periods = stat_max_us = 0;
        }
    }

    {
        int gain = mix_volume * 256 / 120;

        if (gain != 256)
        {
            for (i = 0; i < n * 2; ++i)
            {
                emu_buffer[i] = (int16_t) ((emu_buffer[i] * gain) >> 8);
            }
        }
    }

    if (rate_divider == 1)
    {
        for (i = 0; i < n; ++i)
        {
            mix[i * 2] += emu_buffer[i * 2];
            mix[i * 2 + 1] += emu_buffer[i * 2 + 1];
        }
    }
    else
    {
        // Upsample by two with linear interpolation.
        for (i = 0; i < n; ++i)
        {
            int l = emu_buffer[i * 2];
            int r = emu_buffer[i * 2 + 1];

            mix[i * 4] += (last_left + l) >> 1;
            mix[i * 4 + 1] += (last_right + r) >> 1;
            mix[i * 4 + 2] += l;
            mix[i * 4 + 3] += r;
            last_left = l;
            last_right = r;
        }
    }
}

static void OPL_ALSA_Shutdown(void)
{
    opl_ready = 0;

    // Make sure the mixer is out of I_OPL_Mix() before freeing anything.
    pthread_mutex_lock(&callback_mutex);
    pthread_mutex_unlock(&callback_mutex);

    if (callback_queue != NULL)
    {
        OPL_Queue_Destroy(callback_queue);
        callback_queue = NULL;
    }
    free(emu_buffer);
    emu_buffer = NULL;
}

static int OPL_ALSA_Init(unsigned int port_base)
{
    pthread_mutexattr_t attr;

    (void) port_base;

    show_stats = M_CheckParm("-oplstats") > 0;
    mixer_rate = I_ALSA_MixerRate();
    if (mixer_rate <= 0)
    {
        fprintf(stderr, "OPL: no ALSA mixer running, music disabled\n");
        return 0;
    }

    // The emulator costs a lot on a slow CPU: run it at half the output
    // rate (still above 22 kHz) unless -oplhq asks for the full rate.
    rate_divider = M_CheckParm("-oplhq") ? 1 : 2;
    emu_rate = mixer_rate / rate_divider;
    opl_sample_rate = (unsigned int) emu_rate;

    // Large enough for any mixer period (the period is 10-50 ms).
    emu_buffer_frames = (unsigned int) emu_rate / 4;
    emu_buffer = malloc(emu_buffer_frames * 2 * sizeof(int16_t));
    if (emu_buffer == NULL)
    {
        return 0;
    }

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&callback_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    opl_alsa_paused = 0;
    pause_offset = 0;
    current_time = 0;
    idle_samples = 0;
    last_left = last_right = 0;
    callback_queue = OPL_Queue_Create();

    if (opl_chip == NULL)
    {
        opl_chip = OPL2_Create();
    }
    OPL2_Reset(opl_chip, (uint32_t) emu_rate);
    opl_opl3mode = 0;

    fprintf(stderr, "OPL: music emulated at %d Hz\n", emu_rate);
    opl_ready = 1;
    return 1;
}

static unsigned int OPL_ALSA_PortRead(opl_port_t port)
{
    unsigned int result = 0;

    if (port == OPL_REGISTER_PORT_OPL3)
    {
        return 0xff;
    }

    if (timer1.enabled && current_time > timer1.expire_time)
    {
        result |= 0x80;   // Either have expired
        result |= 0x40;   // Timer 1 has expired
    }

    if (timer2.enabled && current_time > timer2.expire_time)
    {
        result |= 0x80;   // Either have expired
        result |= 0x20;   // Timer 2 has expired
    }

    return result;
}

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    int tics;

    // If the timer is enabled, calculate the time when the timer
    // will expire.

    if (timer->enabled)
    {
        tics = 0x100 - timer->value;
        timer->expire_time = current_time
                           + ((uint64_t) tics * OPL_SECOND) / timer->rate;
    }
}

static void WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value;
            OPLTimer_CalculateEndTime(&timer1);
            break;

        case OPL_REG_TIMER2:
            timer2.value = value;
            OPLTimer_CalculateEndTime(&timer2);
            break;

        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                timer1.enabled = 0;
                timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    timer1.enabled = (value & 0x01) != 0;
                    OPLTimer_CalculateEndTime(&timer1);
                }

                if ((value & 0x20) == 0)
                {
                    timer1.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(&timer2);
                }
            }

            break;

        case OPL_REG_NEW:
            opl_opl3mode = value & 0x01;
            // fall through

        default:
            OPL2_WriteReg(opl_chip, (uint16_t) reg_num, (uint8_t) value);
            break;
    }
}

static void OPL_ALSA_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)
    {
        register_num = value;
    }
    else if (port == OPL_REGISTER_PORT_OPL3)
    {
        register_num = value | 0x100;
    }
    else if (port == OPL_DATA_PORT)
    {
        WriteRegister(register_num, value);
    }
}

static void OPL_ALSA_SetCallback(uint64_t us, opl_callback_t callback,
                                 void *data)
{
    pthread_mutex_lock(&callback_queue_mutex);
    OPL_Queue_Push(callback_queue, callback, data,
                   current_time - pause_offset + us);
    pthread_mutex_unlock(&callback_queue_mutex);
}

static void OPL_ALSA_ClearCallbacks(void)
{
    pthread_mutex_lock(&callback_queue_mutex);
    OPL_Queue_Clear(callback_queue);
    pthread_mutex_unlock(&callback_queue_mutex);
}

static void OPL_ALSA_Lock(void)
{
    pthread_mutex_lock(&callback_mutex);
}

static void OPL_ALSA_Unlock(void)
{
    pthread_mutex_unlock(&callback_mutex);
}

static void OPL_ALSA_SetPaused(int paused)
{
    opl_alsa_paused = paused;
}

static void OPL_ALSA_AdjustCallbacks(float factor)
{
    pthread_mutex_lock(&callback_queue_mutex);
    OPL_Queue_AdjustCallbacks(callback_queue, current_time, factor);
    pthread_mutex_unlock(&callback_queue_mutex);
}

opl_driver_t opl_alsa_driver =
{
    "ALSA",
    OPL_ALSA_Init,
    OPL_ALSA_Shutdown,
    OPL_ALSA_PortRead,
    OPL_ALSA_PortWrite,
    OPL_ALSA_SetCallback,
    OPL_ALSA_ClearCallbacks,
    OPL_ALSA_Lock,
    OPL_ALSA_Unlock,
    OPL_ALSA_SetPaused,
    OPL_ALSA_AdjustCallbacks,
};
