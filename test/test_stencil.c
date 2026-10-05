/* Native tests for the stencil engine. `make -C test` runs them. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stencil.h"

static int g_checks, g_fail;
#define CHECK(c)                                                                   \
    do {                                                                           \
        g_checks++;                                                                \
        if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)
#define CHECKF(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

#define SR 48000.0f

static uint32_t bits[ST_MAX_UNITS / 32];

static uint32_t enc(const char *t)
{
    memset(bits, 0, sizeof bits);
    return st_morse_encode(t, bits, ST_MAX_UNITS);
}
static int bit(uint32_t i) { return (int)((bits[i >> 5] >> (i & 31)) & 1u); }

/* "1" / "0" string of the first n bits */
static const char *bitstr(uint32_t n)
{
    static char b[256];
    for (uint32_t i = 0; i < n && i < 255; i++) b[i] = bit(i) ? '1' : '0';
    b[n < 255 ? n : 255] = 0;
    return b;
}

static void test_morse(void)
{
    CHECK(enc("E") == 8);
    CHECK(strcmp(bitstr(8), "10000000") == 0);
    CHECK(enc("T") == 10);
    CHECK(strcmp(bitstr(10), "1110000000") == 0);
    /* S ... = 5, O --- = 11, gaps 3 + 3, tail 7 */
    CHECK(enc("SOS") == 34);
    CHECK(strcmp(bitstr(34), "1010100011101110111000101010000000") == 0);
    /* The standard word: PARIS is 50 units with its word gap. */
    CHECK(enc("PARIS") == 50);
    CHECK(enc("paris") == 50);
    /* word gap 7 between words, runs of spaces collapse, edges ignored */
    CHECK(enc("A B") == 5 + 7 + 9 + 7);
    CHECK(enc("  A   B  ") == 5 + 7 + 9 + 7);
    /* characters with no Morse are skipped (and keep the letter gap) */
    CHECK(enc("A#B") == 5 + 3 + 9 + 7);
    CHECK(enc("") == 0);
    CHECK(enc("   ") == 0);
    CHECK(enc("###") == 0);
    CHECK(st_morse_char('?') && strcmp(st_morse_char('?'), "..--..") == 0);
    CHECK(strcmp(st_morse_char('0'), "-----") == 0);
    CHECK(st_morse_char('~') == 0);
    /* every letter and digit has a code */
    for (char c = 'A'; c <= 'Z'; c++) CHECK(st_morse_char(c) != 0);
    for (char c = '0'; c <= '9'; c++) CHECK(st_morse_char(c) != 0);
    /* overflow: a very long text truncates at the buffer and says so */
    char big[600];
    memset(big, '0', sizeof big - 1);
    big[sizeof big - 1] = 0;
    CHECK(enc(big) == ST_MAX_UNITS);
}

static void test_euclid(void)
{
    int n = 0;
    for (uint32_t i = 0; i < 16; i++) n += st_euclid_hit(i, 5, 16);
    CHECK(n == 5);
    n = 0;
    for (uint32_t i = 0; i < 8; i++) n += st_euclid_hit(i, 3, 8);
    CHECK(n == 3);
    CHECK(st_euclid_hit(0, 3, 8) == 1); /* starts on the one */
    CHECK(st_euclid_hit(3, 0, 8) == 0);
    for (uint32_t i = 0; i < 8; i++) CHECK(st_euclid_hit(i, 8, 8) == 1);
    CHECK(st_euclid_hit(0, 1, 0) == 0);
}

