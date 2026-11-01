/* SPDX-License-Identifier: GPL-3.0-only */
/* URANUS 88: an eight-voice analog-style polysynth for the M-VAVE FM-1 after the great 1981 flagship, as a
 * fm1-dock module (built alone as its own firmware with tools/build.py --modules ... --name). Not affiliated with
 * Roland or M-VAVE.
 *
 *   KNOB1..4     the page's four values             SELECT     the page
 *   ALGORITHM    the cutoff, on any page            PRESETS    the patch, 11..88
 *   EDIT         VCO-1 > VCO-2 > VCO MOD > MIXER    SEL        VCF > VCF MOD > VCA/KEY
 *   ENV          ENV-1 / ENV-2                      LFO        LFO
 *   FX           CHORUS (again: its mode)           GLO        ASSIGN > GLOBAL > KEY MODE
 *   KEY MODE     WHOLE, or SPLIT / DUAL with a LOWER patch (four voices each); track 2 of the sequencer plays LOWER
 *   ARP          the arpeggiator on / off           SEQ        the sequencer's page
 *   PLAY         the sequencer runs / stops         REC        step entry: keys write steps, SEL a rest, EDIT a tie
 *   SAVE         SAVE, turn PRESETS to the slot, SAVE again (any other button: cancel)
 *   SEQ          SEQUENCER > DRUM MACHINE > DRUM KIT. On the two drum pages the keys are the 808's: the 16 white
 *                keys the steps of the instrument chosen, the black keys choose it (BD SD LT CP RS CB CH OH AC)
 *   OCT- / OCT+  the keys' octave                   HOME held  fm1-dock's picker */
#include "hb_module.h"
#include "jp_engine.h"
#include "jp_presets.h"

static const hb_host_t *H;

/* ------------------------------------------------------------------------------------------------ the pages */
enum { K_10, K_ENUM, K_BPM, K_PM, K_INT1, K_INT0, K_PATCH, K_NOTE, K_OCT, K_VDEPTH };
typedef struct {
    uint8_t glob, idx, max, kind;
    const char *label;
    const char *const *names;
} jp_knob_t;
typedef struct {
    const char *title;
    uint8_t color;
    jp_knob_t k[4];
} jp_page_t;

static const char *const N_W1[] = {"TRI", "SAW", "PULSE", "SQR"};
static const char *const N_W2[] = {"SINE", "SAW", "PULSE", "NOISE"};
static const char *const N_RANGE[] = {"64'", "32'", "16'", "8'", "4'", "2'"};
static const char *const N_ONOFF[] = {"OFF", "ON"};
static const char *const N_DEST[] = {"VCO-1", "BOTH", "VCO-2"};
static const char *const N_PWM[] = {"MAN", "LFO", "ENV-1"};
static const char *const N_SLOPE[] = {"24 DB", "12 DB"};
static const char *const N_ENVSEL[] = {"ENV-1", "ENV-2"};
static const char *const N_LW[] = {"TRI", "SAW", "SQR", "RAND"};
static const char *const N_CH[] = {"OFF", "I", "II", "I+II"};
static const char *const N_AS[] = {"POLY1", "POLY2", "UNISON", "SOLO", "CHORD"};
static const char *const N_CHORD[14] = {"5TH", "SUS2", "M", "MAJ", "SUS4", "M7", "7", "7SUS4", "MAJ7", "AUG", "DIM",
                                        "M7B5", "MMAJ7", "MAJ7B5"};
static const char *const N_AR[] = {"UP", "DOWN", "U&D", "RAND"};
static const char *const N_VO[] = {"AUTO", "4", "6", "8"};
static const char *const N_SK[] = {"PLAY", "TRANSP"};
static const char *const N_PM[] = {"BOTH", "DRUMS", "SEQ"};
static const char *const N_KM[] = {"WHOLE", "SPLIT", "DUAL"};
static const char *const N_DRI[] = {"BD", "SD", "LT", "CP", "RS", "CB", "CH", "OH", "AC"};

