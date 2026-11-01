/* SPDX-License-Identifier: GPL-3.0-only */
/* URANUS 88: a drum machine after the 808, beside the synth. Eight analog-style voices (bass drum, snare, low
 * tom, clap, rim shot, cowbell, closed and open hi-hat), each with TUNE, DECAY and LEVEL; eight patterns of 16
 * steps with an accent row, played by the same clock as the arpeggiator and the sequencer. Everything here runs
 * in the render, on core 0; included by jp_engine.h. */
#ifndef JP_DRUMS_H
#define JP_DRUMS_H

static volatile uint16_t jp_pat[DR_PATTERNS][DR_ROWS];   /* bit s: step s */
static volatile uint8_t jp_dk[3][DR_N];                  /* [0] tune [1] decay [2] level, 0..127 */
static volatile uint8_t jp_dr_step_shown = 255;

typedef struct {
    uint8_t on;
    float env, k, env2, k2;                     /* the body and a second envelope (noise, click, bursts) */
    float ph, ph2, inc, inc2, drop, kdrop;      /* oscillators, a pitch drop */
    float lp, bp, f;                            /* a filter */
    float vel;
    uint32_t t;                                 /* frames since the hit */
} jp_drum_t;

static jp_drum_t dr[DR_N];
static uint8_t dr_pend[DR_N];                   /* a hit waiting for its control step (+1), 0: none */
static float dr_pvel[DR_N];
static float hat_ph[6], hat_hp1, hat_hp1x, hat_hp2, hat_hp2x;
static uint32_t dr_nz = 0x2545F491u;
static float dbufL[JP_MAXN], dbufR[JP_MAXN];

static const float DR_PAN[DR_N] = {0.0f, 0.05f, -0.3f, 0.12f, 0.3f, -0.32f, 0.38f, -0.22f};
static const float HAT_F[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};

static inline float dr_noise(void)
{
    dr_nz ^= dr_nz << 13; dr_nz ^= dr_nz >> 17; dr_nz ^= dr_nz << 5;
    return (float)(int32_t)dr_nz * (1.0f / 2147483648.0f);
}

static inline float dr_sin(float ph)            /* one cycle over 0..1, from 0 */
{
    float w = 2.0f * ph - 1.0f;
    return -4.0f * w * (1.0f - jp_absf(w));
}

static inline float dr_k(float tau) { return jp_exp2(-1.442695f / (tau * JP_SR)); }   /* per-frame decay */

static void dr_trigger(uint32_t i, float vel, uint32_t sb)   /* from the clock or MIDI: at control step sb */
{
    if (i >= DR_N) return;
    dr_pend[i] = (uint8_t)(sb + 1u);
    dr_pvel[i] = vel;
}

static void dr_start(uint32_t i)
{
    jp_drum_t *d = &dr[i];
    float t = (float)jp_dk[0][i] * (1.0f / 127.0f), dc = (float)jp_dk[1][i] * (1.0f / 127.0f);
    float lv = (float)jp_dk[2][i] * (1.0f / 127.0f), dk = jp_exp2(dc * 4.0f - 2.0f);   /* 0.25 .. 4 */
    d->on = 1;
    d->t = 0;
    d->vel = dr_pvel[i] * lv * lv * 1.25f;
    d->env = 1.0f;
    d->env2 = 1.0f;
    switch (i) {
    case DR_BD:
        d->inc = (38.0f + t * 42.0f) / JP_SR;
        d->ph = 0.0f;
        d->k = dr_k(0.07f * jp_exp2(dc * 4.6f));   /* 70 ms .. 1.7 s: long ones are the 808 bass */
        d->drop = 1.0f; d->kdrop = dr_k(0.006f);
        d->k2 = dr_k(0.0012f);
        break;
    case DR_SD:
        d->inc = (150.0f + t * 160.0f) / JP_SR; d->inc2 = d->inc * 1.62f;
        d->k = dr_k(0.06f * dk);
        d->k2 = dr_k(0.07f * dk);
        d->lp = 0.0f;
        break;
    case DR_LT:
        d->inc = (70.0f + t * 130.0f) / JP_SR;
        d->k = dr_k(0.16f * dk);
        d->drop = 1.0f; d->kdrop = dr_k(0.03f);
        d->k2 = dr_k(0.004f);
        break;
    case DR_CP:
        d->f = 6.2831853f * (850.0f + t * 900.0f) / JP_SR;
        d->k = dr_k(0.003f);
        d->k2 = dr_k(0.12f * dk);
        d->env2 = 0.0f;
        break;
    case DR_RS:
        d->inc = (1450.0f + t * 500.0f) / JP_SR; d->inc2 = (440.0f + t * 120.0f) / JP_SR;
        d->ph = d->ph2 = 0.0f;
        d->k = dr_k(0.012f * dk);
        break;
    case DR_CB:
        d->inc = 540.0f * (0.8f + 0.4f * t) / JP_SR; d->inc2 = 800.0f * (0.8f + 0.4f * t) / JP_SR;
        d->f = 6.2831853f * 2640.0f * (0.8f + 0.4f * t) / JP_SR;
        d->k = dr_k(0.006f);
        d->k2 = dr_k(0.09f * dk);
        break;
    case DR_CH:
        d->k = dr_k(0.022f * dk);
        dr[DR_OH].k = dr_k(0.004f);             /* the closed hat chokes the open one */
        break;
    case DR_OH:
        d->k = dr_k(0.2f * dk);
        break;
    }
}

