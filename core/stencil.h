/*
 * stencil -- a rhythmic gate mask for a stereo audio stream.
 *
 * Takes sustained sound (pads, strings, drones) and cuts it into rhythm: a
 * step pattern opens and closes a gate on each channel, and a slice envelope
 * (attack, gate length, release, curve) shapes every opening. The gate is a
 * VCA, a 12 dB/oct lowpass that carves the highs away to "close", or a blend
 * of the two. In DUCK polarity the envelope pulls the level DOWN on each
 * open step instead, which is the sloppy pumping-sidechain sound.
 *
 * The pattern comes from one of three sources:
 *   BANK    16 factory step patterns plus one user pattern sent over USB
 *   EUCLID  K hits spread over N steps
 *   MORSE   a word, typed on the Patch or sent from the web page, spelled
 *           in Morse: dit = 1 step open, dah = 3 open, 1 closed between
 *           elements, 3 between letters, 7 between words (and at the loop
 *           point, so the word repeats as a sentence would)
 *
 * The left channel plays the pattern; the right plays it rotated by
 * R OFFSET steps (0 = the same).
 *
 * Plain C99, no hardware: both front-ends (patch/, alchemy/) and the native
 * tests (test/) drive it the same way. Single-threaded except for the one
 * hand-off documented at st_commit_pattern().
 */
#ifndef STENCIL_H
#define STENCIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_MAX_UNITS   1024u  /* longest pattern, in steps (Morse units)   */
#define ST_BANK_STEPS  16u    /* factory patterns are 16 steps             */
#define ST_BANK_COUNT  17u    /* 16 factory + 1 user                        */
#define ST_USER_PATTERN 16u   /* bank index of the user pattern             */
#define ST_MAX_STEPS   32u    /* BANK / EUCLID length limit                 */
#define ST_WORDS       8u     /* stored Morse words                         */
#define ST_WORD_MAX    48u    /* characters per word, without the NUL      */

typedef enum { ST_SRC_BANK = 0, ST_SRC_EUCLID = 1, ST_SRC_MORSE = 2, ST_SRC_COUNT } st_source_t;
typedef enum { ST_POL_GATE = 0, ST_POL_DUCK = 1 } st_polarity_t;

/* Step lengths, as steps per quarter note. */
typedef enum {
    ST_RATE_1_32 = 0, ST_RATE_1_16T, ST_RATE_1_16, ST_RATE_1_8T, ST_RATE_1_8,
    ST_RATE_1_4T, ST_RATE_1_4, ST_RATE_1_2, ST_RATE_1_1, ST_RATE_COUNT
} st_rate_t;
extern const char *const st_rate_name[ST_RATE_COUNT];
extern const float       st_rate_steps_per_beat[ST_RATE_COUNT];

/* How a Morse word sits in the bar (4/4). FREE: one step per unit, the
 * loop is as long as the word. PAD: one step per unit, the closing gap
 * stretched to the next bar line. BARS_n: the whole word, closing gap
 * included, stretched to loop in exactly n bars (units leave the grid). */
typedef enum {
    ST_FIT_FREE = 0, ST_FIT_PAD, ST_FIT_1_BAR, ST_FIT_2_BARS, ST_FIT_4_BARS, ST_FIT_8_BARS, ST_FIT_COUNT
} st_fit_t;
extern const char *const st_fit_name[ST_FIT_COUNT];

typedef struct {
    /* pattern */
    uint8_t source;      /* st_source_t                                         */
    uint8_t bank;        /* 0..16 (16 = user)                                    */
    uint8_t steps;       /* BANK / EUCLID length, 1..32                          */
    uint8_t hits;        /* EUCLID hits, 0..steps                                */
    uint8_t word;        /* MORSE word slot, 0..7                                */
    int8_t  r_offset;    /* right channel rotation in steps, -16..+16            */
    /* timing */
    uint8_t rate;        /* st_rate_t                                            */
    float   swing;       /* 0..0.5: odd steps start this fraction of a step late */
    float   bpm;         /* free-running tempo                                   */
    /* slice envelope, in fractions of one step */
    float   attack;      /* 0..1                                                 */
    float   gate;        /* 0.05..1: how long the step holds open                */
    float   release;     /* 0..4: a long release spills into the next steps      */
    float   curve;       /* -1 (snappy) .. 0 (linear) .. +1 (slow start)         */
    /* the gate */
    float   depth;       /* 0..1: 1 = closed steps are silent                    */
    float   filter;      /* 0 = VCA gate, 1 = lowpass gate, between = both       */
    float   resonance;   /* 0..0.9, lowpass only                                 */
    uint8_t polarity;    /* st_polarity_t                                        */
    float   mix;         /* 0 dry .. 1 wet                                       */
    uint8_t fit;         /* st_fit_t, MORSE only                                 */
} st_params_t;

void st_default_params(st_params_t *p);

/* One built pattern: a bit per step, per channel. */
typedef struct {
    uint32_t len;                         /* steps, 1..ST_MAX_UNITS */
    uint8_t  tie;                         /* runs of open steps hold through (Morse) */
    uint16_t beats;                       /* >0: the pattern spans this many beats,
                                             units stretched to fit (Morse BARS_n)   */
    uint32_t bits[2][ST_MAX_UNITS / 32u];
} st_pattern_t;

