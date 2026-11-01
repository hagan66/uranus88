/* SPDX-License-Identifier: GPL-3.0-only */
/* URANUS 88: the parameters. A patch is 48 values (0..max each) and a name; the global values (arpeggiator,
 * sequencer, voices) are kept apart, in the settings record. Each page shows four of them on KNOB1..4. */
#ifndef JP_PARAMS_H
#define JP_PARAMS_H
#include <stdint.h>

enum {
    P_V1WAVE, P_V1RANGE, P_XMOD, P_PW,          /* VCO-1 */
    P_V2WAVE, P_V2RANGE, P_V2FINE, P_SYNC,      /* VCO-2 */
    P_LFOVCO, P_ENVVCO, P_MODDEST, P_PWMSRC,    /* VCO MODULATOR */
    P_MIX1, P_MIX2, P_HPF, P_LEVEL,             /* SOURCE MIX */
    P_CUTOFF, P_RES, P_SLOPE, P_ENVMOD,         /* VCF */
    P_VCFENV, P_VCFLFO, P_KEYFLW, P_VCFVEL,     /* VCF MOD */
    P_A1, P_D1, P_S1, P_R1,                     /* ENV-1 */
    P_A2, P_D2, P_S2, P_R2,                     /* ENV-2 (the VCA's) */
    P_LFORATE, P_LFODELAY, P_LFOWAVE, P_TREM,   /* LFO */
    P_CHMODE, P_CHDEPTH, P_CHRATE, P_CHMIX,     /* CHORUS */
    P_VCAVEL, P_ENVKF, P_PORTA, P_ANALOG,       /* VCA / KEY */
    P_ASSIGN, P_VDEPTH, P_BEND, P_WHEEL,        /* KEY ASSIGN (VOICE DEPTH: was the unison's detune) */
    NP
};

enum {
    G_ARPMODE, G_ARPRANGE, G_TEMPO, G_HOLD,
    G_SEQLEN, G_GATE, G_SWING, G_SEQKEY,
    G_VOICES, G_TUNE, G_KEYVEL, G_SPREAD,
    G_PATTERN, G_PLAYMODE, G_DRLEVEL, G_ACCENT,  /* the drum machine */
    G_DRINST,
    G_KEYMODE, G_LOWER, G_SPLIT, G_LOWOCT,       /* WHOLE / SPLIT / DUAL, the lower patch, the split point */
    G_SCREEN, G_BTNLT, G_KEYLT, G_DIM,           /* LIGHTS: the screen's brightness, the buttons' glow, the keys' LEDs, dim when idle */
    NG
};

/* enumerations */
enum { W1_TRI, W1_SAW, W1_PULSE, W1_SQR };
enum { W2_SIN, W2_SAW, W2_PULSE, W2_NOISE };
enum { MD_VCO1, MD_BOTH, MD_VCO2 };
enum { PWM_MAN, PWM_LFO, PWM_ENV1 };
enum { LW_TRI, LW_SAW, LW_SQR, LW_RND };
enum { CH_OFF, CH_I, CH_II, CH_I_II };
enum { AS_POLY1, AS_POLY2, AS_UNISON, AS_SOLO, AS_CHORD };
enum { AR_UP, AR_DOWN, AR_UPDOWN, AR_RANDOM };
enum { VO_AUTO, VO_4, VO_6, VO_8 };
enum { PM_BOTH, PM_DRUMS, PM_SEQ };             /* what PLAY runs */
enum { KM_WHOLE, KM_SPLIT, KM_DUAL };
enum { DR_BD, DR_SD, DR_LT, DR_CP, DR_RS, DR_CB, DR_CH, DR_OH, DR_AC, DR_ROWS };
#define DR_N 8                                  /* instruments (DR_AC is the accent row) */
#define DR_PATTERNS 8

#define JP_NAME 12
typedef struct {
    uint8_t p[NP];
    char name[JP_NAME];
} jp_patch_t;                                   /* 60 bytes */

#define JP_PATCHES 64                           /* 11..88: eight banks of eight, as on the original */
#define JP_STEPS 16
#define SEQ_REST 0xFFu
#define SEQ_TIE  0xFEu

#endif