static void drums_render(uint32_t n)
{
    uint32_t sb, nsb = n / JP_SUB, i, j;
    float hpk = 0.62f;                          /* the hats' high-pass: ~7 kHz */
    for (j = 0; j < n; j++) dbufL[j] = dbufR[j] = 0.0f;
    for (sb = 0; sb < nsb; sb++) {
        float *L = dbufL + sb * JP_SUB, *R = dbufR + sb * JP_SUB;
        uint32_t hats;
        for (i = 0; i < DR_N; i++)
            if (dr_pend[i] && dr_pend[i] - 1u <= sb) { dr_pend[i] = 0; dr_start(i); }
        for (i = 0; i < DR_N; i++) {
            jp_drum_t *d = &dr[i];
            float pl = 1.0f - DR_PAN[i], pr = 1.0f + DR_PAN[i], g = d->vel, y = 0.0f, x;
            if (!d->on || i == DR_CH || i == DR_OH) continue;
            for (j = 0; j < JP_SUB; j++) {
                switch (i) {
                case DR_BD:
                    d->ph += d->inc * (1.0f + 2.4f * d->drop);
                    if (d->ph >= 1.0f) d->ph -= 1.0f;
                    y = jp_clip(1.25f * dr_sin(d->ph) * d->env) + 0.35f * dr_noise() * d->env2;
                    d->drop *= d->kdrop;
                    break;
                case DR_SD:
                    d->ph += d->inc; if (d->ph >= 1.0f) d->ph -= 1.0f;
                    d->ph2 += d->inc2; if (d->ph2 >= 1.0f) d->ph2 -= 1.0f;
                    x = dr_noise();
                    d->lp += (x - d->lp) * 0.18f;   /* the snares: noise above ~1.4 kHz */
                    y = (0.6f * dr_sin(d->ph) + 0.4f * dr_sin(d->ph2)) * d->env * 0.6f + (x - d->lp) * d->env2 * 0.55f;
                    break;
                case DR_LT:
                    d->ph += d->inc * (1.0f + 0.4f * d->drop);
                    if (d->ph >= 1.0f) d->ph -= 1.0f;
                    y = dr_sin(d->ph) * d->env * 0.9f + dr_noise() * d->env2 * 0.15f;
                    d->drop *= d->kdrop;
                    break;
                case DR_CP: {                   /* three quick bursts, then the room */
                    float hp;
                    if (d->t == 441u || d->t == 882u || d->t == 1323u) d->env = 1.0f;
                    if (d->t == 1323u) d->env2 = 0.75f;
                    x = dr_noise() * (d->env + d->env2);
                    d->lp += d->f * d->bp;
                    hp = x - d->lp - 0.55f * d->bp;
                    d->bp += d->f * hp;
                    y = d->bp * 0.9f;
                    break;
                }
                case DR_RS:
                    d->ph += d->inc; if (d->ph >= 1.0f) d->ph -= 1.0f;
                    d->ph2 += d->inc2; if (d->ph2 >= 1.0f) d->ph2 -= 1.0f;
                    y = (dr_sin(d->ph) + 0.6f * dr_sin(d->ph2)) * d->env * 0.6f;
                    break;
                case DR_CB: {
                    float hp;
                    d->ph += d->inc; if (d->ph >= 1.0f) d->ph -= 1.0f;
                    d->ph2 += d->inc2; if (d->ph2 >= 1.0f) d->ph2 -= 1.0f;
                    x = ((d->ph < 0.5f ? 1.0f : -1.0f) + (d->ph2 < 0.5f ? 1.0f : -1.0f)) * (0.6f * d->env + 0.4f * d->env2);
                    d->lp += d->f * d->bp;
                    hp = x - d->lp - 0.7f * d->bp;
                    d->bp += d->f * hp;
                    y = d->bp * 0.45f;
                    break;
                }
                }
                d->env *= d->k;
                d->env2 *= d->k2;
                d->t++;
                y *= g;
                L[j] += y * pl;
                R[j] += y * pr;
            }
            if (d->env < 0.0002f && d->env2 < 0.0002f) d->on = 0;
        }
        /* the hats: six square waves, high-passed twice, under the closed and the open envelope */
        hats = dr[DR_CH].on | dr[DR_OH].on;
        if (hats) {
            jp_drum_t *c = &dr[DR_CH], *o = &dr[DR_OH];
            float ht = (float)jp_dk[0][DR_CH] * (1.0f / 127.0f), hm = 0.85f + 0.35f * ht, inc[6];
            for (i = 0; i < 6; i++) inc[i] = HAT_F[i] * hm / JP_SR;
            for (j = 0; j < JP_SUB; j++) {
                float m = 0.0f, h1, h2, yc, yo;
                for (i = 0; i < 6; i++) {
                    hat_ph[i] += inc[i];
                    if (hat_ph[i] >= 1.0f) hat_ph[i] -= 1.0f;
                    m += hat_ph[i] < 0.5f ? 1.0f : -1.0f;
                }
                m = m * (1.0f / 6.0f) + 0.15f * dr_noise();
                h1 = hpk * (hat_hp1 + m - hat_hp1x); hat_hp1x = m; hat_hp1 = h1;
                h2 = hpk * (hat_hp2 + h1 - hat_hp2x); hat_hp2x = h1; hat_hp2 = h2;
                yc = c->on ? h2 * c->env * c->vel : 0.0f;
                yo = o->on ? h2 * o->env * o->vel : 0.0f;
                c->env *= c->k; o->env *= o->k;
                L[j] += yc * (1.0f - DR_PAN[DR_CH]) + yo * (1.0f - DR_PAN[DR_OH]);
                R[j] += yc * (1.0f + DR_PAN[DR_CH]) + yo * (1.0f + DR_PAN[DR_OH]);
            }
            if (c->env < 0.0002f) c->on = 0;
            if (o->env < 0.0002f) o->on = 0;
        }
    }
}