static void test_build(void)
{
    st_params_t p;
    st_store_t  s;
    st_pattern_t pt;
    st_default_params(&p);
    st_default_store(&s);

    p.source = ST_SRC_BANK; p.bank = 1; p.steps = 16;           /* eighths */
    st_build_pattern(&p, &s, &pt);
    CHECK(pt.len == 16 && !pt.tie);
    for (uint32_t i = 0; i < 16; i++) CHECK(st_pattern_open(&pt, 0, i) == (int)((i & 1) == 0));

    p.steps = 32;                                                 /* repeats */
    st_build_pattern(&p, &s, &pt);
    CHECK(pt.len == 32 && st_pattern_open(&pt, 0, 16) && !st_pattern_open(&pt, 0, 17));

    p.steps = 0;                                                  /* clamped */
    st_build_pattern(&p, &s, &pt);
    CHECK(pt.len == 1);

    p.steps = 16; p.r_offset = 1;                                 /* R = L delayed */
    st_build_pattern(&p, &s, &pt);
    for (uint32_t i = 0; i < 16; i++) CHECK(st_pattern_open(&pt, 1, i) == st_pattern_open(&pt, 0, (i + 15) % 16));
    p.r_offset = -17;                                             /* wraps */
    st_build_pattern(&p, &s, &pt);
    for (uint32_t i = 0; i < 16; i++) CHECK(st_pattern_open(&pt, 1, i) == st_pattern_open(&pt, 0, (i + 1) % 16));
    p.r_offset = 0;

    p.bank = ST_USER_PATTERN;                                     /* user */
    s.user_mask = 0x5u; s.user_len = 3;
    st_build_pattern(&p, &s, &pt);
    CHECK(pt.len == 3 && st_pattern_open(&pt, 0, 0) && !st_pattern_open(&pt, 0, 1) && st_pattern_open(&pt, 0, 2));

    p.source = ST_SRC_EUCLID; p.hits = 40; p.steps = 12;          /* hits clamp */
    st_build_pattern(&p, &s, &pt);
    CHECK(pt.len == 12);
    for (uint32_t i = 0; i < 12; i++) CHECK(st_pattern_open(&pt, 0, i));

    p.source = ST_SRC_MORSE; p.word = 2;                          /* PARIS */
    st_build_pattern(&p, &s, &pt);
    CHECK(pt.len == 50 && pt.tie);
    s.words[3][0] = 0;                                            /* empty word: open */
    p.word = 3;
    st_build_pattern(&p, &s, &pt);
    CHECK(pt.len == 1 && st_pattern_open(&pt, 0, 0));

    for (unsigned b = 0; b < ST_BANK_STEPS; b++) CHECK(strlen(st_bank_pattern[b]) == 16);
}

/* engine helpers ------------------------------------------------------------ */

static stencil_t S;
static st_store_t STORE;
#define BLK 48
static float il[BLK], ir[BLK], ol[BLK], orr[BLK], hl[BLK], hr[BLK];

static void setup(const st_params_t *p)
{
    st_init(&S, SR);
    st_set_params(&S, p);
    st_default_store(&STORE);
    st_build_pattern(p, &STORE, st_back_pattern(&S));
    st_commit_pattern(&S);
}

/* run `frames` of DC 1.0; record the left output at absolute frame `at` */
static uint64_t g_frame;
static void run_dc(uint32_t frames, float *probe_out, uint64_t probe_at)
{
    for (int i = 0; i < BLK; i++) il[i] = ir[i] = 1.0f;
    while (frames)
    {
        uint32_t n = frames < BLK ? frames : BLK;
        st_process(&S, il, ir, ol, orr, hl, hr, n);
        if (probe_out && probe_at >= g_frame && probe_at < g_frame + n) *probe_out = ol[probe_at - g_frame];
        g_frame += n;
        frames -= n;
    }
}

static float out_at(uint64_t frame)
{
    float v = -99.0f;
    run_dc((uint32_t)(frame - g_frame + 1), &v, frame);
    return v;
}