static inline int st_pattern_open(const st_pattern_t *pt, int ch, uint32_t i)
{
    i %= pt->len;
    return (int)((pt->bits[ch][i >> 5] >> (i & 31u)) & 1u);
}

/* Words and the user pattern: what the USB commands write and the presets keep. */
typedef struct {
    char     words[ST_WORDS][ST_WORD_MAX + 1];
    uint32_t user_mask;   /* bit i = step i+1 open */
    uint8_t  user_len;    /* 1..32 */
} st_store_t;

void st_default_store(st_store_t *s);

/* ---- pattern building (any thread; touches nothing shared) ---------------- */

/* Morse for one character, as a string of '.' and '-'; NULL if it has none.
 * Letters are case-insensitive. */
const char *st_morse_char(char c);

/* Spell text into bits (1 = open), ITU timing, with a 7-unit word gap at the
 * end so the loop reads as repeated words. Characters with no Morse are
 * skipped; runs of spaces collapse. Returns the length in units (0 for text
 * with nothing to spell). Writes at most max_units bits. */
uint32_t st_morse_encode(const char *text, uint32_t *bits, uint32_t max_units);

/* Bjorklund-style even spread: step i of n is a hit when (i*k) % n < k. */
int st_euclid_hit(uint32_t i, uint32_t k, uint32_t n);

/* Factory pattern, as 16 chars of 'x' (open) and '.' (closed). */
extern const char *const st_bank_pattern[ST_BANK_STEPS];

/* Build the pattern the params + store describe. */
void st_build_pattern(const st_params_t *p, const st_store_t *s, st_pattern_t *out);

/* ---- the engine --------------------------------------------------------- */

typedef struct {
    float x1, x2;  /* TPT SVF state (ic1eq, ic2eq) */
} st_svf_t;

typedef struct {
    float        sr;
    st_params_t  p;
    st_pattern_t pat[2];
    volatile int cur;          /* which pat[] the audio side reads */
    volatile int swap_req;     /* 1 = pat[cur ^ 1] is ready        */

    /* clock */
    double   pos;              /* position in steps since reset     */
    double   edge_pos;         /* where the last clock edge put us  */
    uint32_t frames_since_edge;
    uint32_t edge_period;      /* frames between the last two edges */
    uint8_t  edge_ppqn;
    uint8_t  have_edge;
    uint8_t  locked;
    uint8_t  reset_armed;      /* next edge is beat 1             */
    float    ext_bpm;

    /* per channel */
    float    env[2];
    float    gain[2];          /* last applied amplitude, for meters */
    st_svf_t svf[2];
    int32_t  last_step[2];
    uint32_t step_starts;      /* count of open-step starts on the left */
} stencil_t;

void st_init(stencil_t *st, float sample_rate);
void st_set_params(stencil_t *st, const st_params_t *p);   /* audio thread / between blocks */

/*
 * Pattern hand-off between a control thread and the audio thread:
 *   control: if (st_pattern_writable(st)) { st_build_pattern(.., st_back_pattern(st));
 *                                           st_commit_pattern(st); }
 *   audio:   st_process() swaps at the top of the next block.
 * One producer, one consumer. The tests call both from one thread.
 */
int           st_pattern_writable(const stencil_t *st);
st_pattern_t *st_back_pattern(stencil_t *st);
void          st_commit_pattern(stencil_t *st);
const st_pattern_t *st_front_pattern(const stencil_t *st);

/* A rising edge on the clock input, ppqn pulses per quarter note (1, 2, 4,
 * 24 for MIDI clock). Call before st_process() for the block it fell in. */
void st_clock_edge(stencil_t *st, uint8_t ppqn);
/* Back to step 1 (see stencil.c for how it lines up with the clock). */
void st_reset(stencil_t *st);

/* Process n frames. out_* may alias in_*. hole_* (may be NULL) get what the
 * gate took away: the input minus the gated signal before the mix. */
void st_process(stencil_t *st, const float *in_l, const float *in_r,
                float *out_l, float *out_r, float *hole_l, float *hole_r, uint32_t n);

/* ---- readouts ----------------------------------------------------------- */
float    st_bpm(const stencil_t *st);           /* the tempo in use     */
int      st_locked(const stencil_t *st);        /* tempo from the clock */
uint32_t st_step(const stencil_t *st);          /* current left step, 0..len-1 */
float    st_env(const stencil_t *st, int ch);   /* 0..1 envelope         */
float    st_gain(const stencil_t *st, int ch);  /* 0..1 last amplitude   */

/* ---- USB command payloads (shared by the Patch's SysEx and the Lab's HostLink) */

enum {
    ST_CMD_SET_WORD = 1,   /* u8 slot, then ASCII bytes (<= 48)              */
    ST_CMD_SET_USER = 2,   /* u8 len (1..32), u8 mask[5] 7-bit LSB-first      */
    ST_CMD_SELECT   = 3,   /* u8 source, u8 index (bank / hits / word slot)   */
    ST_CMD_SET_FIT  = 4,   /* u8 st_fit_t                                     */
};

/* Apply one command. Returns 0 on success, nonzero for a malformed payload
 * (and then changes nothing). The caller rebuilds the pattern and saves. */
int st_apply_cmd(st_store_t *s, st_params_t *p, uint8_t cmd,
                 const uint8_t *payload, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* STENCIL_H */
