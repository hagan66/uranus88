/* SPDX-License-Identifier: GPL-3.0-only */
/* URANUS 88: the sound engine. Eight voices of two VCOs (cross mod, sync, PWM), a non-resonant HPF, a resonant
 * 4-pole low-pass with a 12 dB tap, two ADSRs, one LFO with a per-voice delay, a little analog drift per voice,
 * POLY1 / POLY2 / UNISON / SOLO key assign with portamento, an arpeggiator, a 16-step sequencer and a stereo
 * BBD-style chorus.
 *
 * Single-precision floats (the chip's FPU: -mcpu=r3 -mfprev1), every literal with an f. Everything that touches a
 * voice runs in the render (the audio interrupt): the main loop only queues key events and writes the patch's
 * values, which the render reads once per block. Voices 4..7 render on the second core when the host lends it.
 * Control (envelopes, LFO, pitch, the filter's coefficients) is computed every JP_SUB frames, the level ramps
 * across them. */
#ifndef JP_ENGINE_H
#define JP_ENGINE_H
#include <stdint.h>
#include "jp_params.h"

#define JP_SR      44100.0f
#define JP_SUB     32u                          /* frames per control step (0.7 ms) */
#define JP_SUBT    (32.0f / 44100.0f)           /* its length in seconds */
#define JP_NV      8u
#define JP_MAXN    512u                         /* the longest render() block handled in one go */
#define JP_MAXSB   (JP_MAXN / JP_SUB)

/* ------------------------------------------------------------------------------------------------ maths */
typedef union { float f; uint32_t u; int32_t i; } jp_fu_t;

static inline float jp_exp2(float x)            /* 2^x, ~1e-5 relative, x in -60..60 */
{
    jp_fu_t r;
    int32_t i;
    float f, p;
    if (x < -60.0f) x = -60.0f;
    if (x > 60.0f) x = 60.0f;
    i = (int32_t)x;
    if ((float)i > x) i--;
    f = x - (float)i;
    p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
    r.f = p;
    r.u += (uint32_t)i << 23;
    return r.f;
}

static inline float jp_tan(float x)             /* tan(x) for 0..1.35 (Pade [3/2]) */
{
    float x2 = x * x;
    return x * (15.0f - x2) / (15.0f - 6.0f * x2);
}

static inline float jp_clip(float x)            /* a soft clip: linear-ish to ~0.6, 1 at 1.5 */
{
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    return x - 0.148148f * x * x * x;
}

static inline float jp_absf(float x) { return x < 0.0f ? -x : x; }

static uint32_t jp_rng = 0x9E3779B9u;
static inline uint32_t jp_rand(void)
{
    uint32_t x = jp_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return jp_rng = x;
}
static inline float jp_randf(void) { return (float)(int32_t)jp_rand() * (1.0f / 2147483648.0f); }   /* -1..1 */