enum { CO_RED, CO_ORANGE, CO_YELLOW, CO_WHITE, CO_BLUE, CO_GREY };
#define NPAGES 18u
static const jp_page_t PAGES[NPAGES] = {
    {"VCO-1", CO_RED, {{0, P_V1WAVE, 3, K_ENUM, "WAVE", N_W1}, {0, P_V1RANGE, 5, K_ENUM, "RANGE", N_RANGE},
                       {0, P_XMOD, 127, K_10, "X-MOD", 0}, {0, P_PW, 127, K_10, "P.WIDTH", 0}}},
    {"VCO-2", CO_RED, {{0, P_V2WAVE, 3, K_ENUM, "WAVE", N_W2}, {0, P_V2RANGE, 5, K_ENUM, "RANGE", N_RANGE},
                       {0, P_V2FINE, 127, K_PM, "FINE", 0}, {0, P_SYNC, 1, K_ENUM, "SYNC", N_ONOFF}}},
    {"VCO MODULATOR", CO_RED, {{0, P_LFOVCO, 127, K_10, "LFO", 0}, {0, P_ENVVCO, 127, K_10, "ENV-1", 0},
                               {0, P_MODDEST, 2, K_ENUM, "DEST", N_DEST}, {0, P_PWMSRC, 2, K_ENUM, "PWM", N_PWM}}},
    {"SOURCE MIX", CO_ORANGE, {{0, P_MIX1, 127, K_10, "VCO-1", 0}, {0, P_MIX2, 127, K_10, "VCO-2", 0},
                               {0, P_HPF, 127, K_10, "HPF", 0}, {0, P_LEVEL, 127, K_10, "LEVEL", 0}}},
    {"VCF", CO_ORANGE, {{0, P_CUTOFF, 127, K_10, "CUTOFF", 0}, {0, P_RES, 127, K_10, "RESO", 0},
                        {0, P_SLOPE, 1, K_ENUM, "SLOPE", N_SLOPE}, {0, P_ENVMOD, 127, K_10, "ENV MOD", 0}}},
    {"VCF MODULATION", CO_ORANGE, {{0, P_VCFENV, 1, K_ENUM, "ENV", N_ENVSEL}, {0, P_VCFLFO, 127, K_10, "LFO", 0},
                                   {0, P_KEYFLW, 127, K_10, "KEY FLW", 0}, {0, P_VCFVEL, 127, K_10, "VELO", 0}}},
    {"ENV-1", CO_YELLOW, {{0, P_A1, 127, K_10, "ATTACK", 0}, {0, P_D1, 127, K_10, "DECAY", 0},
                          {0, P_S1, 127, K_10, "SUSTAIN", 0}, {0, P_R1, 127, K_10, "RELEASE", 0}}},
    {"ENV-2  (VCA)", CO_YELLOW, {{0, P_A2, 127, K_10, "ATTACK", 0}, {0, P_D2, 127, K_10, "DECAY", 0},
                                 {0, P_S2, 127, K_10, "SUSTAIN", 0}, {0, P_R2, 127, K_10, "RELEASE", 0}}},
    {"LFO", CO_WHITE, {{0, P_LFORATE, 127, K_10, "RATE", 0}, {0, P_LFODELAY, 127, K_10, "DELAY", 0},
                       {0, P_LFOWAVE, 3, K_ENUM, "WAVE", N_LW}, {0, P_TREM, 127, K_10, "VCA MOD", 0}}},
    {"CHORUS", CO_BLUE, {{0, P_CHMODE, 3, K_ENUM, "MODE", N_CH}, {0, P_CHDEPTH, 127, K_10, "DEPTH", 0},
                         {0, P_CHRATE, 127, K_PM, "RATE", 0}, {0, P_CHMIX, 127, K_10, "MIX", 0}}},
    {"VCA / KEYBOARD", CO_WHITE, {{0, P_VCAVEL, 127, K_10, "VELO", 0}, {0, P_ENVKF, 127, K_10, "ENV KEY", 0},
                                  {0, P_PORTA, 127, K_10, "PORTA", 0}, {0, P_ANALOG, 127, K_10, "ANALOG", 0}}},
    {"KEY ASSIGN", CO_WHITE, {{0, P_ASSIGN, 4, K_ENUM, "MODE", N_AS}, {0, P_VDEPTH, 127, K_VDEPTH, "V.DEPTH", 0},
                              {0, P_BEND, 12, K_INT0, "BEND", 0}, {0, P_WHEEL, 127, K_10, "MOD WHL", 0}}},
    {"ARPEGGIO", CO_BLUE, {{1, G_ARPMODE, 3, K_ENUM, "MODE", N_AR}, {1, G_ARPRANGE, 3, K_INT1, "RANGE", 0},
                           {1, G_TEMPO, 200, K_BPM, "TEMPO", 0}, {1, G_HOLD, 1, K_ENUM, "HOLD", N_ONOFF}}},
    {"SEQUENCER", CO_BLUE, {{1, G_TEMPO, 200, K_BPM, "TEMPO", 0}, {1, G_SEQLEN, 15, K_INT1, "LENGTH", 0},
                            {1, G_GATE, 127, K_10, "GATE", 0}, {1, G_SWING, 127, K_10, "SWING", 0}}},
    {"GLOBAL", CO_GREY, {{1, G_VOICES, 3, K_ENUM, "VOICES", N_VO}, {1, G_TUNE, 127, K_PM, "TUNE", 0},
                         {1, G_SEQKEY, 1, K_ENUM, "SEQ KEYS", N_SK}, {1, G_SPREAD, 127, K_10, "SPREAD", 0}}},
    {"DRUM MACHINE", CO_ORANGE, {{1, G_PATTERN, 7, K_INT1, "PATTERN", 0}, {1, G_PLAYMODE, 2, K_ENUM, "PLAY", N_PM},
                                 {1, G_DRLEVEL, 127, K_10, "LEVEL", 0}, {1, G_ACCENT, 127, K_10, "ACCENT", 0}}},
    {"DRUM KIT", CO_ORANGE, {{1, G_DRINST, 8, K_ENUM, "INST", N_DRI}, {2, 0, 127, K_10, "TUNE", 0},
                             {2, 1, 127, K_10, "DECAY", 0}, {2, 2, 127, K_10, "LEVEL", 0}}},
    {"KEY MODE", CO_WHITE, {{1, G_KEYMODE, 2, K_ENUM, "MODE", N_KM}, {1, G_LOWER, 63, K_PATCH, "LOWER", 0},
                            {1, G_SPLIT, 127, K_NOTE, "SPLIT", 0}, {1, G_LOWOCT, 3, K_OCT, "LOW OCT", 0}}},
};
enum { PG_VCO1, PG_VCO2, PG_VCOMOD, PG_MIX, PG_VCF, PG_VCFMOD, PG_ENV1, PG_ENV2, PG_LFO, PG_CHORUS, PG_VCA,
       PG_ASSIGN, PG_ARP, PG_SEQ, PG_GLOBAL, PG_DRUMS, PG_KIT, PG_KEYMODE };
#define DRUM_PAGE (page == PG_DRUMS || page == PG_KIT)

/* ------------------------------------------------------------------------------------------------ state */
static jp_patch_t lib[JP_PATCHES];
static uint8_t cur, page, edited, octave = 2;  /* octave 2: the keys from F3 */
static uint8_t save_mode, save_to;
static uint8_t rec_pos, rec_len;
static uint8_t key_note[27];
static uint8_t cc1, press_max;
static uint32_t settings_due, msg_until;
static const char *msg;

#define SET_MAGIC 0x4A50383Au                   /* "JP8:": the drum machine, two tracks, split */
#define LIB_MAGIC 0x4A50424Bu
typedef struct {
    uint32_t magic;
    uint8_t g[NG];
    uint8_t seq[JP_STEPS], seq2[JP_STEPS];
    uint8_t cur, octave, page, pad;
    uint16_t pat[DR_PATTERNS][DR_ROWS];
    uint8_t dk[3][DR_N];
} jp_settings_t;                                /* (fits the host's 240 bytes) */
typedef char jp_settings_fits[sizeof(jp_settings_t) <= 240u ? 1 : -1];
static struct { uint32_t magic; jp_patch_t p[JP_PATCHES]; } libfile;

static uint8_t valget(const jp_knob_t *k)
{
    if (k->glob == 2u) return jp_g[G_DRINST] < DR_N ? jp_dk[k->idx][jp_g[G_DRINST]] : 0u;   /* the kit's chosen voice */
    return k->glob ? jp_g[k->idx] : jp_p[k->idx];
}

