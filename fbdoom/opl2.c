//
// Lightweight integer OPL2 (YM3812) emulator for CPUs without an FPU.
//
// Only the melodic 2-operator mode is emulated (no rhythm mode, no OPL3),
// which is all the Doom music driver uses. It follows the structure of the
// real chip (log-scaled envelope, phase modulation, feedback, tremolo,
// vibrato, key scaling, four waveforms) but silent operators cost nothing
// and the output rate is free to be lower than the chip's 49716 Hz.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2 of the License, or (at your
// option) any later version.
//

#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "opl2.h"

#define CHIP_RATE 49716.0
#define EG_TICK_RATE 24858.0  // envelope updates/s (half the sample rate)
#define ATT_MAX 511
#define FP 16                 // envelope level fixed-point bits

enum { EG_OFF, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

typedef struct
{
    int am, vib, egt, ksr, mult, ksl, tl, ar, dr, sl, rr, wave;
    uint32_t phase;
    uint32_t inc;         // phase increment per output sample (20-bit phase)
    int32_t level;        // attenuation, ATT_MAX << FP is silent
    int32_t sl_level;
    int32_t base_att;     // total level + key scale attenuation
    int state;
    int rate_atk, rate_dec, rate_sus, rate_rel;
    int out, prev;
} op_t;

typedef struct
{
    int fnum, block, keyon, fb, conn;
} chan_t;

struct opl2_chip
{
    op_t op[18];
    chan_t ch[9];
    int rate;
    int am_depth, vib_depth;
    uint32_t trem_acc, trem_step;   // 16.16, advances 64-sample units
    uint32_t vib_acc, vib_step;
    int32_t dec_inc[64];
    int32_t atk_f[64];
    double rate_scale;              // CHIP_RATE / output rate
};

// Operators are numbered in register order: per group of six, three
// modulators followed by three carriers.
#define CHAN_OF(n) ((n) / 6 * 3 + (n) % 3)

static int16_t sin_tab[1024];
static int16_t gain_tab[ATT_MAX + 1];
static int tables_ready;

static const int mult_tab[16] =
    { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
static const int ksl_rom[16] =
    { 0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64 };
static const int ksl_shift[4] = { 8, 1, 2, 0 };

// Register offset -> operator index (-1: unused).
static const int8_t slot_of[32] =
{
     0,  1,  2,  3,  4,  5, -1, -1,  6,  7,  8,  9, 10, 11, -1, -1,
    12, 13, 14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

static void InitTables(void)
{
    int i;

    if (tables_ready)
    {
        return;
    }

    for (i = 0; i < 1024; ++i)
    {
        sin_tab[i] = (int16_t) lrint(sin((i + 0.5) * 2 * M_PI / 1024) * 4084);
    }
    for (i = 0; i <= ATT_MAX; ++i)
    {
        gain_tab[i] = (int16_t) lrint(32767 * pow(10.0, -i * 0.1875 / 20));
    }
    gain_tab[ATT_MAX] = 0;
    tables_ready = 1;
}

static int RateIndex(int reg, int keycode, int ksr)
{
    int r;

    if (reg == 0)
    {
        return 0;
    }
    r = reg * 4 + (ksr ? keycode : keycode >> 2);
    return r > 63 ? 63 : r;
}

// Recompute everything that depends on frequency or operator registers.
static void UpdateOp(opl2_chip *c, int n)
{
    op_t *o = &c->op[n];
    chan_t *ch = &c->ch[CHAN_OF(n)];
    int keycode = (ch->block << 1) | (ch->fnum >> 9);
    int ksl;

    ksl = (ksl_rom[ch->fnum >> 6] << 2) - ((8 - ch->block) << 5);
    if (ksl < 0)
    {
        ksl = 0;
    }
    o->base_att = o->tl * 4 + (ksl >> ksl_shift[o->ksl]);

    o->rate_atk = RateIndex(o->ar, keycode, o->ksr);
    o->rate_dec = RateIndex(o->dr, keycode, o->ksr);
    o->rate_rel = RateIndex(o->rr, keycode, o->ksr);
    o->rate_sus = o->egt ? 0 : o->rate_rel;
    o->sl_level = (o->sl == 15 ? 31 : o->sl) * 16 << FP;
}

static void UpdatePhaseInc(opl2_chip *c, int n, int vib_delta)
{
    op_t *o = &c->op[n];
    chan_t *ch = &c->ch[CHAN_OF(n)];
    int fnum = ch->fnum + vib_delta;
    double inc = ((double) ((fnum << ch->block) * mult_tab[o->mult]) / 2.0)
               * c->rate_scale;

    o->inc = (uint32_t) inc;
}

static void RecalcChannel(opl2_chip *c, int chn)
{
    static const int mod_of[9] = { 0, 1, 2, 6, 7, 8, 12, 13, 14 };
    int m = mod_of[chn];

    UpdateOp(c, m);
    UpdateOp(c, m + 3);
    UpdatePhaseInc(c, m, 0);
    UpdatePhaseInc(c, m + 3, 0);
}

static void KeyOn(opl2_chip *c, int chn)
{
    static const int mod_of[9] = { 0, 1, 2, 6, 7, 8, 12, 13, 14 };
    int i;

    for (i = 0; i < 2; ++i)
    {
        op_t *o = &c->op[mod_of[chn] + i * 3];

        o->phase = 0;
        o->prev = o->out = 0;
        if (o->rate_atk >= 60)
        {
            o->level = 0;
            o->state = EG_DECAY;
        }
        else
        {
            o->state = o->rate_atk ? EG_ATTACK : EG_OFF;
        }
        if (o->state == EG_OFF)
        {
            o->level = ATT_MAX << FP;
        }
    }
}

static void KeyOff(opl2_chip *c, int chn)
{
    static const int mod_of[9] = { 0, 1, 2, 6, 7, 8, 12, 13, 14 };
    int i;

    for (i = 0; i < 2; ++i)
    {
        op_t *o = &c->op[mod_of[chn] + i * 3];

        if (o->state != EG_OFF)
        {
            o->state = EG_RELEASE;
        }
    }
}

opl2_chip *OPL2_Create(void)
{
    return calloc(1, sizeof(opl2_chip));
}

void OPL2_Reset(opl2_chip *c, uint32_t rate)
{
    int r, i;

    InitTables();
    memset(c, 0, sizeof(*c));
    c->rate = (int) rate;
    c->rate_scale = CHIP_RATE / rate;
    c->trem_step = (uint32_t) (65536.0 * (CHIP_RATE / 64.0) / rate);
    c->vib_step = (uint32_t) (65536.0 * (CHIP_RATE / 1024.0) / rate);

    for (r = 1; r < 64; ++r)
    {
        int h = r >> 2, l = r & 3;
        double per_tick = (double) ((4 + l) << h) / 16384.0;
        double dec = per_tick * EG_TICK_RATE / rate * 65536.0;
        double atk = per_tick / 8.0 * EG_TICK_RATE / rate;

        c->dec_inc[r] = (int32_t) (dec > 4 * 65536.0 ? 4 * 65536.0 : dec);
        c->atk_f[r] = (int32_t) ((atk > 0.5 ? 0.5 : atk) * 65536.0);
    }

    for (i = 0; i < 18; ++i)
    {
        c->op[i].level = ATT_MAX << FP;
        c->op[i].state = EG_OFF;
    }
}

void OPL2_WriteReg(opl2_chip *c, uint16_t reg, uint8_t v)
{
    int hi = reg & 0xe0, low = reg & 0x1f;

    if (reg == 0xbd)
    {
        c->am_depth = (v >> 7) & 1;
        c->vib_depth = (v >> 6) & 1;
        return;
    }

    switch (hi)
    {
        case 0x20:
        case 0x40:
        case 0x60:
        case 0x80:
        case 0xe0:
        {
            int n = slot_of[low];
            op_t *o;
            int chn;

            if (n < 0 || reg >= 0xf6)
            {
                return;
            }
            o = &c->op[n];
            chn = CHAN_OF(n);

            if (hi == 0x20)
            {
                o->am = (v >> 7) & 1;
                o->vib = (v >> 6) & 1;
                o->egt = (v >> 5) & 1;
                o->ksr = (v >> 4) & 1;
                o->mult = v & 15;
            }
            else if (hi == 0x40)
            {
                o->ksl = v >> 6;
                o->tl = v & 63;
            }
            else if (hi == 0x60)
            {
                o->ar = v >> 4;
                o->dr = v & 15;
            }
            else if (hi == 0x80)
            {
                o->sl = v >> 4;
                o->rr = v & 15;
            }
            else
            {
                o->wave = v & 3;
            }
            RecalcChannel(c, chn);
            break;
        }

        default:
            if (reg >= 0xa0 && reg <= 0xa8)
            {
                int chn = reg - 0xa0;

                c->ch[chn].fnum = (c->ch[chn].fnum & 0x300) | v;
                RecalcChannel(c, chn);
            }
            else if (reg >= 0xb0 && reg <= 0xb8)
            {
                int chn = reg - 0xb0;
                int on = (v >> 5) & 1;

                c->ch[chn].fnum = (c->ch[chn].fnum & 0xff) | ((v & 3) << 8);
                c->ch[chn].block = (v >> 2) & 7;
                RecalcChannel(c, chn);
                if (on && !c->ch[chn].keyon)
                {
                    KeyOn(c, chn);
                }
                else if (!on && c->ch[chn].keyon)
                {
                    KeyOff(c, chn);
                }
                c->ch[chn].keyon = on;
            }
            else if (reg >= 0xc0 && reg <= 0xc8)
            {
                int chn = reg - 0xc0;

                c->ch[chn].fb = (v >> 1) & 7;
                c->ch[chn].conn = v & 1;
            }
            break;
    }
}

static inline int WaveOut(const op_t *o, int phase10, int att)
{
    int s;

    phase10 &= 1023;
    switch (o->wave)
    {
        case 1:
            if (phase10 & 512)
            {
                return 0;
            }
            s = sin_tab[phase10];
            break;
        case 2:
            s = sin_tab[phase10];
            s = s < 0 ? -s : s;
            break;
        case 3:
            if (phase10 & 256)
            {
                return 0;
            }
            s = sin_tab[phase10 & 255];
            s = s < 0 ? -s : s;
            break;
        default:
            s = sin_tab[phase10];
            break;
    }

    if (att > ATT_MAX)
    {
        att = ATT_MAX;
    }
    return (s * gain_tab[att]) >> 15;
}

static inline void StepEnvelope(opl2_chip *c, op_t *o)
{
    switch (o->state)
    {
        case EG_ATTACK:
            o->level -= (int32_t) (((int64_t) (o->level + (1 << FP))
                                    * c->atk_f[o->rate_atk]) >> 16);
            if (o->level <= (1 << FP) / 2)
            {
                o->level = 0;
                o->state = EG_DECAY;
            }
            break;

        case EG_DECAY:
            if (o->rate_dec)
            {
                o->level += c->dec_inc[o->rate_dec];
            }
            if (o->level >= o->sl_level)
            {
                o->level = o->sl_level;
                o->state = EG_SUSTAIN;
            }
            break;

        case EG_SUSTAIN:
            if (o->rate_sus)
            {
                o->level += c->dec_inc[o->rate_sus];
            }
            break;

        case EG_RELEASE:
            if (o->rate_rel)
            {
                o->level += c->dec_inc[o->rate_rel];
            }
            break;

        default:
            return;
    }

    if (o->level >= (ATT_MAX << FP))
    {
        o->level = ATT_MAX << FP;
        o->state = EG_OFF;
    }
}

void OPL2_GenerateStream(opl2_chip *c, int16_t *buf, uint32_t n)
{
    static const int mod_of[9] = { 0, 1, 2, 6, 7, 8, 12, 13, 14 };
    uint32_t s;

    for (s = 0; s < n; ++s)
    {
        int trem, vibpos, chn, sum = 0;
        int tri;

        c->trem_acc += c->trem_step;
        tri = (c->trem_acc >> 16) % 210;
        tri = tri < 105 ? tri : 210 - tri;
        trem = c->am_depth ? tri >> 2 : tri >> 4;

        c->vib_acc += c->vib_step;
        vibpos = (c->vib_acc >> 16) & 7;

        for (chn = 0; chn < 9; ++chn)
        {
            chan_t *ch = &c->ch[chn];
            op_t *m = &c->op[mod_of[chn]];
            op_t *k = &c->op[mod_of[chn] + 3];
            int fbmod = 0, modout, outv;

            if (k->state == EG_OFF && (!ch->conn || m->state == EG_OFF))
            {
                continue;
            }

            // Modulator, with feedback.
            if (ch->fb)
            {
                fbmod = (m->prev + m->out) >> (9 - ch->fb);
            }
            if (m->vib)
            {
                int range = (ch->fnum >> 7) & 7, d = 0;

                if ((vibpos & 3) == 1 || (vibpos & 3) == 3)
                {
                    d = range;
                }
                else if ((vibpos & 3) == 2)
                {
                    d = range << 1;
                }
                if (!c->vib_depth)
                {
                    d >>= 1;
                }
                UpdatePhaseInc(c, mod_of[chn], vibpos & 4 ? -d : d);
            }
            StepEnvelope(c, m);
            m->prev = m->out;
            m->phase += m->inc;
            m->out = WaveOut(m, (int) ((m->phase >> 10) & 1023) + fbmod,
                             (m->level >> FP) + m->base_att
                             + (m->am ? trem : 0));
            modout = m->out;

            // Carrier.
            if (k->vib)
            {
                int range = (ch->fnum >> 7) & 7, d = 0;

                if ((vibpos & 3) == 1 || (vibpos & 3) == 3)
                {
                    d = range;
                }
                else if ((vibpos & 3) == 2)
                {
                    d = range << 1;
                }
                if (!c->vib_depth)
                {
                    d >>= 1;
                }
                UpdatePhaseInc(c, mod_of[chn] + 3, vibpos & 4 ? -d : d);
            }
            StepEnvelope(c, k);
            k->phase += k->inc;
            outv = WaveOut(k, (int) ((k->phase >> 10) & 1023)
                           + (ch->conn ? 0 : modout),
                           (k->level >> FP) + k->base_att
                           + (k->am ? trem : 0));
            if (ch->conn)
            {
                outv += modout;
            }
            sum += outv;
        }

        buf[s * 2] = buf[s * 2 + 1] =
            (int16_t) (sum > 32767 ? 32767 : (sum < -32768 ? -32768 : sum));
    }
}