/* one step of the pattern, from the clock */
static void drums_step(uint32_t step, uint32_t sb)
{
    uint32_t pat = jp_g[G_PATTERN] % DR_PATTERNS, i, bit = 1u << (step & 15u);
    float acc = (float)jp_g[G_ACCENT] * (1.0f / 127.0f);
    float vel = (jp_pat[pat][DR_AC] & bit) ? 1.0f : 1.0f - 0.45f * acc;
    for (i = 0; i < DR_N; i++)
        if (jp_pat[pat][i] & bit) dr_trigger(i, vel, sb);
    jp_dr_step_shown = (uint8_t)(step & 15u);
}

/* the factory patterns and kit (main loop, at start) */
static const char *const DR_FACTORY[DR_PATTERNS][DR_ROWS] = {
    /*  BD                  SD                  LT                  CP                  RS                  CB
        CH                  OH                  AC */
    {"x...x...x...x...", "................", "................", "....x.......x...", "................", "................",
     "xx.xxx.xxx.xxx.x", "..x...x...x...x.", "x...x...x...x..."},                               /* 1 ITALO */
    {"x...x...x...x...", "....x.......x...", "................", "................", "................", "................",
     "x...x...x...x...", "..x...x...x...x.", "....x.......x..."},                               /* 2 DISCO */
    {"x.....x...x.....", "....x.......x...", "................", "....x.......x...", "...x..........x.", "..........x...x.",
     "x.x.x.x.x.x.x.x.", "................", "x.....x.....x..."},                               /* 3 ELECTRO */
    {"x...x...x...x...", "................", "................", "....x.......x...", "......x.......x.", "................",
     "...x...x...x...x", "..x...x...x...x.", "x...x...x...x..."},                               /* 4 HOUSE */
    {"x..x......x.....", "....x.......x...", "................", "................", "................", "................",
     "x.x.x.x.x.x.x.x.", "..............x.", "....x.......x..."},                               /* 5 HIP HOP */
    {"x......x..x.....", "....x.......x...", "............x.x.", "................", "................", "...x.......x....",
     "xxxxxxxxxxxxxxxx", "................", "x...x...x...x..."},                               /* 6 MIAMI */
    {"x...x...x...x...", "................", "x..x..x...x..x..", "................", "..x.....x.x.....", "x.....x.....x...",
     "..x...x...x...x.", "................", "x.......x......."},                               /* 7 CONGA */
    {"x...x...x...x...", "................", "................", "................", "................", "................",
     "................", "................", "................"},                               /* 8 a start */
};
static const uint8_t DR_KIT0[3][DR_N] = {
    {32, 60, 50, 60, 64, 64, 64, 64},           /* tune */
    {64, 50, 60, 50, 50, 50, 50, 60},           /* decay */
    {118, 96, 92, 98, 80, 74, 84, 80},          /* level */
};

static void drums_defaults(void)
{
    uint32_t p, r, s;
    for (p = 0; p < DR_PATTERNS; p++)
        for (r = 0; r < DR_ROWS; r++) {
            uint16_t m = 0;
            for (s = 0; s < 16u; s++) if (DR_FACTORY[p][r][s] == 'x') m |= (uint16_t)(1u << s);
            jp_pat[p][r] = m;
        }
    for (r = 0; r < 3u; r++) for (s = 0; s < DR_N; s++) jp_dk[r][s] = DR_KIT0[r][s];
}

#endif