static void test_gate(void)
{
    /* 120 BPM, 1/16: a step is 0.125 s = 6000 frames. */
    st_params_t p;
    st_default_params(&p);
    p.bank = 1;            /* x.x.x.x. */
    p.attack = 0.0f; p.release = 0.0f; p.gate = 0.5f; p.depth = 1.0f;
    setup(&p);
    g_frame = 0;
    CHECKF(out_at(1500), 1.0f, 1e-4);     /* step 1, a quarter in: open        */
    CHECKF(out_at(4500), 0.0f, 1e-4);     /* step 1, past the gate: closed      */
    CHECKF(out_at(9000), 0.0f, 1e-4);     /* step 2, closed step: silent        */
    CHECKF(out_at(13500), 1.0f, 1e-4);    /* step 3 open                        */
    CHECK(S.step_starts >= 2);

    /* depth 0.5 leaves half the level in the gaps */
    p.depth = 0.5f;
    setup(&p);
    g_frame = 0;
    CHECKF(out_at(9000), 0.5f, 1e-4);

    /* mix 0 is the dry signal, bit for bit */
    p.depth = 1.0f; p.mix = 0.0f;
    setup(&p);
    g_frame = 0;
    CHECKF(out_at(9000), 1.0f, 0.0);

    /* holes + gated = input */
    p.mix = 1.0f;
    setup(&p);
    g_frame = 0;
    run_dc(9001, NULL, 0);
    for (int i = 0; i < BLK; i++) CHECKF(ol[i] + hl[i], 1.0f, 1e-6);

    /* attack takes the time it says: a quarter step = 1500 frames */
    p.attack = 0.25f; p.gate = 1.0f; p.bank = 0;
    setup(&p);
    g_frame = 0;
    CHECKF(out_at(750), 0.5f, 0.01f);
    CHECKF(out_at(1600), 1.0f, 1e-4);
}

static void test_duck(void)
{
    st_params_t p;
    st_default_params(&p);
    p.bank = 3;            /* quarters: x... */
    p.polarity = ST_POL_DUCK;
    p.attack = 0.0f; p.gate = 0.1f; p.release = 2.0f; p.depth = 1.0f;
    setup(&p);
    g_frame = 0;
    CHECK(out_at(300) < 0.01f);           /* ducked right on the beat         */
    float mid = out_at(600 + 6000);       /* one step into the 2-step release */
    CHECK(mid > 0.4f && mid < 0.6f);      /* half way back                    */
    CHECKF(out_at(23900), 1.0f, 1e-3);    /* recovered before the next beat   */
    CHECK(out_at(24300) < 0.01f);         /* and ducks again                  */
}

static float rms_tone(float hz, uint32_t from, uint32_t len)
{
    double acc = 0.0;
    uint32_t done = 0;
    while (done < from + len)
    {
        for (int i = 0; i < BLK; i++)
            il[i] = ir[i] = sinf(2.0f * 3.14159265f * hz * (float)(g_frame + (uint64_t)i) / SR);
        st_process(&S, il, ir, ol, orr, NULL, NULL, BLK);
        for (int i = 0; i < BLK; i++)
            if (done + (uint32_t)i >= from && done + (uint32_t)i < from + len) acc += ol[i] * ol[i];
        g_frame += BLK;
        done += BLK;
    }
    return (float)sqrt(acc / len) * 1.41421356f;
}

static void test_filter(void)
{
    st_params_t p;
    st_default_params(&p);
    p.bank = 1; p.attack = 0.0f; p.release = 0.0f; p.gate = 0.5f;
    p.filter = 1.0f; p.resonance = 0.0f;
    setup(&p);
    g_frame = 0;
    /* open quarter of step 1: a 5 kHz tone passes */
    CHECK(rms_tone(5000.0f, 600, 2400) > 0.8f);
    /* closed step 2: 5 kHz is carved away */
    setup(&p);
    g_frame = 0;
    CHECK(rms_tone(5000.0f, 7000, 4000) < 0.01f);
    /* ... while a low tone survives better than the high one did */
    setup(&p);
    g_frame = 0;
    CHECK(rms_tone(25.0f, 7000, 3840) > 0.3f);
}