static inline float jp_blep(float t, float dt)  /* the polyBLEP residual at phase t (0..1), step dt */
{
    if (t < dt) {
        float x = t / dt;
        return x + x - x * x - 1.0f;
    }
    if (t > 1.0f - dt) {
        float x = (t - 1.0f) / dt;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}

/* ------------------------------------------------------------------------------------------------ state */
typedef struct {
    uint8_t sb, type, note, vel, from, layer;
    float uni, vlev;                            /* ON / LEGATO: the voice's detune (semitones) and level */
} jp_vev_t;
enum { EV_ON = 1, EV_OFF, EV_LEGATO };

typedef struct {
    /* logical state: what the events queued so far leave (the key assign's view) */
    uint8_t lnote, lgate;
    uint32_t age;
    /* events for this block, in order */
    uint8_t nev;
    jp_vev_t ev[6];
    /* the voice */
    uint8_t note, gate, st1, st2, layer, llayer;   /* its layer, as playing and as assigned */
    uint8_t lkey;                               /* the key that started it (DUO, CHORD, the subs: not its note) */
    float vlev;
    float vel, pitch, target, uni;
    float p1, p2, o2;
    uint32_t nz;
    float s1, s2, s3, s4, hpz;
    float e1, e2, amp, fade;
    float drift, dvel, cutoff_off, det_off, pan_l, pan_r;
} jp_voice_t;

/* what the render derives from the patch once per block (shared by both cores, read only while they run) */
typedef struct {
    float tune, bend, wheel;
    float lfo_vco, env_vco, xmod, pw, pwm_lfo, pwm_env, pwm_man;
    uint8_t w1, w2, dest, sync, slope, vcfenv, nv, pad;
    float r1, r2, fine;
    float m1, m2, hp_k, level;
    float cut, res_k, comp, envmod, vcflfo, keyflw, vcfvel;
    float a1t, d1t, s1, r1t, a2t, d2t, s2, r2t, envkf;
    float ka1, kd1, kr1, ka2, kd2, kr2;         /* the envelopes' coefficients (no key follow), once per block */
    float trem, vcavel, glide_k, analog, delay_k, spread;
    float lfo[JP_MAXSB + 1];                    /* the LFO at each control step */
} jp_ctl_t;

static jp_voice_t jv[JP_NV];
static jp_ctl_t jcl[2];                         /* UPPER, LOWER */
static float bufL[2][JP_MAXN], bufR[2][JP_MAXN];

/* the patch and the global values (written by the main loop) */
static volatile uint8_t jp_p[NP];
static volatile uint8_t jp_g[NG];

/* ------------------------------------------------------------------------------------------------ voices */
static float env_k(float t, float tau_div)      /* a one-pole coefficient per control step for a time t */
{
    float tau = t / tau_div;
    if (tau < 0.00005f) return 1.0f;
    return 1.0f - jp_exp2(-1.442695f * JP_SUBT / tau);
}

static inline float par_time(uint8_t v)         /* 0..127 -> 1 ms .. 12 s */
{
    return 0.001f * jp_exp2((float)v * 0.10669f);
}

static void voice_event(jp_voice_t *v, const jp_vev_t *e)
{
    const jp_ctl_t *c;
    if (e->type == EV_OFF) {
        if (v->note == e->note || !v->gate) {
            v->gate = 0;
            if (v->st1) v->st1 = 4;
            if (v->st2) v->st2 = 4;
        }
        return;
    }
    if (e->type == EV_ON) v->layer = e->layer;  /* (a voice keeps its layer for the note's whole life) */
    v->uni = e->uni;
    v->vlev = e->vlev;
    c = &jcl[v->layer];
    v->target = (float)e->note;
    if (e->type == EV_LEGATO && v->gate) {
        if (c->glide_k >= 1.0f) v->pitch = v->target;
        v->note = e->note;
        return;
    }
    v->pitch = (c->glide_k < 1.0f && e->from < 128u) ? (float)e->from : v->target;
    v->note = e->note;
    v->gate = 1;
    v->vel = (float)e->vel * (1.0f / 127.0f);
    v->st1 = v->st2 = 1;                        /* from the level they are at, as an analog envelope */
    if (c->delay_k < 1.0f) v->fade = 0.0f; else v->fade = 1.0f;
    if (v->e2 < 0.001f) {                       /* a fresh start: the oscillators where an analog voice might be */
        v->p1 = (float)(jp_rand() >> 9) * (1.0f / 8388608.0f);
        v->p2 = (float)(jp_rand() >> 9) * (1.0f / 8388608.0f);
    }
}

/* one voice over n frames into L/R (n a multiple of JP_SUB, sb0 = the first control step's index) */
static void voice_render(jp_voice_t *v, float *L, float *R, uint32_t n)
{
    const float inc0 = 440.0f / JP_SR;
    uint32_t sb, nsb = n / JP_SUB, ei = 0;
    float p1 = v->p1, p2 = v->p2, o2 = v->o2;
    float s1 = v->s1, s2 = v->s2, s3 = v->s3, s4 = v->s4, hpz = v->hpz;
    float amp = v->amp;
    uint32_t nz = v->nz | 1u;
    const jp_ctl_t *c = &jcl[v->layer];

    for (sb = 0; sb < nsb; sb++) {
        float lfo, kf, a1, d1, r1, a2, d2, r2, semi, n1, n2, inc1, inc2, pw, oct, fc, g, G, b, G2, G3, G4;
        float k, inv, amp_to, damp, comp, tmp;
        uint32_t i;
        while (ei < v->nev && v->ev[ei].sb <= sb)
            voice_event(v, &v->ev[ei++]);
        c = &jcl[v->layer];
        if (!v->st2 && amp < 0.00001f) {       /* silent: skip the rest of this block's audio */
            amp = 0.0f;
            continue;
        }
        lfo = c->lfo[sb];

        /* envelopes (key follow shortens them up the keyboard) */
        if (c->envkf > 0.0f) {                  /* key follow: this note's own coefficients */
            kf = jp_exp2(-c->envkf * ((float)v->note - 60.0f) * (1.0f / 24.0f));
            a1 = env_k(c->a1t * kf, 1.466f); d1 = env_k(c->d1t * kf, 4.6f); r1 = env_k(c->r1t * kf, 4.6f);
            a2 = env_k(c->a2t * kf, 1.466f); d2 = env_k(c->d2t * kf, 4.6f); r2 = env_k(c->r2t * kf, 4.6f);
        } else {
            a1 = c->ka1; d1 = c->kd1; r1 = c->kr1; a2 = c->ka2; d2 = c->kd2; r2 = c->kr2;
        }
        if (v->st1 == 1) { v->e1 += (1.3f - v->e1) * a1; if (v->e1 >= 1.0f) { v->e1 = 1.0f; v->st1 = 2; } }
        else if (v->st1 == 2) { v->e1 += (c->s1 - v->e1) * d1; }
        else if (v->st1 == 4) { v->e1 -= v->e1 * r1; if (v->e1 < 0.0003f) { v->e1 = 0.0f; v->st1 = 0; } }
        if (v->st2 == 1) { v->e2 += (1.3f - v->e2) * a2; if (v->e2 >= 1.0f) { v->e2 = 1.0f; v->st2 = 2; } }
        else if (v->st2 == 2) { v->e2 += (c->s2 - v->e2) * d2; }
        else if (v->st2 == 4) { v->e2 -= v->e2 * r2; if (v->e2 < 0.0003f) { v->e2 = 0.0f; v->st2 = 0; } }

        /* LFO delay, glide, drift */
        if (v->fade < 1.0f) { v->fade += c->delay_k; if (v->fade > 1.0f) v->fade = 1.0f; }
        v->pitch += (v->target - v->pitch) * c->glide_k;
        /* slow drift: a new random target about every 2 s, glided to over ~0.8 s (rate-independent) */
        if (jp_randf() < JP_SUBT * 1.0f - 1.0f) v->dvel = jp_randf();
        v->drift += (v->dvel - v->drift) * (JP_SUBT * 1.25f);

        /* pitch */
        semi = v->pitch + c->tune + c->bend + v->uni + v->drift * c->analog * 0.03f;
        tmp = lfo * (v->fade * c->lfo_vco + c->wheel) + v->e1 * c->env_vco;
        n1 = semi + c->r1 + (c->dest != MD_VCO2 ? tmp : 0.0f);
        n2 = semi + c->r2 + c->fine + v->det_off * c->analog + (c->dest != MD_VCO1 ? tmp : 0.0f);
        inc1 = inc0 * jp_exp2((n1 - 69.0f) * (1.0f / 12.0f));
        inc2 = inc0 * jp_exp2((n2 - 69.0f) * (1.0f / 12.0f));
        if (inc1 > 0.45f) inc1 = 0.45f;
        if (inc2 > 0.45f) inc2 = 0.45f;
        pw = c->pwm_man + c->pwm_lfo * (0.5f + 0.5f * lfo) * v->fade + c->pwm_env * v->e1;
        if (pw > 0.48f) pw = 0.48f;
        pw = 0.5f - pw;                         /* 0.5 (square) .. 0.02 */

        /* VCF */
        oct = c->cut + c->envmod * (c->vcfenv ? v->e2 : v->e1) + c->vcflfo * lfo * v->fade
            + c->keyflw * ((float)v->note - 60.0f) * (1.0f / 12.0f) + c->vcfvel * (v->vel - 0.6f)
            + v->cutoff_off * c->analog;
        fc = 16.0f * jp_exp2(oct);
        if (fc > 0.42f * JP_SR) fc = 0.42f * JP_SR;
        if (fc < 8.0f) fc = 8.0f;
        g = jp_tan(3.14159265f * fc / JP_SR);
        b = 1.0f / (1.0f + g);
        G = g * b;
        G2 = G * G; G3 = G2 * G; G4 = G3 * G;
        k = c->res_k;
        inv = 1.0f / (1.0f + k * G4);
        comp = 1.0f + c->comp * k;

        /* VCA */
        amp_to = v->e2 * c->level * v->vlev * (1.0f - c->vcavel * (1.0f - v->vel))
               * (1.0f - c->trem * (0.5f + 0.5f * lfo) * v->fade);
        damp = (amp_to - amp) * (1.0f / (float)JP_SUB);

        for (i = 0; i < JP_SUB; i++) {
            float o1, x, S, u, y, di1, w;
            uint32_t j = sb * JP_SUB + i, wrap = 0;
            di1 = inc1 + c->xmod * inc1 * o2;
            if (di1 < 0.0f) di1 = 0.0f;
            if (di1 > 0.45f) di1 = 0.45f;
            p1 += di1;
            if (p1 >= 1.0f) { p1 -= 1.0f; wrap = 1; }
            switch (c->w1) {
            case W1_SAW: o1 = 2.0f * p1 - 1.0f - jp_blep(p1, di1); break;
            case W1_PULSE:
            case W1_SQR: {
                float q = c->w1 == W1_SQR ? 0.5f : pw, t2 = p1 - q;
                if (t2 < 0.0f) t2 += 1.0f;
                o1 = (p1 < q ? 1.0f : -1.0f) + jp_blep(p1, di1) - jp_blep(t2, di1);
                break;
            }
            default: o1 = 4.0f * jp_absf(p1 - 0.5f) - 1.0f; break;
            }
            if (c->sync && wrap) {
                p2 = di1 > 0.0f ? p1 / di1 * inc2 : 0.0f;
            } else {
                p2 += inc2;
                if (p2 >= 1.0f) p2 -= 1.0f;
            }
            switch (c->w2) {
            case W2_SAW: o2 = 2.0f * p2 - 1.0f - jp_blep(p2, inc2); break;
            case W2_PULSE: {
                float t2 = p2 - pw;
                if (t2 < 0.0f) t2 += 1.0f;
                o2 = (p2 < pw ? 1.0f : -1.0f) + jp_blep(p2, inc2) - jp_blep(t2, inc2);
                break;
            }
            case W2_NOISE:
                nz ^= nz << 13; nz ^= nz >> 17; nz ^= nz << 5;
                o2 = (float)(int32_t)nz * (1.0f / 2147483648.0f);
                break;
            default: w = 2.0f * p2 - 1.0f; o2 = 4.0f * w * (1.0f - jp_absf(w)); break;
            }
            x = o1 * c->m1 + o2 * c->m2;
            if (c->hp_k > 0.0f) {               /* the HPF: what is left above its corner */
                hpz += (x - hpz) * c->hp_k;
                x -= hpz;
            }

            /* the ladder: solved for its feedback (zero delay), the input soft clipped */
            S = b * (G3 * s1 + G2 * s2 + G * s3 + s4);
            u = jp_clip((x * comp - k * S) * inv);
            w = (u - s1) * G; y = w + s1; s1 = y + w;
            w = (y - s2) * G; y = w + s2; s2 = y + w;
            if (c->slope) {                     /* 12 dB: the second stage's output */
                float y2 = y;
                w = (y - s3) * G; y = w + s3; s3 = y + w;
                w = (y - s4) * G; y = w + s4; s4 = y + w;
                y = y2;
            } else {
                w = (y - s3) * G; y = w + s3; s3 = y + w;
                w = (y - s4) * G; y = w + s4; s4 = y + w;
            }
            amp += damp;
            y *= amp;
            L[j] += y * v->pan_l;
            R[j] += y * v->pan_r;
        }
        amp = amp_to;
    }
    while (ei < v->nev)                         /* (events past the block: none, by construction) */
        voice_event(v, &v->ev[ei++]);
    v->nev = 0;
    /* a denormal guard on the states */
    if (jp_absf(s1) < 1e-15f) s1 = 0.0f;
    if (jp_absf(s2) < 1e-15f) s2 = 0.0f;
    if (jp_absf(s3) < 1e-15f) s3 = 0.0f;
    if (jp_absf(s4) < 1e-15f) s4 = 0.0f;
    v->p1 = p1; v->p2 = p2; v->o2 = o2;
    v->s1 = s1; v->s2 = s2; v->s3 = s3; v->s4 = s4; v->hpz = hpz;
    v->amp = amp; v->nz = nz;
}

/* ------------------------------------------------------------------------------------------------ key assign */
/* Two layers, as the original's panel had them: UPPER (the patch on the panel) and LOWER (G_LOWER). WHOLE gives
 * every voice to UPPER; SPLIT and DUAL give half the voices to each (UPPER on core 0, LOWER on core 1). A layer
 * assigns its own voices by its own patch's KEY ASSIGN. */
static const uint8_t ORDER[JP_NV] = {0, 4, 1, 5, 2, 6, 3, 7};   /* alternating cores */
typedef struct {
    uint32_t rr;
    uint8_t stack[16], nstack, last, mode, nv;
    uint8_t vl[JP_NV];                          /* its voices */
    uint8_t held[16], sus[16];                  /* bitmaps of 128 notes */
} jp_layer_t;
static jp_layer_t LA[2] = {{0, {0}, 0, 255, 255, 8, {0}, {0}, {0}}, {0, {0}, 0, 255, 255, 0, {0}, {0}, {0}}};
static uint32_t as_age;
static uint8_t as_sustain, as_km = 255, as_split;
static int8_t as_lowoff;
static volatile uint8_t jp_p2[NP];              /* the LOWER patch (the main loop copies it) */

static const volatile uint8_t *lay_p(uint32_t L) { return L ? jp_p2 : jp_p; }

static void vev2(jp_voice_t *v, uint32_t L, uint8_t sb, uint8_t type, uint8_t note, uint8_t vel, uint8_t key,
                 float uni, float vlev)
{
    jp_vev_t *e = &v->ev[v->nev >= 6 ? 5 : v->nev];  /* full: the last one gives way */
    e->sb = sb; e->type = type; e->note = note; e->vel = vel; e->from = LA[L].last; e->layer = (uint8_t)L;
    e->uni = uni; e->vlev = vlev;
    if (v->nev < 6) v->nev++;
    if (type == EV_OFF) v->lgate = 0;
    else { v->lgate = 1; v->lnote = note; v->lkey = key; v->llayer = (uint8_t)L; v->age = ++as_age; }
}
static void vev(jp_voice_t *v, uint32_t L, uint8_t sb, uint8_t type, uint8_t note, uint8_t vel)
{
    vev2(v, L, sb, type, note, vel, note, 0.0f, 1.0f);
}

/* VOICE DEPTH (P_VDEPTH), as on a prologue: POLY two voices a key (DUO), the second louder and wider as it
 * turns; SOLO sub oscillators an octave and two below; UNISON the detune; CHORD the chord a key plays */
static const int8_t CHORDS_IV[14][4] = {
    {0, 7, 12, -1}, {0, 2, 7, -1}, {0, 3, 7, -1}, {0, 4, 7, -1}, {0, 5, 7, -1}, {0, 3, 7, 10}, {0, 4, 7, 10},
    {0, 5, 7, 10}, {0, 4, 7, 11}, {0, 4, 8, -1}, {0, 3, 6, -1}, {0, 3, 6, 10}, {0, 3, 7, 11}, {0, 4, 6, 11}};
static float la_depth(uint32_t L) { return (float)lay_p(L)[P_VDEPTH] * (1.0f / 127.0f); }

static void la_mono_play(uint32_t L, uint8_t sb, uint8_t note, uint8_t vel)
{
    jp_layer_t *a = &LA[L];
    float d = la_depth(L);
    uint32_t i, nv;
    if (a->mode == AS_SOLO) {                   /* the note, then subs: -1 octave, -2 octaves */
        nv = 1u + (d > 0.0f ? 1u : 0u) + (d > 0.5f ? 1u : 0u);
        if (nv > a->nv) nv = a->nv;
        for (i = 0; i < nv; i++) {
            jp_voice_t *v = &jv[a->vl[i]];
            int32_t n = (int32_t)note - 12 * (int32_t)i;
            float lv = i == 0u ? 1.0f : i == 1u ? (d * 2.0f > 1.0f ? 1.0f : d * 2.0f) : d * 2.0f - 1.0f;
            if (n < 0) n += 12;
            vev2(v, L, sb, v->lgate ? EV_LEGATO : EV_ON, (uint8_t)n, vel, note, 0.0f, lv);
        }
        for (; i < a->nv; i++)                  /* subs turned down: their voices let go */
            if (jv[a->vl[i]].lgate) vev(&jv[a->vl[i]], L, sb, EV_OFF, jv[a->vl[i]].lnote, 0);
        return;
    }
    nv = a->nv;
    for (i = 0; i < nv; i++) {                  /* UNISON: every voice, spread up to +-50 cents */
        jp_voice_t *v = &jv[a->vl[i]];
        float u = nv > 1u ? d * 0.5f * ((float)i - 0.5f * (float)(nv - 1u)) / (0.5f * (float)(nv - 1u)) : 0.0f;
        vev2(v, L, sb, v->lgate ? EV_LEGATO : EV_ON, note, vel, note, u, 1.0f);
    }
}

static uint32_t la_pick(uint32_t L)             /* a voice for a new note: silent, else releasing, else the oldest */
{
    jp_layer_t *a = &LA[L];
    uint32_t i, pick = 0, nv = a->nv, best = 0xFFFFFFFFu;
    for (i = 0; i < nv; i++) {
        uint32_t idx = a->mode == AS_POLY1 ? (a->rr + 1u + i) % nv : i;
        jp_voice_t *v = &jv[a->vl[idx]];
        uint32_t score;
        if (v->lgate) score = 0x80000000u + (v->age & 0x7FFFFFFFu);   /* gated: the oldest, last of all */
        else if (!v->st2 && !v->nev) score = i;                        /* silent */
        else score = 0x1000u + i;                                     /* releasing */
        if (score < best) { best = score; pick = idx; }
    }
    if (a->mode == AS_POLY1) a->rr = pick;
    return a->vl[pick];
}

static void la_on(uint32_t L, uint8_t note, uint8_t vel, uint8_t sb)
{
    jp_layer_t *a = &LA[L];
    uint32_t i, nv = a->nv;
    float d = la_depth(L);
    if (note > 127u || !nv) return;
    a->held[note >> 3] |= (uint8_t)(1u << (note & 7u));
    a->sus[note >> 3] &= (uint8_t)~(1u << (note & 7u));
    if (a->mode == AS_UNISON || a->mode == AS_SOLO) {
        for (i = 0; i < a->nstack; i++)
            if (a->stack[i] == note) break;
        if (i < a->nstack) { for (; i + 1u < a->nstack; i++) a->stack[i] = a->stack[i + 1u]; a->nstack--; }
        if (a->nstack >= 16u) { for (i = 0; i < 15u; i++) a->stack[i] = a->stack[i + 1u]; a->nstack = 15; }
        a->stack[a->nstack++] = note;
        la_mono_play(L, sb, note, vel);
        a->last = note;
        return;
    }
    if (a->mode != AS_CHORD && d <= 0.0f)       /* the same note again: its own voice, as before */
        for (i = 0; i < nv; i++) {
            jp_voice_t *v = &jv[a->vl[i]];
            if (v->lnote == note && v->lkey == note && (v->lgate || v->e2 > 0.0f)) {
                vev(v, L, sb, EV_ON, note, vel);
                a->last = note;
                return;
            }
        }
    for (i = 0; i < JP_NV; i++)                 /* the same key again (DUO, CHORD): let its voices go first */
        if (jv[i].lgate && jv[i].lkey == note && jv[i].llayer == L) vev(&jv[i], L, sb, EV_OFF, jv[i].lnote, 0);
    if (a->mode == AS_CHORD) {                  /* the chord VOICE DEPTH chooses, a voice a note */
        const int8_t *iv = CHORDS_IV[(uint32_t)(d * 13.99f)];
        for (i = 0; i < 4u && iv[i] >= 0; i++) {
            int32_t n = (int32_t)note + iv[i];
            if (n < 128) vev2(&jv[la_pick(L)], L, sb, EV_ON, (uint8_t)n, vel, note, 0.0f, 0.8f);
        }
    } else {
        vev2(&jv[la_pick(L)], L, sb, EV_ON, note, vel, note, 0.0f, 1.0f);
        if (d > 0.0f && nv > 1u)                /* DUO: a second voice, detuned up to 25 cents, louder as it turns */
            vev2(&jv[la_pick(L)], L, sb, EV_ON, note, vel, note, 0.04f + d * 0.21f, 0.35f + d * 0.65f);
    }
    a->last = note;
}

static void la_off(uint32_t L, uint8_t note, uint8_t sb)
{
    jp_layer_t *a = &LA[L];
    uint32_t i;
    if (note > 127u) return;
    a->held[note >> 3] &= (uint8_t)~(1u << (note & 7u));
    if (as_sustain) { a->sus[note >> 3] |= (uint8_t)(1u << (note & 7u)); return; }
    if (a->mode == AS_UNISON || a->mode == AS_SOLO) {
        uint32_t top;
        for (i = 0; i < a->nstack; i++)
            if (a->stack[i] == note) break;
        if (i == a->nstack) return;
        top = i + 1u == a->nstack;
        for (; i + 1u < a->nstack; i++) a->stack[i] = a->stack[i + 1u];
        a->nstack--;
        if (!a->nstack) {
            for (i = 0; i < JP_NV; i++)
                if (jv[i].lgate && jv[i].llayer == L) vev(&jv[i], L, sb, EV_OFF, jv[i].lnote, 0);
        } else if (top) {
            la_mono_play(L, sb, a->stack[a->nstack - 1u], 100);
            a->last = a->stack[a->nstack - 1u];
        }
        return;
    }
    for (i = 0; i < JP_NV; i++)                 /* every voice the key started (DUO, CHORD: more than one) */
        if (jv[i].lgate && jv[i].lkey == note && jv[i].llayer == L)
            vev(&jv[i], L, sb, EV_OFF, jv[i].lnote, 0);
}

static void as_all_off(uint8_t sb)
{
    uint32_t i, L;
    for (i = 0; i < JP_NV; i++)
        if (jv[i].lgate) vev(&jv[i], jv[i].llayer, sb, EV_OFF, jv[i].lnote, 0);
    for (L = 0; L < 2u; L++) {
        LA[L].nstack = 0;
        for (i = 0; i < 16u; i++) LA[L].held[i] = LA[L].sus[i] = 0;
    }
}

/* what the keys, the arpeggiator and the sequencer's first track play: routed by the key mode */
static void as_on(uint8_t note, uint8_t vel, uint8_t sb)
{
    int32_t lo = (int32_t)note + (int32_t)as_lowoff;
    if (as_km == KM_SPLIT) {
        if (note < as_split) { if (lo >= 0 && lo < 128) la_on(1, (uint8_t)lo, vel, sb); }
        else la_on(0, note, vel, sb);
    } else {
        la_on(0, note, vel, sb);
        if (as_km == KM_DUAL && lo >= 0 && lo < 128) la_on(1, (uint8_t)lo, vel, sb);
    }
}

static void as_off(uint8_t note, uint8_t sb)
{
    int32_t lo = (int32_t)note + (int32_t)as_lowoff;
    if (as_km == KM_SPLIT) {
        if (note < as_split) { if (lo >= 0 && lo < 128) la_off(1, (uint8_t)lo, sb); }
        else la_off(0, note, sb);
    } else {
        la_off(0, note, sb);
        if (as_km == KM_DUAL && lo >= 0 && lo < 128) la_off(1, (uint8_t)lo, sb);
    }
}

/* the sequencer's second track: the LOWER layer (WHOLE: the only one), its octave too */
static void as_on_lower(uint8_t note, uint8_t vel, uint8_t sb)
{
    int32_t lo = (int32_t)note + (int32_t)as_lowoff;
    if (as_km == KM_WHOLE) la_on(0, note, vel, sb);
    else if (lo >= 0 && lo < 128) la_on(1, (uint8_t)lo, vel, sb);
}
static void as_off_lower(uint8_t note, uint8_t sb)
{
    int32_t lo = (int32_t)note + (int32_t)as_lowoff;
    if (as_km == KM_WHOLE) la_off(0, note, sb);
    else if (lo >= 0 && lo < 128) la_off(1, (uint8_t)lo, sb);
}

static void as_sustain_set(uint8_t on, uint8_t sb)
{
    uint32_t n, L;
    as_sustain = on;
    if (on) return;
    for (L = 0; L < 2u; L++)
        for (n = 0; n < 128u; n++)
            if (LA[L].sus[n >> 3] >> (n & 7u) & 1u) {
                LA[L].sus[n >> 3] &= (uint8_t)~(1u << (n & 7u));
                if (!(LA[L].held[n >> 3] >> (n & 7u) & 1u)) la_off(L, (uint8_t)n, sb);
            }
}

/* ------------------------------------------------------------------------------------------------ key events */
#define KQ 64u
static volatile struct { uint8_t type, a, b; } kq[KQ];
static volatile uint32_t kq_w, kq_r;
enum { KE_ON = 1, KE_OFF, KE_SUSTAIN, KE_PANIC, KE_BEND, KE_WHEEL, KE_DRUM };

static void key_push(uint8_t type, uint8_t a, uint8_t b)    /* main loop only */
{
    uint32_t w = kq_w;
    if (w - kq_r >= KQ) return;
    kq[w % KQ].type = type; kq[w % KQ].a = a; kq[w % KQ].b = b;
    kq_w = w + 1u;
}

#include "jp_drums.h"

/* ------------------------------------------------------------------------------------------------ arp and sequencer */
/* jp_seq_play is the transport (PLAY): it runs the drum machine, the sequencer or both (G_PLAYMODE) on one clock,
 * and the arpeggiator follows that clock while it runs */
static volatile uint8_t jp_arp_on, jp_seq_play, jp_seq_rec;
static uint8_t eng_seq_on, eng_dr_on, tp_was, ck_step;
static volatile uint8_t jp_seq[JP_STEPS], jp_seq2[JP_STEPS];   /* track 1 (the keys' way), track 2 (LOWER) */
static volatile uint8_t jp_seq_pos_shown = 255;
static volatile int8_t jp_transpose;
static uint8_t ar_list[16], ar_n, ar_phys, ar_note = 255, ar_pos, ar_vel = 100, ar_was;
static uint8_t sq_n[2] = {255, 255}, sq_tie[2], sq_step, sq_was;
static float ck_pos, ck_len = 1.0f, ck_gate = 1.0f;
static uint8_t ck_gated, ck_restart;
static float bend_v, wheel_v;

static void ar_add(uint8_t note)
{
    uint32_t i, j;
    if (jp_g[G_HOLD] && ar_phys == 0u) ar_n = 0;     /* a new chord after letting go */
    ar_phys++;
    for (i = 0; i < ar_n; i++) if (ar_list[i] == note) return;
    if (ar_n >= 16u) return;
    for (i = 0; i < ar_n && ar_list[i] < note; i++) ;
    for (j = ar_n; j > i; j--) ar_list[j] = ar_list[j - 1u];
    ar_list[i] = note;
    if (!ar_n && !jp_seq_play) ck_restart = 1;   /* (with the transport running, the arp joins its clock) */
    ar_n++;
}

static void ar_remove(uint8_t note)
{
    uint32_t i;
    if (ar_phys) ar_phys--;
    if (jp_g[G_HOLD]) return;
    for (i = 0; i < ar_n; i++) if (ar_list[i] == note) break;
    if (i == ar_n) return;
    for (; i + 1u < ar_n; i++) ar_list[i] = ar_list[i + 1u];
    ar_n--;
}

static uint8_t ar_next(void)
{
    uint32_t range = (uint32_t)jp_g[G_ARPRANGE] + 1u, L = ar_n * range, idx, per;
    int32_t n;
    if (!L) return 255;
    switch (jp_g[G_ARPMODE]) {
    case AR_DOWN: idx = L - 1u - ar_pos % L; break;
    case AR_UPDOWN:
        per = L > 1u ? 2u * L - 2u : 1u;
        idx = ar_pos % per;
        if (idx >= L) idx = per - idx;
        break;
    case AR_RANDOM: idx = jp_rand() % L; break;
    default: idx = ar_pos % L; break;
    }
    ar_pos++;
    n = (int32_t)ar_list[idx % ar_n] + 12 * (int32_t)(idx / ar_n);
    return (uint8_t)(n > 127 ? 127 : n);
}

static void clock_set(void)
{
    ck_gate = (0.1f + 0.9f * (float)jp_g[G_GATE] * (1.0f / 127.0f));
}

static void clock_step(uint8_t sb)
{
    float sw = (float)jp_g[G_SWING] * (0.33f / 127.0f);
    float base = JP_SR * 60.0f / (40.0f + (float)jp_g[G_TEMPO]) * 0.25f;   /* a sixteenth */
    ck_gated = 1;                               /* (nothing to let go of, unless a note starts below) */
    if (eng_dr_on) drums_step(ck_step, sb);
    if (eng_seq_on) {
        uint8_t len = (uint8_t)(jp_g[G_SEQLEN] + 1u), t;
        if (sq_step >= len) sq_step = 0;
        for (t = 0; t < 2u; t++) {
            const volatile uint8_t *q = t ? jp_seq2 : jp_seq;
            uint8_t s = q[sq_step];
            if (s != SEQ_TIE && sq_n[t] != 255u) { if (t) as_off_lower(sq_n[t], sb); else as_off(sq_n[t], sb); sq_n[t] = 255; }
            if (s < 128u) {
                int32_t n = (int32_t)s + jp_transpose;
                if (n < 0) n = 0;
                if (n > 127) n = 127;
                sq_n[t] = (uint8_t)n;
                if (t) as_on_lower(sq_n[t], 100, sb); else as_on(sq_n[t], 100, sb);
            }
            sq_tie[t] = q[(sq_step + 1u) % len] == SEQ_TIE;   /* tied on: no gate off */
            if (sq_n[t] != 255u && !sq_tie[t]) ck_gated = 0;
        }
        jp_seq_pos_shown = sq_step;
        sq_step = (uint8_t)((sq_step + 1u) % len);
    } else if (jp_arp_on) {
        if (ar_note != 255u) { as_off(ar_note, sb); ar_note = 255; }
        ar_note = ar_next();
        if (ar_note != 255u) { as_on(ar_note, ar_vel, sb); ck_gated = 0; }
    }
    ck_len = base * ((ck_step & 1u) ? 1.0f - sw : 1.0f + sw);   /* the swing: long, short */
    ck_step++;
}

static void clock_gate_off(uint8_t sb)
{
    if (eng_seq_on) {
        if (sq_n[0] != 255u && !sq_tie[0]) { as_off(sq_n[0], sb); sq_n[0] = 255; }
        if (sq_n[1] != 255u && !sq_tie[1]) { as_off_lower(sq_n[1], sb); sq_n[1] = 255; }
    } else if (ar_note != 255u) { as_off(ar_note, sb); ar_note = 255; }
}

/* ------------------------------------------------------------------------------------------------ chorus */
#define CH_N 1024u
static float ch_buf[CH_N];
static uint32_t ch_w;
static float ch_ph, ch_lpL, ch_lpR;

static void chorus(float *L, float *R, uint32_t n)
{
    static const float RATE[4] = {0.0f, 0.513f, 0.863f, 9.75f};
    static const float DEPTH[4] = {0.0f, 1.0f, 1.0f, 0.18f};
    uint8_t mode = jp_p[P_CHMODE];
    float rate, depth, mix, dry, base = 0.0035f * JP_SR, inc, a;
    uint32_t i;
    if (mode > 3u) mode = 3;
    if (mode == CH_OFF) {                       /* keep the line filled, so turning it on does not pop */
        for (i = 0; i < n; i++) { ch_buf[ch_w] = 0.5f * (L[i] + R[i]); ch_w = (ch_w + 1u) & (CH_N - 1u); }
        return;
    }
    rate = RATE[mode] * jp_exp2(((float)jp_p[P_CHRATE] - 64.0f) * (1.0f / 32.0f));
    depth = DEPTH[mode] * (float)jp_p[P_CHDEPTH] * (1.0f / 127.0f) * 0.0019f * JP_SR;
    mix = (float)jp_p[P_CHMIX] * (1.0f / 127.0f);
    dry = 1.0f - 0.35f * mix;
    inc = rate / JP_SR;
    a = 0.62f;                                  /* the BBD's darkness: a one-pole at ~7 kHz */
    for (i = 0; i < n; i++) {
        float tri, dl, dr, fl, fr, xl, xr, rl, rr;
        uint32_t il, ir;
        ch_buf[ch_w] = 0.5f * (L[i] + R[i]);
        ch_ph += inc;
        if (ch_ph >= 1.0f) ch_ph -= 1.0f;
        tri = 4.0f * jp_absf(ch_ph - 0.5f) - 1.0f;
        dl = base + depth * tri;
        dr = base - depth * tri;
        il = (uint32_t)dl; fl = dl - (float)il;
        ir = (uint32_t)dr; fr = dr - (float)ir;
        xl = ch_buf[(ch_w - il) & (CH_N - 1u)] * (1.0f - fl) + ch_buf[(ch_w - il - 1u) & (CH_N - 1u)] * fl;
        xr = ch_buf[(ch_w - ir) & (CH_N - 1u)] * (1.0f - fr) + ch_buf[(ch_w - ir - 1u) & (CH_N - 1u)] * fr;
        ch_lpL += (xl - ch_lpL) * a;
        ch_lpR += (xr - ch_lpR) * a;
        rl = L[i] * dry + ch_lpL * mix * 0.9f;
        rr = R[i] * dry + ch_lpR * mix * 0.9f;
        L[i] = rl; R[i] = rr;
        ch_w = (ch_w + 1u) & (CH_N - 1u);
    }
}

/* ------------------------------------------------------------------------------------------------ the block */
static float lf_ph[2], lf_val[2], lf_rnd[2];
static uint8_t eng_nv = 8;
static volatile uint8_t jp_nv_now = 8, jp_cpu = 0, jp_active = 0, jp_cores = 1;
static uint32_t eng_under;

static void derive(uint32_t L, uint32_t nsb)
{
    const volatile uint8_t *p = lay_p(L);
    jp_ctl_t *c = &jcl[L];
    static const float RANGE[6] = {-36.0f, -24.0f, -12.0f, 0.0f, 12.0f, 24.0f};
    float lr, x, inc;
    uint32_t i;
    c->tune = ((float)jp_g[G_TUNE] - 64.0f) * (1.0f / 64.0f) * 0.5f;
    c->bend = bend_v * (float)p[P_BEND];
    c->wheel = wheel_v * (float)p[P_WHEEL] * (1.5f / 127.0f);
    x = (float)p[P_LFOVCO] * (1.0f / 127.0f); c->lfo_vco = x * x * 12.0f;
    x = (float)p[P_ENVVCO] * (1.0f / 127.0f); c->env_vco = x * x * 36.0f;
    x = (float)p[P_XMOD] * (1.0f / 127.0f); c->xmod = x * x * 4.0f;
    c->w1 = p[P_V1WAVE] & 3u; c->w2 = p[P_V2WAVE] & 3u;
    c->dest = p[P_MODDEST] > 2u ? 1u : p[P_MODDEST];
    c->sync = p[P_SYNC] ? 1u : 0u;
    c->slope = p[P_SLOPE] ? 1u : 0u;
    c->vcfenv = p[P_VCFENV] ? 1u : 0u;
    x = (float)p[P_PW] * (0.46f / 127.0f);
    c->pwm_man = p[P_PWMSRC] == PWM_MAN ? x : 0.0f;
    c->pwm_lfo = p[P_PWMSRC] == PWM_LFO ? x : 0.0f;
    c->pwm_env = p[P_PWMSRC] == PWM_ENV1 ? x : 0.0f;
    c->r1 = RANGE[p[P_V1RANGE] > 5u ? 5u : p[P_V1RANGE]];
    c->r2 = RANGE[p[P_V2RANGE] > 5u ? 5u : p[P_V2RANGE]];
    c->fine = ((float)p[P_V2FINE] - 64.0f) * (1.0f / 64.0f);
    c->m1 = (float)p[P_MIX1] * (0.8f / 127.0f);
    c->m2 = (float)p[P_MIX2] * (0.8f / 127.0f);
    c->hp_k = p[P_HPF] ? 1.0f - jp_exp2(-1.442695f * 6.2831853f * 15.0f * jp_exp2((float)p[P_HPF] * (6.5f / 127.0f)) / JP_SR) : 0.0f;
    x = (float)p[P_LEVEL] * (1.0f / 127.0f); c->level = x * x * 0.42f;
    if (p[P_ASSIGN] == AS_SOLO) c->level *= 2.0f;        /* one voice as loud as a chord */
    else if (p[P_ASSIGN] == AS_UNISON) c->level *= 0.9f;
    c->cut = (float)p[P_CUTOFF] * (10.0f / 127.0f);
    x = (float)p[P_RES] * (1.0f / 127.0f); c->res_k = 4.15f * x * (0.45f + 0.55f * x);
    c->comp = 0.35f;
    c->envmod = (float)p[P_ENVMOD] * (9.0f / 127.0f);
    c->vcflfo = (float)p[P_VCFLFO] * (3.0f / 127.0f);
    c->keyflw = (float)p[P_KEYFLW] * (1.0f / 127.0f);
    c->vcfvel = (float)p[P_VCFVEL] * (3.0f / 127.0f);
    c->a1t = par_time(p[P_A1]); c->d1t = par_time(p[P_D1]); c->r1t = par_time(p[P_R1]);
    c->a2t = par_time(p[P_A2]); c->d2t = par_time(p[P_D2]); c->r2t = par_time(p[P_R2]);
    c->s1 = (float)p[P_S1] * (1.0f / 127.0f);
    c->s2 = (float)p[P_S2] * (1.0f / 127.0f);
    c->envkf = (float)p[P_ENVKF] * (1.0f / 127.0f);
    c->ka1 = env_k(c->a1t, 1.466f); c->kd1 = env_k(c->d1t, 4.6f); c->kr1 = env_k(c->r1t, 4.6f);
    c->ka2 = env_k(c->a2t, 1.466f); c->kd2 = env_k(c->d2t, 4.6f); c->kr2 = env_k(c->r2t, 4.6f);
    c->trem = (float)p[P_TREM] * (0.9f / 127.0f);
    c->vcavel = (float)p[P_VCAVEL] * (1.0f / 127.0f);
    c->glide_k = p[P_PORTA] ? env_k(0.004f * jp_exp2((float)p[P_PORTA] * (9.5f / 127.0f)), 2.3f) : 1.0f;
    c->analog = (float)p[P_ANALOG] * (1.0f / 127.0f);
    c->delay_k = p[P_LFODELAY] ? JP_SUBT / (0.02f * jp_exp2((float)p[P_LFODELAY] * (7.6f / 127.0f))) : 1.0f;
    c->spread = (float)jp_g[G_SPREAD] * (0.45f / 127.0f);

    /* the LFO, one value per control step */
    lr = 0.05f * jp_exp2((float)p[P_LFORATE] * (9.2f / 127.0f));
    inc = lr * JP_SUBT;
    for (i = 0; i < nsb; i++) {
        float v;
        lf_ph[L] += inc;
        if (lf_ph[L] >= 1.0f) { lf_ph[L] -= 1.0f; lf_rnd[L] = jp_randf(); }
        switch (p[P_LFOWAVE] & 3u) {
        case LW_SAW: v = 1.0f - 2.0f * lf_ph[L]; break;
        case LW_SQR: v = lf_ph[L] < 0.5f ? 1.0f : -1.0f; break;
        case LW_RND: v = lf_rnd[L]; break;
        default: v = 4.0f * jp_absf(lf_ph[L] - 0.5f) - 1.0f; break;
        }
        lf_val[L] += (v - lf_val[L]) * 0.35f;   /* a little smoothing: no clicks from the square */
        c->lfo[i] = lf_val[L];
    }
}

static void voices_core(uint32_t half, uint32_t n)
{
    uint32_t v, i;
    float *L = bufL[half], *R = bufR[half];
    for (i = 0; i < n; i++) L[i] = R[i] = 0.0f;
    for (v = half * 4u; v < half * 4u + 4u; v++)
        voice_render(&jv[v], L, R, n);
}

static void eng_init(void)
{
    uint32_t i;
    for (i = 0; i < JP_NV; i++) {
        jv[i].lnote = jv[i].note = 255;
        jv[i].nz = 0x1234567u * (i + 1u);
        jv[i].cutoff_off = jp_randf() * 0.18f;
        jv[i].det_off = jp_randf() * 0.07f;
        jv[i].drift = jv[i].dvel = jp_randf() * 0.5f;
        jv[i].pan_l = jv[i].pan_r = 1.0f;
    }
}

/* block prologue (core 0): the queued keys, the clock, the voices' events. Returns the control steps. */
static uint32_t eng_prologue(uint32_t n)
{
    uint32_t nsb = n / JP_SUB, sb, i, L;
    uint8_t mode = jp_p[P_ASSIGN] <= AS_CHORD ? jp_p[P_ASSIGN] : AS_POLY1, km = jp_g[G_KEYMODE] <= KM_DUAL ? jp_g[G_KEYMODE] : KM_WHOLE;
    uint8_t nv;

    /* the voices allowed: the setting, or (AUTO) what the load allows */
    switch (jp_g[G_VOICES]) {
    case VO_4: nv = 4; break;
    case VO_6: nv = 6; break;
    case VO_8: nv = 8; break;
    default: nv = eng_nv; break;
    }
    jp_nv_now = nv;
    /* the layers: a change of key mode, split or octave lets every note go first */
    if (km != as_km || jp_g[G_SPLIT] != as_split || (int8_t)(12 * ((int32_t)jp_g[G_LOWOCT] - 2)) != as_lowoff) {
        as_all_off(0);
        as_km = km;
        as_split = jp_g[G_SPLIT];
        as_lowoff = (int8_t)(12 * ((int32_t)jp_g[G_LOWOCT] - 2));
    }
    if (km == KM_WHOLE) {
        LA[0].nv = nv; LA[1].nv = 0;
        for (i = 0; i < nv; i++) LA[0].vl[i] = ORDER[i];
    } else {                                    /* UPPER the even places of ORDER (core 0), LOWER the odd (core 1) */
        LA[0].nv = (uint8_t)((nv + 1u) / 2u); LA[1].nv = (uint8_t)(nv / 2u);
        for (i = 0; i < nv; i++) LA[i & 1u].vl[i / 2u] = ORDER[i];
    }
    for (L = 0; L < 2u; L++) {
        uint8_t m = lay_p(L)[P_ASSIGN] <= AS_CHORD ? lay_p(L)[P_ASSIGN] : AS_POLY1;
        if (m != LA[L].mode) {
            uint32_t j;
            for (j = 0; j < JP_NV; j++)
                if (jv[j].lgate && jv[j].llayer == L) vev(&jv[j], L, 0, EV_OFF, jv[j].lnote, 0);
            LA[L].nstack = 0;
            LA[L].mode = m;
        }
    }
    derive(0, nsb);
    derive(1, nsb);
    for (i = 0; i < JP_NV; i++) {
        float sp = (i & 1u) ? jcl[0].spread : -jcl[0].spread;
        if (((i >> 1) & 1u)) sp *= 0.5f;
        if (mode == AS_SOLO && km == KM_WHOLE) sp = 0.0f;   /* one voice: in the middle */
        jv[i].pan_l = 1.0f - sp;
        jv[i].pan_r = 1.0f + sp;
    }
    eng_seq_on = jp_seq_play && jp_g[G_PLAYMODE] != PM_DRUMS;
    eng_dr_on = jp_seq_play && jp_g[G_PLAYMODE] != PM_SEQ;

    while (kq_r != kq_w) {
        uint32_t r = kq_r % KQ;
        uint8_t t = kq[r].type, a = kq[r].a, b = kq[r].b;
        kq_r++;
        switch (t) {
        case KE_ON:
            if (eng_seq_on && !jp_seq_rec && jp_g[G_SEQKEY]) { jp_transpose = (int8_t)((int32_t)a - 60); break; }
            if (jp_arp_on && !eng_seq_on) { ar_vel = b; ar_add(a); break; }
            as_on(a, b, 0);
            break;
        case KE_OFF:
            if (eng_seq_on && !jp_seq_rec && jp_g[G_SEQKEY]) break;
            if (jp_arp_on && !eng_seq_on) { ar_remove(a); break; }
            as_off(a, 0);
            break;
        case KE_SUSTAIN: as_sustain_set(a, 0); break;
        case KE_BEND: bend_v = ((float)((int32_t)a << 7 | b) - 8192.0f) * (1.0f / 8192.0f); break;
        case KE_WHEEL: wheel_v = (float)a * (1.0f / 127.0f); break;
        case KE_DRUM: dr_trigger(a, (float)b * (1.0f / 127.0f), 0); break;
        case KE_PANIC:
            as_sustain = 0;
            as_all_off(0);
            ar_n = ar_phys = 0; ar_note = 255; sq_n[0] = sq_n[1] = 255;
            break;
        }
    }
    /* the transport, the sequencer, the arp: on and off */
    if (jp_seq_play != tp_was) {
        tp_was = jp_seq_play;
        if (tp_was) { ck_restart = 1; ck_step = 0; sq_step = 0; }
        else jp_dr_step_shown = 255;
    }
    if (eng_seq_on != sq_was) {
        sq_was = eng_seq_on;
        if (sq_was) { sq_step = 0; jp_transpose = 0; if (ar_note != 255u) { as_off(ar_note, 0); ar_note = 255; } }
        else {
            if (sq_n[0] != 255u) { as_off(sq_n[0], 0); sq_n[0] = 255; }
            if (sq_n[1] != 255u) { as_off_lower(sq_n[1], 0); sq_n[1] = 255; }
            jp_seq_pos_shown = 255;
        }
    }
    if (jp_arp_on != ar_was) {
        ar_was = jp_arp_on;
        if (!ar_was) { if (ar_note != 255u) { as_off(ar_note, 0); ar_note = 255; } ar_n = ar_phys = 0; }
        else ar_pos = 0;
    }
    if (jp_arp_on && !eng_seq_on && !ar_n && ar_note != 255u) { as_off(ar_note, 0); ar_note = 255; }

    if (jp_seq_play || (jp_arp_on && ar_n)) {
        if (ck_restart) { ck_restart = 0; ck_pos = 0.0f; ar_pos = 0; clock_set(); clock_step(0); }
        else clock_set();
        for (sb = 0; sb < nsb; sb++) {
            ck_pos += (float)JP_SUB;
            if (!ck_gated && ck_pos >= ck_len * ck_gate) { ck_gated = 1; clock_gate_off((uint8_t)sb); }
            if (ck_pos >= ck_len) { ck_pos -= ck_len; clock_step((uint8_t)sb); }
        }
    }
    return nsb;
}

/* the voices' load: AUTO voices follow it (busy is the block's time over its budget, x 256) */
static void eng_load(uint32_t busy)
{
    uint32_t i, act = 0;
    for (i = 0; i < JP_NV; i++) if (jv[i].st2) act++;
    jp_active = (uint8_t)act;
    jp_cpu = (uint8_t)(busy * 100u >> 8 > 255u ? 255u : busy * 100u >> 8);
    if (busy > 230u) {                          /* over 90 %: one voice less, now */
        if (eng_nv > 4u) eng_nv--;
        eng_under = 0;
    } else if (busy < 140u && act >= eng_nv) {  /* under 55 % with all of them playing: one more, after a while */
        if (++eng_under > 400u && eng_nv < 8u) { eng_nv++; eng_under = 0; }
    }
}

/* the mix into the host's Q15 frames (a DC block at ~8 Hz, then the soft clip) */
static float dc_xl, dc_xr, dc_yl, dc_yr;
static void eng_out(int32_t *out, uint32_t n, uint32_t both)
{
    uint32_t i;
    float *L = bufL[0], *R = bufR[0];
    float dg = (float)jp_g[G_DRLEVEL] * (1.0f / 127.0f);
    if (both)
        for (i = 0; i < n; i++) { L[i] += bufL[1][i]; R[i] += bufR[1][i]; }
    chorus(L, R, n);
    drums_render(n);                            /* the drums: beside the chorus, not through it */
    dg = dg * dg * 0.8f;
    for (i = 0; i < n; i++) { L[i] += dbufL[i] * dg; R[i] += dbufR[i] * dg; }
    for (i = 0; i < n; i++) {
        float l, r;
        dc_yl = L[i] - dc_xl + 0.9989f * dc_yl; dc_xl = L[i];
        dc_yr = R[i] - dc_xr + 0.9989f * dc_yr; dc_xr = R[i];
        l = jp_clip(dc_yl) * 32000.0f;
        r = jp_clip(dc_yr) * 32000.0f;
        out[2u * i] += (int32_t)l;
        out[2u * i + 1u] += (int32_t)r;
    }
}

#endif