static void valset(const jp_knob_t *k, int32_t v)
{
    if (v < 0) v = 0;
    if (v > k->max) v = k->max;
    if (k->glob == 2u) {
        if (jp_g[G_DRINST] < DR_N) jp_dk[k->idx][jp_g[G_DRINST]] = (uint8_t)v;
        settings_due = H->ms() + 2000u;
    } else if (k->glob) {
        jp_g[k->idx] = (uint8_t)v;
        settings_due = H->ms() + 2000u;
    } else if (jp_p[k->idx] != (uint8_t)v) {
        jp_p[k->idx] = (uint8_t)v;
        edited = 1;
    }
}

static void patch_load(uint32_t i)
{
    uint32_t j;
    cur = (uint8_t)(i % JP_PATCHES);
    for (j = 0; j < NP; j++) jp_p[j] = lib[cur].p[j];
    edited = 0;
    settings_due = H->ms() + 2000u;
}

static void settings_save(void)
{
    jp_settings_t s;
    uint32_t i;
    s.magic = SET_MAGIC;
    for (i = 0; i < NG; i++) s.g[i] = jp_g[i];
    for (i = 0; i < JP_STEPS; i++) { s.seq[i] = jp_seq[i]; s.seq2[i] = jp_seq2[i]; }
    s.cur = cur; s.octave = octave; s.page = page; s.pad = 0;
    for (i = 0; i < DR_PATTERNS * DR_ROWS; i++) s.pat[i / DR_ROWS][i % DR_ROWS] = jp_pat[i / DR_ROWS][i % DR_ROWS];
    for (i = 0; i < 3u * DR_N; i++) s.dk[i / DR_N][i % DR_N] = jp_dk[i / DR_N][i % DR_N];
    H->save(&s, sizeof s);
}

static void say(const char *s, uint32_t ms) { msg = s; msg_until = H->ms() + ms; }

/* ------------------------------------------------------------------------------------------------ panel */
static void leds(void)
{
    static const uint8_t PAGE_BTN[NPAGES] = {HB_BTN_EDIT, HB_BTN_EDIT, HB_BTN_EDIT, HB_BTN_EDIT, HB_BTN_SEL,
        HB_BTN_SEL, HB_BTN_ENV, HB_BTN_ENV, HB_BTN_LFO, HB_BTN_FX, HB_BTN_SEL, HB_BTN_GLO, HB_BTN_ARP, HB_BTN_SEQ,
        HB_BTN_GLO, HB_BTN_SEQ, HB_BTN_SEQ, HB_BTN_GLO};
    static const uint8_t BTNS[] = {HB_BTN_EDIT, HB_BTN_SEL, HB_BTN_ENV, HB_BTN_LFO, HB_BTN_FX, HB_BTN_GLO,
                                   HB_BTN_SEQ};
    uint32_t i;
    for (i = 0; i < sizeof BTNS; i++) H->led(BTNS[i], BTNS[i] == PAGE_BTN[page] ? 1u : 0u);
    H->led(HB_BTN_ARP, jp_arp_on ? 1u : PAGE_BTN[page] == HB_BTN_ARP ? 2u : 0u);
    H->led(HB_BTN_PLAY, jp_seq_play ? 1u : 0u);
    H->led(HB_BTN_REC, jp_seq_rec ? 1u : 0u);
    H->led(HB_BTN_SAVE, save_mode ? 1u : 0u);
    H->led(HB_BTN_OCT_DOWN, octave < 2u ? 1u : 0u);
    H->led(HB_BTN_OCT_UP, octave > 2u ? 1u : 0u);
}