static void test_clock(void)
{
    st_params_t p;
    st_default_params(&p);
    p.bpm = 90.0f;
    setup(&p);
    g_frame = 0;
    CHECK(!st_locked(&S));
    CHECKF(st_bpm(&S), 90.0f, 1e-3);

    /* 100 BPM at 24 ppqn: 1200 frames per pulse */
    for (int k = 0; k < 24 * 8; k++)
    {
        st_clock_edge(&S, 24);
        run_dc(1200, NULL, 0);
    }
    CHECK(st_locked(&S));
    CHECKF(st_bpm(&S), 100.0f, 0.01f);
    /* 1/16 = 4 steps a beat: 8 beats = step 32 = step 0 of 16, after the
     * lock-in pulse; allow the snap to land either side */
    uint32_t st = st_step(&S);
    CHECK(st == 15 || st == 0 || st == 1);

    /* reset right after an edge makes that edge step 1 */
    st_clock_edge(&S, 24);
    run_dc(48, NULL, 0);
    st_reset(&S);
    CHECK(st_step(&S) == 0);
    for (int k = 0; k < 6; k++) { run_dc(1200 - (k == 0 ? 48 : 0), NULL, 0); st_clock_edge(&S, 24); }
    CHECK(st_step(&S) == 1);              /* one beat-quarter later: step 2 (index 1) */

    /* a reset mid-beat holds step 1 until the next edge */
    run_dc(600, NULL, 0);
    st_reset(&S);
    run_dc(500, NULL, 0);
    CHECK(st_step(&S) == 0 && S.pos == 0.0);
    st_clock_edge(&S, 24);
    CHECK(S.pos == 0.0);

    /* bounce: an edge 1 ms after another is ignored */
    run_dc(1200, NULL, 0);
    st_clock_edge(&S, 24);
    uint32_t period = S.edge_period;
    run_dc(48, NULL, 0);
    st_clock_edge(&S, 24);
    CHECK(S.edge_period == period);

    /* clock stops: after 2 s free-running at the TEMPO setting again */
    run_dc((uint32_t)(2.5f * SR), NULL, 0);
    CHECK(!st_locked(&S));
    CHECKF(st_bpm(&S), 90.0f, 1e-3);

    /* 1 ppqn quarter-note clock locks too */
    for (int k = 0; k < 4; k++) { st_clock_edge(&S, 1); run_dc(24000, NULL, 0); }
    CHECK(st_locked(&S));
    CHECKF(st_bpm(&S), 120.0f, 0.01f);
}

static void test_swing(void)
{
    st_params_t p;
    st_default_params(&p);
    p.swing = 0.25f;
    setup(&p);
    g_frame = 0;
    /* step 2 starts a quarter step late: pos 1.1 is still step 1 */
    run_dc(6000 + 600, NULL, 0);
    CHECK(st_step(&S) == 1); /* pos ~1.1, raw step index; the swing is in the gate */
    CHECK(S.last_step[0] == 0);
    run_dc(1200, NULL, 0); /* pos ~1.3 */
    CHECK(S.last_step[0] == 1);
}

static void test_morse_tie(void)
{
    /* T = one dah: three units open, held through even with a short gate */
    st_params_t p;
    st_default_params(&p);
    p.source = ST_SRC_MORSE; p.word = 0;
    p.attack = 0.0f; p.release = 0.0f; p.gate = 0.3f;
    setup(&p);
    strcpy(STORE.words[0], "T");
    st_build_pattern(&p, &STORE, st_back_pattern(&S));
    st_commit_pattern(&S);
    g_frame = 0;
    CHECKF(out_at(6000 + 4500), 1.0f, 1e-4);      /* unit 2, past the gate: held */
    CHECKF(out_at(12000 + 1000), 1.0f, 1e-4);     /* unit 3, inside the gate      */
    CHECKF(out_at(12000 + 4500), 0.0f, 1e-4);     /* unit 3 past the gate: closes */
    CHECKF(out_at(18000 + 3000), 0.0f, 1e-4);     /* the word gap                 */
    CHECKF(out_at(60000 + 1000), 1.0f, 1e-4);     /* loops after 10 units         */
}

static void test_cmds(void)
{
    st_store_t s;
    st_params_t p;
    st_default_store(&s);
    st_default_params(&p);
    const uint8_t w[] = {5, 'h', 'i', '\n', ' ', 't', 'i', 'm'};
    CHECK(st_apply_cmd(&s, &p, ST_CMD_SET_WORD, w, sizeof w) == 0);
    CHECK(strcmp(s.words[5], "HI TIM") == 0);
    const uint8_t bad_slot[] = {8, 'a'};
    CHECK(st_apply_cmd(&s, &p, ST_CMD_SET_WORD, bad_slot, 2) != 0);
    uint8_t longw[1 + 80];
    longw[0] = 1;
    memset(longw + 1, 'a', 80);
    CHECK(st_apply_cmd(&s, &p, ST_CMD_SET_WORD, longw, sizeof longw) == 0);
    CHECK(strlen(s.words[1]) == ST_WORD_MAX);

    /* user pattern: 32 steps, alternate bits across the 7-bit packing */
    const uint8_t u[] = {32, 0x55, 0x2A, 0x55, 0x2A, 0x05};
    CHECK(st_apply_cmd(&s, &p, ST_CMD_SET_USER, u, sizeof u) == 0);
    CHECK(s.user_len == 32 && s.user_mask == 0x55555555u);
    const uint8_t u_bad[] = {0, 0, 0, 0, 0, 0};
    CHECK(st_apply_cmd(&s, &p, ST_CMD_SET_USER, u_bad, sizeof u_bad) != 0);
    CHECK(s.user_len == 32);

    const uint8_t sel[] = {ST_SRC_MORSE, 3};
    CHECK(st_apply_cmd(&s, &p, ST_CMD_SELECT, sel, 2) == 0);
    CHECK(p.source == ST_SRC_MORSE && p.word == 3);
    const uint8_t sel_bad[] = {ST_SRC_BANK, 17};
    CHECK(st_apply_cmd(&s, &p, ST_CMD_SELECT, sel_bad, 2) != 0);
    CHECK(p.source == ST_SRC_MORSE); /* a bad select changes nothing */
    CHECK(st_apply_cmd(&s, &p, 99, sel, 2) != 0);
}

static void test_stability(void)
{
    /* noise through every mode for 20 s: finite, bounded */
    st_params_t p;
    st_default_params(&p);
    uint32_t rng = 1;
    int bad = 0;
    for (int mode = 0; mode < 6; mode++)
    {
        p.filter = mode * 0.2f;
        p.resonance = 0.9f;
        p.polarity = (uint8_t)(mode & 1);
        p.swing = 0.5f;
        p.rate = (uint8_t)mode;
        p.curve = mode < 3 ? -1.0f : 1.0f;
        setup(&p);
        for (int b = 0; b < (int)(20 * SR / 6 / BLK); b++)
        {
            for (int i = 0; i < BLK; i++)
            {
                rng = rng * 1664525u + 1013904223u;
                il[i] = ((float)(rng >> 8) / 8388608.0f) - 1.0f;
                ir[i] = -il[i];
            }
            st_process(&S, il, ir, ol, orr, hl, hr, BLK);
            for (int i = 0; i < BLK; i++)
                if (!isfinite(ol[i]) || !isfinite(orr[i]) || fabsf(ol[i]) > 8.0f) bad++;
        }
    }
    CHECK(bad == 0);
}

static void test_swap(void)
{
    st_params_t p;
    st_default_params(&p);
    setup(&p);
    run_dc(BLK, NULL, 0);                  /* takes the first commit */
    CHECK(st_pattern_writable(&S));
    p.source = ST_SRC_MORSE;
    st_build_pattern(&p, &STORE, st_back_pattern(&S));
    st_commit_pattern(&S);
    CHECK(!st_pattern_writable(&S));
    CHECK(st_front_pattern(&S)->len == 16); /* not yet */
    run_dc(BLK, NULL, 0);
    CHECK(st_pattern_writable(&S));
    CHECK(st_front_pattern(&S)->tie == 1);
}

int main(void)
{
    test_morse();
    test_euclid();
    test_build();
    test_gate();
    test_duck();
    test_filter();
    test_clock();
    test_swing();
    test_morse_tie();
    test_cmds();
    test_stability();
    test_swap();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