static const uint8_t WHITE_KEY[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const uint8_t BLACK_KEY[DR_ROWS] = {1, 3, 5, 8, 10, 13, 15, 17, 20};   /* the drum pages: BD SD .. OH AC */
static uint32_t key_leds_shown = 0xFFFFFFFFu, key_glow_shown = 0xFFFFFFFFu;

static void key_leds(void)                      /* held keys lit; on the sequencer, its steps glow on the white keys */
{
    uint32_t want = 0, glow = 0, k, i;
    for (k = 0; k < 27u; k++) if (key_note[k]) want |= 1u << k;
    if (DRUM_PAGE && !jp_seq_rec) {             /* the row's steps glow, the playhead and the instrument are lit */
        uint32_t row = jp_g[G_DRINST] < DR_ROWS ? jp_g[G_DRINST] : 0u, bits = jp_pat[jp_g[G_PATTERN] % DR_PATTERNS][row];
        for (i = 0; i < 16u; i++) if (bits >> i & 1u) glow |= 1u << WHITE_KEY[i];
        if (jp_dr_step_shown < 16u) want |= 1u << WHITE_KEY[jp_dr_step_shown];
        want |= 1u << BLACK_KEY[row];
    } else if (page == PG_SEQ || jp_seq_rec) {
        uint32_t len = (uint32_t)jp_g[G_SEQLEN] + 1u, at = jp_seq_rec ? rec_pos : jp_seq_pos_shown;
        for (i = 0; i < len; i++)
            if ((jp_seq_rec == 2u ? jp_seq2[i] : jp_seq[i]) < 128u || (!jp_seq_rec && jp_seq2[i] < 128u))
                glow |= 1u << WHITE_KEY[i];
        if (at < 16u) want |= 1u << WHITE_KEY[at];
    }
    if (want == key_leds_shown && glow == key_glow_shown) return;
    for (k = 0; k < 27u; k++)
        if (((want ^ key_leds_shown) | (glow ^ key_glow_shown)) >> k & 1u)
            H->led(HB_LED_KEY(k), want >> k & 1u ? 1u : glow >> k & 1u ? 2u : 0u);
    key_leds_shown = want;
    key_glow_shown = glow;
}

static void page_go(uint32_t p) { page = (uint8_t)(p % NPAGES); }

static void rec_store(void)                     /* a track's recording ends: track 1 sets the length, track 2 may grow it */
{
    if (rec_len && (jp_seq_rec == 1u || rec_len - 1u > jp_g[G_SEQLEN])) jp_g[G_SEQLEN] = (uint8_t)(rec_len - 1u);
    settings_due = H->ms() + 500u;
}

static void seq_rec_toggle(void)                /* REC: track 1, again: track 2 (LOWER), again: stored */
{
    if (!jp_seq_rec) {
        jp_seq_rec = 1; rec_pos = 0; rec_len = 0; page = PG_SEQ;
        say("REC TRACK 1: SEL REST EDIT TIE", 2500);
    } else if (jp_seq_rec == 1u) {
        rec_store();
        jp_seq_rec = 2; rec_pos = 0; rec_len = 0;
        say("REC TRACK 2 (LOWER)", 2500);
    } else {
        rec_store();
        jp_seq_rec = 0;
        say("SEQUENCE STORED", 1200);
    }
}

static void rec_write(uint8_t v)
{
    if (jp_seq_rec == 2u) jp_seq2[rec_pos] = v; else jp_seq[rec_pos] = v;
    if (rec_len < rec_pos + 1u) rec_len = (uint8_t)(rec_pos + 1u);
    rec_pos = (uint8_t)((rec_pos + 1u) % JP_STEPS);
    if (!rec_pos) say("16 STEPS: FROM THE TOP", 1000);
}

static void button(uint32_t id)
{
    if (save_mode && id != HB_BTN_SAVE) { save_mode = 0; say("SAVE CANCELLED", 1000); return; }
    if (jp_seq_rec && id == HB_BTN_SEL) { rec_write(SEQ_REST); return; }
    if (jp_seq_rec && id == HB_BTN_EDIT) { rec_write(SEQ_TIE); return; }
    switch (id) {
    case HB_BTN_EDIT: page_go(page < PG_MIX ? page + 1u : PG_VCO1); break;
    case HB_BTN_SEL: page_go(page == PG_VCF ? PG_VCFMOD : page == PG_VCFMOD ? PG_VCA : PG_VCF); break;
    case HB_BTN_ENV: page_go(page == PG_ENV1 ? PG_ENV2 : PG_ENV1); break;
    case HB_BTN_LFO: page_go(PG_LFO); break;
    case HB_BTN_FX:
        if (page == PG_CHORUS) valset(&PAGES[PG_CHORUS].k[0], (jp_p[P_CHMODE] + 1) & 3);
        page_go(PG_CHORUS);
        break;
    case HB_BTN_GLO: page_go(page == PG_ASSIGN ? PG_GLOBAL : page == PG_GLOBAL ? PG_KEYMODE : PG_ASSIGN); break;
    case HB_BTN_ARP:
        jp_arp_on = !jp_arp_on;
        page_go(PG_ARP);
        say(jp_arp_on ? "ARPEGGIO ON" : "ARPEGGIO OFF", 900);
        break;
    case HB_BTN_SEQ: page_go(page == PG_SEQ ? PG_DRUMS : page == PG_DRUMS ? PG_KIT : PG_SEQ); break;
    case HB_BTN_PLAY:
        if (jp_seq_rec) { rec_store(); jp_seq_rec = 0; say("SEQUENCE STORED", 1200); }
        jp_seq_play = !jp_seq_play;
        break;
    case HB_BTN_REC: seq_rec_toggle(); break;
    case HB_BTN_SAVE:
        if (!save_mode) { save_mode = 1; save_to = cur; break; }
        {
            uint32_t j;
            for (j = 0; j < NP; j++) lib[save_to].p[j] = jp_p[j];
            if (save_to != cur) for (j = 0; j < JP_NAME; j++) lib[save_to].name[j] = lib[cur].name[j];
            libfile.magic = LIB_MAGIC;
            for (j = 0; j < JP_PATCHES; j++) libfile.p[j] = lib[j];
            save_mode = 0;
            cur = save_to;
            edited = 0;
            if (HB_HOST_HAS(H, bank_save) && H->bank_save(&libfile, sizeof libfile) == 0) say("SAVED", 1200);
            else say("SAVE FAILED", 2000);
            settings_due = H->ms() + 500u;
        }
        break;
    case HB_BTN_OCT_DOWN: if (octave > 0u) octave--; settings_due = H->ms() + 2000u; break;
    case HB_BTN_OCT_UP: if (octave < 4u) octave++; settings_due = H->ms() + 2000u; break;
    default: break;
    }
}

static void drum_key(uint32_t k)                /* the drum pages: a white key sets a step, a black one an instrument */
{
    uint32_t i, pat = jp_g[G_PATTERN] % DR_PATTERNS, row = jp_g[G_DRINST] < DR_ROWS ? jp_g[G_DRINST] : 0u;
    for (i = 0; i < 16u; i++)
        if (WHITE_KEY[i] == k) { jp_pat[pat][row] ^= (uint16_t)(1u << i); settings_due = H->ms() + 2000u; return; }
    for (i = 0; i < DR_ROWS; i++)
        if (BLACK_KEY[i] == k) {
            jp_g[G_DRINST] = (uint8_t)i;
            if (i < DR_N) key_push(KE_DRUM, (uint8_t)i, 110);
            return;
        }
}

static void knob(const jp_knob_t *k, int32_t arg)
{
    int32_t v = valget(k), step = arg;
    if (k->kind != K_ENUM && k->max > 20u && (arg > 1 || arg < -1)) step = arg * 2;   /* fast turns go further */
    valset(k, v + step);
}

static void ui(uint32_t ev, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev);
    switch (HB_EV_KIND(ev)) {
    case HB_EV_ENC:
        if (id >= HB_ENC_KNOB1 && id <= HB_ENC_KNOB4) knob(&PAGES[page].k[id - HB_ENC_KNOB1], arg);
        else if (id == HB_ENC_SELECT) page_go((uint32_t)((int32_t)page + NPAGES * 4 + arg));
        else if (id == HB_ENC_ALGORITHM) knob(&PAGES[PG_VCF].k[0], arg);
        else if (id == HB_ENC_PRESETS) {
            int32_t t = ((save_mode ? save_to : cur) + JP_PATCHES * 4 + arg) % JP_PATCHES;
            if (save_mode) save_to = (uint8_t)t;
            else patch_load((uint32_t)t);
        }
        break;
    case HB_EV_BTN_DOWN: button(id); break;
    case HB_EV_KEY_DOWN:
        if (id < 27u && DRUM_PAGE && !jp_seq_rec) { drum_key(id); break; }
        if (id < 27u) {
            int32_t n = 41 + (int32_t)octave * 12 + (int32_t)id;   /* octave 2: F3 = 53 */
            uint8_t vel = arg > 0 ? (uint8_t)arg : 100u;
            if (n > 127) n = 127;
            key_note[id] = (uint8_t)n;
            if (jp_seq_rec) rec_write((uint8_t)n);
            key_push(KE_ON, (uint8_t)n, vel);
            H->midi_send(0x90u, (uint32_t)n, vel);
        }
        break;
    case HB_EV_KEY_UP:
        if (id < 27u && key_note[id]) {
            key_push(KE_OFF, key_note[id], 0);
            H->midi_send(0x80u, key_note[id], 0);
            key_note[id] = 0;
        }
        break;
    case HB_EV_KEY_PRESSURE: {                  /* aftertouch: vibrato, as the mod wheel */
        uint8_t m = (uint8_t)(arg > 127 ? 127 : arg < 0 ? 0 : arg);
        press_max = m;
        key_push(KE_WHEEL, m > cc1 ? m : cc1, 0);
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ MIDI */
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    if ((st & 15u) == 9u && (st & 0xF0u) == 0x90u) {   /* channel 10: the drums, General MIDI's notes */
        static const int8_t GM[24] = {DR_BD, DR_BD, DR_RS, DR_SD, DR_CP, DR_SD, DR_LT, DR_CH, DR_LT, DR_CH, DR_LT,
                                      DR_OH, DR_LT, DR_LT, -1, DR_LT, -1, -1, -1, -1, -1, DR_CB, -1, -1};   /* 35.. */
        if (d2 && d1 >= 35u && d1 < 59u && GM[d1 - 35u] >= 0) key_push(KE_DRUM, (uint8_t)GM[d1 - 35u], (uint8_t)d2);
        return;
    }
    switch (st & 0xF0u) {
    case 0x90u: key_push(d2 ? KE_ON : KE_OFF, (uint8_t)d1, (uint8_t)d2); break;
    case 0x80u: key_push(KE_OFF, (uint8_t)d1, 0); break;
    case 0xE0u: key_push(KE_BEND, (uint8_t)d2, (uint8_t)d1); break;
    case 0xC0u: patch_load(d1); break;
    case 0xD0u: key_push(KE_WHEEL, (uint8_t)(d1 > cc1 ? d1 : cc1), 0); break;
    case 0xB0u:
        switch (d1) {
        case 1: cc1 = (uint8_t)d2; key_push(KE_WHEEL, (uint8_t)d2, 0); break;
        case 5: valset(&PAGES[PG_VCA].k[2], (int32_t)d2); break;
        case 64: key_push(KE_SUSTAIN, d2 >= 64u ? 1u : 0u, 0); break;
        case 71: valset(&PAGES[PG_VCF].k[1], (int32_t)d2); break;
        case 72: valset(&PAGES[PG_ENV2].k[3], (int32_t)d2); break;
        case 73: valset(&PAGES[PG_ENV2].k[0], (int32_t)d2); break;
        case 74: valset(&PAGES[PG_VCF].k[0], (int32_t)d2); break;
        case 120: case 123: key_push(KE_PANIC, 0, 0); break;
        default: break;
        }
        break;
    default: break;
    }
}
static void note(uint32_t ch, uint32_t key, uint32_t vel) { midi(0x90u | (ch & 15u), key, vel); }
static void cc(uint32_t ch, uint32_t num, uint32_t val) { midi(0xB0u | (ch & 15u), num, val); }

/* ------------------------------------------------------------------------------------------------ audio */
static uint8_t two_cores;
static void c1_job(uint32_t n) { voices_core(1, n); }

static void render(int32_t *out, uint32_t n)
{
    uint32_t t0 = H->ticks();
    while (n >= JP_SUB) {
        uint32_t m = n > JP_MAXN ? JP_MAXN : n & ~(JP_SUB - 1u), busy, took = 0;
        eng_prologue(m);
        if (two_cores) took = (uint32_t)H->c1_run(c1_job, m);
        voices_core(0, m);
        if (took) { if (H->c1_wait() != 0) voices_core(1, m); }
        else voices_core(1, m);
        jp_cores = (uint8_t)(took ? 2u : 1u);
        eng_out(out, m, 1);
        busy = (H->ticks() - t0) * 256u / (m * 544u);
        eng_load(busy);
        out += 2u * m;
        n -= m;
        t0 = H->ticks();
    }
}

/* ------------------------------------------------------------------------------------------------ the screen */
static const uint16_t SEC[6] = {HB_RGB(214, 56, 40), HB_RGB(236, 124, 34), HB_RGB(240, 200, 48),
                                HB_RGB(228, 224, 212), HB_RGB(84, 140, 210), HB_RGB(150, 150, 150)};
#define C_BG     HB_RGB(0, 0, 0)
#define C_PANEL  HB_RGB(30, 30, 34)
#define C_LINE   HB_RGB(70, 70, 76)
#define C_TEXT   HB_RGB(210, 210, 200)
#define C_DIM    HB_RGB(90, 90, 96)
#define C_AMBER  HB_RGB(255, 172, 48)
#define C_LED    HB_RGB(255, 44, 30)
#define C_LEDOFF HB_RGB(52, 10, 8)
#define C_WIN    HB_RGB(18, 4, 4)

static void seg_digit(int32_t x, int32_t y, uint32_t d)
{
    static const uint8_t SEG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};  /* gfedcba */
    uint32_t s = d < 10u ? SEG[d] : 0u;
    H->fill(x + 4, y, 14, 4, s & 1u ? C_LED : C_LEDOFF);          /* a */
    H->fill(x + 18, y + 4, 4, 15, s & 2u ? C_LED : C_LEDOFF);     /* b */
    H->fill(x + 18, y + 23, 4, 15, s & 4u ? C_LED : C_LEDOFF);    /* c */
    H->fill(x + 4, y + 38, 14, 4, s & 8u ? C_LED : C_LEDOFF);     /* d */
    H->fill(x, y + 23, 4, 15, s & 16u ? C_LED : C_LEDOFF);        /* e */
    H->fill(x, y + 4, 4, 15, s & 32u ? C_LED : C_LEDOFF);         /* f */
    H->fill(x + 4, y + 19, 14, 4, s & 64u ? C_LED : C_LEDOFF);    /* g */
}

static char *put(char *b, const char *s) { while (*s) *b++ = *s++; *b = 0; return b; }
static char *put_u(char *b, uint32_t v)
{
    char t[12];
    uint32_t i = 0;
    do { t[i++] = (char)('0' + v % 10u); v /= 10u; } while (v && i < 11u);
    while (i) *b++ = t[--i];
    *b = 0;
    return b;
}

static void val_text(const jp_knob_t *k, uint8_t v, char *b)
{
    switch (k->kind) {
    case K_ENUM: put(b, k->names[v > k->max ? k->max : v]); break;
    case K_BPM: b = put_u(b, 40u + v); put(b, " BPM"); break;
    case K_INT1: put_u(b, v + 1u); break;
    case K_INT0: put_u(b, v); break;
    case K_PATCH: b[0] = (char)('1' + v / 8u); b[1] = (char)('1' + v % 8u); b[2] = 0; break;
    case K_NOTE: {
        static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        b = put(b, NN[v % 12u]);
        if (v >= 12u) put_u(b, v / 12u - 1u);
        break;
    }
    case K_VDEPTH: {                            /* what VOICE DEPTH does in this KEY ASSIGN mode */
        uint32_t m = jp_p[P_ASSIGN];
        if (m == AS_CHORD) { put(b, N_CHORD[(uint32_t)v * 14u / 128u]); break; }
        if (m == AS_UNISON) { b = put_u(b, ((uint32_t)v * 50u + 63u) / 127u); put(b, " CENT"); break; }
        if (!v) { put(b, "OFF"); break; }
        b = put(b, m == AS_SOLO ? (v > 63u ? "SUB2 " : "SUB ") : "DUO ");
        put_u(b, ((uint32_t)v * 9u + 63u) / 127u + 1u);
        break;
    }
    case K_OCT: put(b, v == 0u ? "-2" : v == 1u ? "-1" : v == 2u ? "0" : "+1"); break;
    case K_PM: {
        int32_t d = (int32_t)v - 64;
        if (d > 0) *b++ = '+'; else if (d < 0) { *b++ = '-'; d = -d; }
        put_u(b, (uint32_t)d);
        break;
    }
    default: {                                  /* 0.0 .. 10, as the sliders were marked */
        uint32_t t = ((uint32_t)v * 100u + 63u) / 127u;
        b = put_u(b, t / 10u);
        if (t < 100u) { *b++ = '.'; *b++ = (char)('0' + t % 10u); *b = 0; }
        break;
    }
    }
}

static void draw_header(void)
{
    uint32_t i;
    H->fill(0, 0, 240, 240, C_BG);
    H->text(8, 2, 110, 0, "URANUS 88", C_TEXT, 0);
    H->text(138, 2, 96, 0, "EIGHT VOICES", C_DIM, 0);
    for (i = 0; i < 5u; i++) H->fill((int32_t)i * 48, 20, 48, 3, SEC[i]);
    H->fill(6, 27, 228, 50, C_LINE);            /* the window's bezel */
    H->fill(8, 29, 224, 46, C_WIN);
}

static uint32_t sh_patch = ~0u, sh_page = ~0u, sh_vals = ~0u, sh_status = ~0u, sh_bottom = ~0u;

static void draw_patch(void)
{
    char b[24], *e;
    const char *as = N_AS[jp_p[P_ASSIGN] <= AS_CHORD ? jp_p[P_ASSIGN] : 0u];
    uint32_t show = save_mode ? save_to : cur;
    seg_digit(16, 31, show / 8u + 1u);
    seg_digit(42, 31, show % 8u + 1u);
    H->fill(66, 70, 4, 4, edited && !save_mode ? C_LED : C_WIN);   /* the edit dot */
    H->cv_begin(152, 42, C_WIN);
    if (save_mode) H->cv_text(0, 2, 0, "SAVE TO ?  PRESETS", C_LED);
    else H->cv_text(0, 2, 0, lib[cur].name, C_AMBER);
    if (jp_g[G_KEYMODE] == KM_SPLIT || jp_g[G_KEYMODE] == KM_DUAL) {   /* the LOWER patch beside */
        char n[3];
        n[0] = (char)('1' + jp_g[G_LOWER] / 8u); n[1] = (char)('1' + jp_g[G_LOWER] % 8u); n[2] = 0;
        e = put(b, jp_g[G_KEYMODE] == KM_SPLIT ? "SPLIT " : "DUAL ");
        e = put(e, n);
    } else e = put(b, as);
    if (jp_arp_on) e = put(e, " ARP");
    if (jp_seq_play) e = put(e, " SEQ");
    if (jp_seq_rec) e = put(e, " REC");
    H->cv_text(0, 23, 0, b, C_TEXT);
    H->cv_blit(76, 31);
}

static void draw_page(void)
{
    const jp_page_t *pg = &PAGES[page];
    char b[8], *e;
    uint16_t c = SEC[pg->color];
    H->fill(0, 81, 240, 22, c);
    H->fill(0, 103, 240, 2, C_BG);
    H->cv_begin(240, 22, c);
    H->cv_text(8, 3, 0, pg->title, C_BG);
    e = put_u(b, page + 1u); e = put(e, "/"); put_u(e, NPAGES);
    H->cv_text(232 - H->text_w(0, b), 3, 0, b, C_BG);
    H->cv_blit(0, 81);
}

static void wave_icon(int32_t x, int32_t y, uint32_t w)   /* 24 x 12, in the button: 0 tri 1 saw 2 pulse 3 square 4 sine
                                                         * 5 noise 6 falling saw 7 random steps */
{
    static const int8_t SINE[13] = {6, 3, 1, 0, 1, 3, 6, 9, 11, 12, 11, 9, 6};
    static const int8_t RND[6] = {3, 9, 1, 7, 11, 5};
    uint16_t c = C_BG;
    int32_t i;
    switch (w) {
    case 0: H->cv_line(x, y + 12, x + 6, y, c); H->cv_line(x + 6, y, x + 18, y + 12, c); H->cv_line(x + 18, y + 12, x + 24, y, c); break;
    case 1: H->cv_line(x, y + 12, x + 12, y, c); H->cv_line(x + 12, y, x + 12, y + 12, c); H->cv_line(x + 12, y + 12, x + 24, y, c); break;
    case 6: H->cv_line(x, y, x + 12, y + 12, c); H->cv_line(x + 12, y + 12, x + 12, y, c); H->cv_line(x + 12, y, x + 24, y + 12, c); break;
    case 2: case 3: {
        int32_t hi = w == 2 ? 4 : 12;
        H->cv_line(x, y + 12, x, y, c); H->cv_line(x, y, x + hi, y, c); H->cv_line(x + hi, y, x + hi, y + 12, c);
        H->cv_line(x + hi, y + 12, x + 24, y + 12, c);
        break;
    }
    case 4: for (i = 0; i < 12; i++) H->cv_line(x + i * 2, y + SINE[i], x + i * 2 + 2, y + SINE[i + 1], c); break;
    case 5: for (i = 0; i < 40; i++) H->cv_pixel(x + (int32_t)(H->rand() % 25u), y + (int32_t)(H->rand() % 13u), c); break;
    default:
        for (i = 0; i < 6; i++) {
            H->cv_line(x + i * 4, y + RND[i], x + i * 4 + 4, y + RND[i], c);
            if (i < 5) H->cv_line(x + i * 4 + 4, y + RND[i], x + i * 4 + 4, y + RND[i + 1], c);
        }
        break;
    }
}

static void draw_knobs(void)
{
    const jp_page_t *pg = &PAGES[page];
    uint16_t c = SEC[pg->color];
    uint32_t i;
    H->cv_begin(240, 96, C_BG);
    for (i = 0; i < 4u; i++) {
        const jp_knob_t *k = &pg->k[i];
        int32_t x0 = (int32_t)i * 60, w;
        uint8_t v = valget(k);
        char b[16];
        if (i) H->cv_rect(x0, 4, 1, 88, C_PANEL);
        w = H->text_w(0, k->label);
        H->cv_text(x0 + (60 - w) / 2, 0, 0, k->label, C_TEXT);
        if (k->kind == K_ENUM) {                /* a row of LEDs and the button */
            uint32_t j, n = (uint32_t)k->max + 1u, dx = n > 6u ? 6u : n > 4u ? 8u : 11u;
            int32_t lx = x0 + 30 - (int32_t)(n * dx) / 2;
            for (j = 0; j < n; j++)
                H->cv_rect(lx + (int32_t)(j * dx) + 1, 26, (int32_t)dx - 3, 5, j == v ? C_LED : C_LEDOFF);
            H->cv_rect(x0 + 12, 40, 36, 24, c);
            H->cv_rect(x0 + 12, 60, 36, 4, C_PANEL);
            if (k->names == N_W1 || k->names == N_W2 || k->names == N_LW)
                wave_icon(x0 + 18, 44, k->names == N_W1 ? v : k->names == N_W2 ? (v == W2_SIN ? 4u : v == W2_NOISE ? 5u : v)
                          : (v == LW_TRI ? 0u : v == LW_SAW ? 6u : v == LW_SQR ? 3u : 7u));
        } else {                                /* a slider: the groove, the marks, the cap */
            int32_t top = 22, h = 52, cy;
            uint32_t j;
            for (j = 0; j <= 10u; j++)
                H->cv_rect(x0 + 16, top + h - (int32_t)(j * (uint32_t)h / 10u), j % 5u ? 4 : 7, 1, C_DIM);
            H->cv_rect(x0 + 28, top - 2, 4, h + 4, HB_RGB(0, 0, 0));
            H->cv_rect(x0 + 29, top + h - (int32_t)v * h / (int32_t)k->max, 2,
                       (int32_t)v * h / (int32_t)k->max, c);
            cy = top + h - (int32_t)v * h / (int32_t)k->max;
            H->cv_rect(x0 + 19, cy - 5, 22, 10, HB_RGB(36, 36, 40));
            H->cv_rect(x0 + 19, cy - 5, 22, 1, HB_RGB(110, 110, 116));
            H->cv_rect(x0 + 19, cy, 22, 2, C_TEXT);
        }
        val_text(k, v, b);
        w = H->text_w(0, b);
        H->cv_text(x0 + (60 - w) / 2, 78, 0, b, C_AMBER);
    }
    H->cv_blit(0, 106);
}

static void draw_bottom(void)
{
    char b[40], *e;
    uint32_t i;
    H->cv_begin(240, 34, C_BG);
    H->cv_rect(0, 0, 240, 1, C_PANEL);
    if (msg && H->ms() < msg_until) {
        H->cv_text(120 - H->text_w(0, msg) / 2, 9, 0, msg, C_AMBER);
    } else if (DRUM_PAGE && !jp_seq_rec) {      /* the row's 16 steps, in the 808's four colours */
        uint32_t row = jp_g[G_DRINST] < DR_ROWS ? jp_g[G_DRINST] : 0u, bits = jp_pat[jp_g[G_PATTERN] % DR_PATTERNS][row];
        for (i = 0; i < 16u; i++) {
            int32_t x = 6 + (int32_t)i * 14;
            H->cv_rect(x, 10, 12, 16, bits >> i & 1u ? SEC[i / 4u] : HB_RGB(40, 40, 46));
            if (i == jp_dr_step_shown) H->cv_rect(x, 4, 12, 3, C_LED);
        }
    } else if (page == PG_SEQ || jp_seq_rec) {  /* the steps */
        uint32_t len = (uint32_t)jp_g[G_SEQLEN] + 1u, at = jp_seq_rec ? rec_pos : jp_seq_pos_shown;
        for (i = 0; i < JP_STEPS; i++) {        /* track 1 above, track 2 (LOWER) below */
            int32_t x = 6 + (int32_t)i * 14;
            uint32_t t;
            for (t = 0; t < 2u; t++) {
                uint8_t s = t ? jp_seq2[i] : jp_seq[i];
                uint16_t c = i >= len ? C_PANEL : s < 128u ? SEC[t ? CO_ORANGE : CO_BLUE] : s == SEQ_TIE ? C_DIM
                           : HB_RGB(40, 40, 46);
                H->cv_rect(x, 9 + (int32_t)t * 10, 12, 8, c);
                if (jp_seq_rec == t + 1u && i == at) H->cv_rect(x, 9 + (int32_t)t * 10, 12, 8, C_LED);
            }
            if (i == at) H->cv_rect(x, 3, 12, 3, C_LED);
            if (!(i & 3u)) H->cv_rect(x, 29, 2, 3, C_DIM);
        }
    } else {
        e = put_u(b, jp_active); e = put(e, "/"); e = put_u(e, jp_nv_now);
        e = put(e, " VOICES  CPU "); e = put_u(e, jp_cpu); put(e, "%");
        H->cv_text(6, 9, 0, b, C_DIM);
        e = put(b, "OCT ");
        if (octave >= 2u) { e = put(e, "+"); put_u(e, octave - 2u); } else { e = put(e, "-"); put_u(e, 2u - octave); }
        H->cv_text(234 - H->text_w(0, b), 9, 0, b, C_DIM);
    }
    H->cv_blit(0, 206);
}

static uint32_t vals_sig(void)
{
    const jp_page_t *pg = &PAGES[page];
    return (uint32_t)valget(&pg->k[0]) | (uint32_t)valget(&pg->k[1]) << 8 | (uint32_t)valget(&pg->k[2]) << 16
         | (uint32_t)valget(&pg->k[3]) << 24;
}

static void draw(uint32_t full)
{
    uint32_t sig, now = H->ms(), j;
    for (j = 0; j < NP; j++) jp_p2[j] = lib[jp_g[G_LOWER] % JP_PATCHES].p[j];   /* the LOWER patch, as saved */
    if (full) {
        draw_header();
        sh_patch = sh_page = sh_vals = sh_status = sh_bottom = ~0u;
        key_leds_shown = key_glow_shown = 0xFFFFFFFFu;
    }
    sig = (save_mode ? 0x100u + save_to : cur) | (uint32_t)edited << 9 | (uint32_t)jp_arp_on << 10
        | (uint32_t)jp_seq_play << 11 | (uint32_t)(jp_seq_rec ? 1u : 0u) << 12 | (uint32_t)(jp_p[P_ASSIGN] & 7u) << 13
        | (uint32_t)(jp_g[G_KEYMODE] & 3u) << 16 | (uint32_t)(jp_g[G_LOWER] & 63u) << 18;
    if (sig != sh_patch) { sh_patch = sig; draw_patch(); leds(); }
    if (page != sh_page) { sh_page = page; sh_vals = ~0u; draw_page(); leds(); }
    sig = vals_sig();
    if (sig != sh_vals) { sh_vals = sig; draw_knobs(); }
    sig = (uint32_t)jp_active | (uint32_t)jp_nv_now << 4 | (uint32_t)(jp_cpu / 4u) << 8
        | (uint32_t)(page == PG_SEQ || jp_seq_rec) << 16 | (uint32_t)jp_seq_pos_shown << 17
        | (uint32_t)rec_pos << 25 | (uint32_t)octave << 29 | (msg && now < msg_until ? 1u << 31 : 0u);
    if (DRUM_PAGE && !jp_seq_rec) {
        uint32_t row = jp_g[G_DRINST] < DR_ROWS ? jp_g[G_DRINST] : 0u;
        sig = 0x80000000u | (uint32_t)(jp_dr_step_shown & 31u) | row << 5 | (uint32_t)(jp_g[G_PATTERN] & 7u) << 9
            | (uint32_t)jp_pat[jp_g[G_PATTERN] % DR_PATTERNS][row] << 12 | (msg && now < msg_until ? 1u << 30 : 0u);
    }
    if (sig != sh_bottom || (page == PG_SEQ && (now & 0x3FFu) < 20u)) { sh_bottom = sig; draw_bottom(); }
    key_leds();
    if (settings_due && (int32_t)(now - settings_due) >= 0) { settings_due = 0; settings_save(); }
}

/* ------------------------------------------------------------------------------------------------ start */
static void defaults(void)
{
    static const uint8_t G0[NG] = {AR_UP, 0, 80, 0, 15, 70, 0, 0, VO_AUTO, 64, 1, 60, 0, PM_BOTH, 100, 80, DR_BD,
                                   KM_WHOLE, 4, 60, 1};
    static const uint8_t SEQ0[JP_STEPS] = {48, 48, 60, SEQ_REST, 48, 55, 58, 60, 46, 46, 58, SEQ_REST, 46, 53,
                                           55, 58};
    uint32_t i;
    for (i = 0; i < NG; i++) jp_g[i] = G0[i];
    for (i = 0; i < JP_STEPS; i++) { jp_seq[i] = SEQ0[i]; jp_seq2[i] = SEQ_REST; }
    drums_defaults();
}

static void library_defaults(void)
{
    uint32_t i, j;
    for (i = 0; i < JP_PATCHES; i++) {
        if (i < JP_FACTORY) { lib[i] = FACTORY[i]; continue; }
        lib[i] = INIT_PATCH;
        for (j = 0; j < JP_NAME; j++) lib[i].name[j] = 0;
        put(lib[i].name, "USER ");
        lib[i].name[5] = (char)('0' + i / 8u + 1u);
        lib[i].name[6] = (char)('0' + i % 8u + 1u);
    }
}

static int init(const hb_host_t *h)
{
    jp_settings_t s;
    uint32_t i;
    H = h;
    eng_init();
    defaults();
    library_defaults();
    if (HB_HOST_HAS(H, bank_load) && H->bank_load(&libfile, sizeof libfile) == 0 && libfile.magic == LIB_MAGIC)
        for (i = 0; i < JP_PATCHES; i++) lib[i] = libfile.p[i];
    cur = 0;
    if (H->load(&s, sizeof s) == 0 && s.magic == SET_MAGIC) {
        for (i = 0; i < NG; i++) jp_g[i] = s.g[i];
        for (i = 0; i < JP_STEPS; i++) { jp_seq[i] = s.seq[i]; jp_seq2[i] = s.seq2[i]; }
        for (i = 0; i < DR_PATTERNS * DR_ROWS; i++) jp_pat[i / DR_ROWS][i % DR_ROWS] = s.pat[i / DR_ROWS][i % DR_ROWS];
        for (i = 0; i < 3u * DR_N; i++) jp_dk[i / DR_N][i % DR_N] = s.dk[i / DR_N][i % DR_N];
        cur = s.cur % JP_PATCHES;
        octave = s.octave <= 4u ? s.octave : 2u;
        page = s.page < NPAGES ? s.page : 0u;
    }
    patch_load(cur);
    settings_due = 0;
    two_cores = HB_HOST_HAS(H, c1_wait) ? 1u : 0u;
    H->log("uranus88: ready");
    return 0;
}

static void release(void)
{
    uint32_t i;
    for (i = 0; i < JP_NV; i++) {
        jv[i].gate = jv[i].lgate = 0;
        jv[i].st1 = jv[i].st2 = 0;
        jv[i].e1 = jv[i].e2 = jv[i].amp = 0.0f;
        jv[i].nev = 0;
    }
    LA[0].nstack = LA[1].nstack = 0;
    jp_seq_play = jp_seq_rec = 0;
    for (i = 0; i < 27u; i++) key_note[i] = 0;
}

HB_DESC = {HB_ABI, "URANUS 88", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
