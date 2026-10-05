/* stencil -- see stencil.h for what it does and the rules it keeps. */
#include "stencil.h"

#include <math.h>
#include <string.h>

const char *const st_fit_name[ST_FIT_COUNT] = {
    "Free", "Pad", "1 bar", "2 bars", "4 bars", "8 bars"};
const char *const st_rate_name[ST_RATE_COUNT] = {
    "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1/1"};
const float st_rate_steps_per_beat[ST_RATE_COUNT] = {
    8.0f, 6.0f, 4.0f, 3.0f, 2.0f, 1.5f, 1.0f, 0.5f, 0.25f};

/* 16 steps each, 'x' = open. Index 16 is the user pattern (st_store_t). */
const char *const st_bank_pattern[ST_BANK_STEPS] = {
    "xxxxxxxxxxxxxxxx", /*  1 sixteenths: the trance gate            */
    "x.x.x.x.x.x.x.x.", /*  2 eighths                                 */
    ".x.x.x.x.x.x.x.x", /*  3 off-sixteenths                          */
    "x...x...x...x...", /*  4 quarters (DUCK: four-on-the-floor pump) */
    "..x...x...x...x.", /*  5 off-beats                               */
    "x..x..x.x..x..x.", /*  6 3-3-2                                   */
    "xx.xx.xx.xx.x.x.", /*  7                                         */
    "x.xx.xx.x.xx.x.x", /*  8                                         */
    "xxx.xxx.xxx.xx.x", /*  9                                         */
    "x.x..x.x.x..x.x.", /* 10                                         */
    "xx..xx..xx..x.x.", /* 11                                         */
    "x..x.x..x..x.x..", /* 12                                         */
    "xxxx..xx..xxxx.x", /* 13                                         */
    "x.xxx.x.xxx.x.xx", /* 14                                         */
    "x......x......x.", /* 15                                         */
    "xx.x.xx.xx.x.x.x", /* 16                                         */
};

void st_default_params(st_params_t *p)
{
    memset(p, 0, sizeof *p);
    p->source    = ST_SRC_BANK;
    p->bank      = 0;
    p->steps     = 16;
    p->hits      = 5;
    p->word      = 0;
    p->r_offset  = 0;
    p->rate      = ST_RATE_1_16;
    p->swing     = 0.0f;
    p->bpm       = 120.0f;
    p->attack    = 0.02f;
    p->gate      = 0.5f;
    p->release   = 0.15f;
    p->curve     = 0.0f;
    p->depth     = 1.0f;
    p->filter    = 0.0f;
    p->resonance = 0.2f;
    p->polarity  = ST_POL_GATE;
    p->mix       = 1.0f;
    p->fit       = ST_FIT_PAD;
}

static const char *const k_default_words[ST_WORDS] = {
    "STENCIL", "SOS", "PARIS", "CQ", "HELLO WORLD", "TIM", "DAISY", "LOVE"};

void st_default_store(st_store_t *s)
{
    memset(s, 0, sizeof *s);
    for (unsigned i = 0; i < ST_WORDS; i++)
        strncpy(s->words[i], k_default_words[i], ST_WORD_MAX);
    s->user_mask = 0x9249u; /* x..x..x..x..x..x */
    s->user_len  = 16;
}

/* ---- Morse ------------------------------------------------------------------ */

static const char *const k_letters[26] = {
    ".-",   "-...", "-.-.", "-..",  ".",    "..-.", "--.",  "....", "..",
    ".---", "-.-",  ".-..", "--",   "-.",   "---",  ".--.", "--.-", ".-.",
    "...",  "-",    "..-",  "...-", ".--",  "-..-", "-.--", "--.."};
static const char *const k_digits[10] = {
    "-----", ".----", "..---", "...--", "....-",
    ".....", "-....", "--...", "---..", "----."};
static const struct { char c; const char *m; } k_punct[] = {
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'\'', ".----."},
    {'!', "-.-.--"}, {'/', "-..-."},  {'(', "-.--."},  {')', "-.--.-"},
    {'&', ".-..."},  {':', "---..."}, {';', "-.-.-."}, {'=', "-...-"},
    {'+', ".-.-."},  {'-', "-....-"}, {'_', "..--.-"}, {'"', ".-..-."},
    {'$', "...-..-"}, {'@', ".--.-."}};

const char *st_morse_char(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return k_letters[c - 'A'];
    if (c >= '0' && c <= '9') return k_digits[c - '0'];
    for (unsigned i = 0; i < sizeof k_punct / sizeof k_punct[0]; i++)
        if (k_punct[i].c == c) return k_punct[i].m;
    return 0;
}

static void put_bits(uint32_t *bits, uint32_t max, uint32_t *n, uint32_t count, int on)
{
    for (uint32_t i = 0; i < count; i++, (*n)++)
    {
        if (*n >= max) continue;
        if (on) bits[*n >> 5] |= 1u << (*n & 31u);
        else    bits[*n >> 5] &= ~(1u << (*n & 31u));
    }
}

uint32_t st_morse_encode(const char *text, uint32_t *bits, uint32_t max_units)
{
    uint32_t n = 0;
    int      any = 0, space = 0;
    for (const char *t = text; t && *t; t++)
    {
        if (*t == ' ') { space = 1; continue; }
        const char *m = st_morse_char(*t);
        if (!m) continue;
        if (any) put_bits(bits, max_units, &n, space ? 7u : 3u, 0);
        space = 0;
        for (const char *e = m; *e; e++)
        {
            if (e != m) put_bits(bits, max_units, &n, 1u, 0);
            put_bits(bits, max_units, &n, *e == '-' ? 3u : 1u, 1);
        }
        any = 1;
    }
    if (!any) return 0;
    put_bits(bits, max_units, &n, 7u, 0);
    return n < max_units ? n : max_units;
}

/* ---- patterns --------------------------------------------------------------- */

int st_euclid_hit(uint32_t i, uint32_t k, uint32_t n)
{
    if (n == 0 || k == 0) return 0;
    if (k >= n) return 1;
    return ((i % n) * k) % n < k;
}

static uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void st_build_pattern(const st_params_t *p, const st_store_t *s, st_pattern_t *out)
{
    memset(out, 0, sizeof *out);
    uint32_t *L = out->bits[0];
    const uint32_t steps = clampu(p->steps, 1u, ST_MAX_STEPS);

    switch (p->source)
    {
        case ST_SRC_EUCLID:
        {
            out->len = steps;
            const uint32_t k = p->hits > steps ? steps : p->hits;
            for (uint32_t i = 0; i < steps; i++)
                if (st_euclid_hit(i, k, steps)) L[i >> 5] |= 1u << (i & 31u);
            break;
        }
        case ST_SRC_MORSE:
        {
            const unsigned w = p->word < ST_WORDS ? p->word : 0;
            out->len = st_morse_encode(s->words[w], L, ST_MAX_UNITS);
            out->tie = 1;
            if (out->len == 0) { out->len = 1; L[0] = 1u; break; } /* nothing to spell: open */
            if (p->fit == ST_FIT_PAD)
            {
                /* closed steps to the next bar line; every rate has a whole
                 * number of steps in a 4/4 bar (32 at 1/32 .. 1 at 1/1) */
                const uint32_t bar = (uint32_t)(4.0f * st_rate_steps_per_beat[p->rate < ST_RATE_COUNT ? p->rate : ST_RATE_1_16] + 0.5f);
                uint32_t padded = (out->len + bar - 1u) / bar * bar;
                while (padded > ST_MAX_UNITS && padded >= bar) padded -= bar;   /* whole bars that fit */
                if (padded >= out->len) out->len = padded;
            }
            else if (p->fit >= ST_FIT_1_BAR && p->fit < ST_FIT_COUNT)
                out->beats = (uint16_t)(4u << (p->fit - ST_FIT_1_BAR));
            break;
        }
        case ST_SRC_BANK:
        default:
            if (p->bank >= ST_USER_PATTERN)
            {
                out->len = clampu(s->user_len, 1u, ST_MAX_STEPS);
                for (uint32_t i = 0; i < out->len; i++)
                    if ((s->user_mask >> i) & 1u) L[i >> 5] |= 1u << (i & 31u);
            }
            else
            {
                out->len = steps;
                const char *pat = st_bank_pattern[p->bank];
                for (uint32_t i = 0; i < steps; i++)
                    if (pat[i % ST_BANK_STEPS] == 'x') L[i >> 5] |= 1u << (i & 31u);
            }
            break;
    }

    /* Right = left delayed by r_offset steps. */
    const int32_t len = (int32_t)out->len;
    int32_t off = p->r_offset % len;
    if (off < 0) off += len;
    for (int32_t i = 0; i < len; i++)
    {
        const int32_t j = (i - off + len) % len;
        if ((L[j >> 5] >> (j & 31)) & 1u) out->bits[1][i >> 5] |= 1u << (i & 31);
    }
}

/* ---- engine ------------------------------------------------------------------- */

void st_init(stencil_t *st, float sample_rate)
{
    memset(st, 0, sizeof *st);
    st->sr = sample_rate > 1000.0f ? sample_rate : 48000.0f;
    st_default_params(&st->p);
    st_store_t s;
    st_default_store(&s);
    st_build_pattern(&st->p, &s, &st->pat[0]);
    st->last_step[0] = st->last_step[1] = -1;
    st->ext_bpm = 120.0f;
}

void st_set_params(stencil_t *st, const st_params_t *p) { st->p = *p; }

int st_pattern_writable(const stencil_t *st) { return st->swap_req == 0; }
st_pattern_t *st_back_pattern(stencil_t *st) { return &st->pat[st->cur ^ 1]; }
void st_commit_pattern(stencil_t *st) { st->swap_req = 1; }
const st_pattern_t *st_front_pattern(const stencil_t *st) { return &st->pat[st->cur]; }

static float steps_per_beat(const stencil_t *st)
{
    return st_rate_steps_per_beat[st->p.rate < ST_RATE_COUNT ? st->p.rate : ST_RATE_1_16];
}

void st_clock_edge(stencil_t *st, uint8_t ppqn)
{
    if (ppqn == 0) ppqn = 1;
    /* contact bounce / doubled edges: nothing real is closer than 2 ms */
    if (st->have_edge && (float)st->frames_since_edge < 0.002f * st->sr) return;
    const double spe = (double)steps_per_beat(st) / (double)ppqn;
    if (ppqn != st->edge_ppqn) { st->edge_ppqn = ppqn; st->locked = 0; }

    if (st->have_edge)
    {
        st->edge_period = st->frames_since_edge;
        st->ext_bpm = 60.0f * st->sr / ((float)st->edge_period * (float)ppqn);
    }
    if (st->reset_armed)
    {
        st->edge_pos = 0.0;
        st->reset_armed = 0;
        if (st->have_edge) st->locked = 1;
    }
    else if (st->have_edge)
    {
        if (st->locked)
            st->edge_pos += spe;
        else
        {
            /* Lock on: snap to the nearest edge of the grid. */
            st->edge_pos = floor(st->pos / spe + 0.5) * spe;
            st->locked   = 1;
        }
    }
    else
        st->edge_pos = st->pos;
    st->pos = st->edge_pos;
    st->have_edge = 1;
    st->frames_since_edge = 0;
}

/*
 * Reset: back to step 1. A reset within 5 ms after a clock edge makes THAT
 * edge beat 1 (clock and reset patched from one sequencer rarely land in
 * the same sample). Later than that, the next edge is beat 1 and, while
 * clocked, the pattern holds at step 1 until it comes.
 */
void st_reset(stencil_t *st)
{
    st->pos = 0.0;
    st->edge_pos = 0.0;
    st->last_step[0] = st->last_step[1] = -1;
    st->reset_armed = !(st->have_edge && (float)st->frames_since_edge < 0.005f * st->sr);
}

float st_bpm(const stencil_t *st) { return st->locked ? st->ext_bpm : st->p.bpm; }
int   st_locked(const stencil_t *st) { return st->locked; }
float st_env(const stencil_t *st, int ch) { return st->env[ch & 1]; }
float st_gain(const stencil_t *st, int ch) { return st->gain[ch & 1]; }

uint32_t st_step(const stencil_t *st)
{
    const st_pattern_t *pt = st_front_pattern(st);
    const double pq = st->pos + 1e-7;
    if (pt->beats)
        return (uint32_t)((uint64_t)(pq / steps_per_beat(st) * (double)pt->len / (double)pt->beats) % pt->len);
    return (uint32_t)((uint64_t)pq % pt->len);
}

static inline float shape(float e, float c)
{
    if (c > 0.0f) return e + (e * e * e - e) * c;
    if (c < 0.0f) { const float u = 1.0f - e; return e + ((1.0f - u * u * u) - e) * -c; }
    return e;
}

/* Zavalishin TPT state-variable lowpass, 12 dB/oct. */
static inline float svf_lp(st_svf_t *f, float x, float g, float k)
{
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    const float v3 = x - f->x2;
    const float v1 = a1 * f->x1 + a2 * v3;
    const float v2 = f->x2 + a2 * f->x1 + a3 * v3;
    f->x1 = 2.0f * v1 - f->x1;
    f->x2 = 2.0f * v2 - f->x2;
    return v2;
}

void st_process(stencil_t *st, const float *in_l, const float *in_r,
                float *out_l, float *out_r, float *hole_l, float *hole_r, uint32_t n)
{
    if (st->swap_req) { st->cur ^= 1; st->swap_req = 0; }
    const st_pattern_t *pt = &st->pat[st->cur];
    const st_params_t  *p  = &st->p;

    const float  sr   = st->sr;
    const double spb  = (double)steps_per_beat(st);
    const double spe  = st->edge_ppqn ? spb / (double)st->edge_ppqn : spb;
    double rate;      /* steps per frame */
    if (st->locked && st->edge_period)
        rate = spe / (double)st->edge_period;
    else
    {
        float bpm = p->bpm;
        if (!(bpm >= 20.0f && bpm <= 400.0f)) bpm = 120.0f;
        rate = (double)bpm / 60.0 * spb / (double)sr;
    }

    float swing = p->swing;
    if (swing < 0.0f) swing = 0.0f;
    if (swing > 0.5f) swing = 0.5f;
    const float min_frames = 0.001f * sr;               /* 1 ms: no clicks */
    const float depth  = p->depth < 0.0f ? 0.0f : p->depth > 1.0f ? 1.0f : p->depth;
    const float filt   = p->filter < 0.0f ? 0.0f : p->filter > 1.0f ? 1.0f : p->filter;
    const float mix    = p->mix < 0.0f ? 0.0f : p->mix > 1.0f ? 1.0f : p->mix;
    float res = p->resonance < 0.0f ? 0.0f : p->resonance > 0.9f ? 0.9f : p->resonance;
    const float k_damp = 1.4142f * (1.0f - res) + 0.05f;
    const float nyq_g  = tanf(3.14159265f * 0.45f);
    const int   duck   = p->polarity == ST_POL_DUCK;

    const float *in[2]   = {in_l, in_r};
    float       *out[2]  = {out_l, out_r};
    float       *hole[2] = {hole_l, hole_r};

    for (uint32_t i = 0; i < n; i++)
    {
        /* --- where we are --- */
        st->frames_since_edge++;
        if (st->have_edge)
        {
            const uint32_t lost = st->edge_period ? 4u * st->edge_period : 0u;
            const uint32_t cap  = (uint32_t)(2.0f * sr);
            if (st->frames_since_edge > (lost > cap ? lost : cap))
            {
                st->have_edge = 0;
                st->locked    = 0;
            }
        }
        if (!(st->reset_armed && st->locked)) st->pos += rate;
        if (st->locked && st->pos > st->edge_pos + spe - 1e-6) st->pos = st->edge_pos + spe - 1e-6;

        /* a hair forward: edge_pos is a sum of fractions (six 24-ppqn
         * edges of 1/6 step make 0.99999...), and must land ON the step */
        const double   pq   = st->pos + 1e-7;
        const uint64_t pair = (uint64_t)(pq * 0.5);
        const double   ph   = pq - 2.0 * (double)pair;
        uint64_t idx;
        double   phi, steplen;
        if (pt->beats)
        {
            /* stretched: the pattern is pt->beats long, whatever its units */
            const double upb = (double)pt->len / (double)pt->beats;   /* units per beat */
            const double u   = pq / spb * upb;
            idx     = (uint64_t)u;
            phi     = u - (double)idx;
            steplen = spb / upb;                                    /* in steps */
        }
        else if (ph < 1.0 + swing) { idx = 2 * pair;     steplen = 1.0 + swing; phi = ph / steplen; }
        else                       { idx = 2 * pair + 1; steplen = 1.0 - swing; phi = (ph - 1.0 - swing) / steplen; }
        const float fps = (float)(steplen / rate);      /* frames in this step */

        float att = p->attack * fps;
        float rel = p->release * fps;
        if (att < min_frames) att = min_frames;
        if (rel < min_frames) rel = min_frames;

        const uint32_t i32 = (uint32_t)(idx % pt->len);
        for (int ch = 0; ch < 2; ch++)
        {
            const int open = st_pattern_open(pt, ch, i32);
            const int next = st_pattern_open(pt, ch, i32 + 1u);
            const int high = open && (phi < (double)p->gate || (pt->tie && next));
            float e = st->env[ch];
            if (high) { e += 1.0f / att; if (e > 1.0f) e = 1.0f; }
            else      { e -= 1.0f / rel; if (e < 0.0f) e = 0.0f; }
            st->env[ch] = e;

            if ((int32_t)i32 != st->last_step[ch])
            {
                if (ch == 0 && open) st->step_starts++;
                st->last_step[ch] = (int32_t)i32;
            }

            const float s  = shape(e, p->curve);
            const float o  = duck ? 1.0f - depth * s : 1.0f - depth * (1.0f - s);
            const float ga = 1.0f - (1.0f - o) * (1.0f - filt);   /* VCA share   */
            const float of = 1.0f - (1.0f - o) * filt;            /* filter share */
            st->gain[ch] = o;

            const float x = in[ch][i];
            float y = x;
            if (filt > 0.001f)
            {
                const float fc = 30.0f * exp2f(of * 9.2288f);     /* 30 Hz .. 18 kHz */
                float g = tanf(3.14159265f * fc / sr);
                if (g > nyq_g) g = nyq_g;
                y = svf_lp(&st->svf[ch], y, g, k_damp);
            }
            else
                st->svf[ch].x1 = st->svf[ch].x2 = 0.0f;
            y *= ga;

            if (hole[ch]) hole[ch][i] = x - y;
            out[ch][i] = x + (y - x) * mix;
        }
    }
    /* keep pos bounded over very long runs (a whole number of patterns) */
    if (!st->locked && st->pos > 1.0e9)
    {
        const double wrap = pt->beats ? (double)pt->beats * spb * 4.0 : (double)pt->len * 2.0;
        const double k = floor(st->pos / wrap) * wrap;
        st->pos -= k;
    }
}

/* ---- commands ------------------------------------------------------------------ */

int st_apply_cmd(st_store_t *s, st_params_t *p, uint8_t cmd, const uint8_t *pl, uint32_t len)
{
    switch (cmd)
    {
        case ST_CMD_SET_WORD:
        {
            if (len < 1 || pl[0] >= ST_WORDS) return 1;
            char    *w = s->words[pl[0]];
            uint32_t n = 0;
            for (uint32_t i = 1; i < len && n < ST_WORD_MAX; i++)
            {
                char c = (char)pl[i];
                if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                if (c < 0x20 || c > 0x7E) continue;
                w[n++] = c;
            }
            w[n] = 0;
            return 0;
        }
        case ST_CMD_SET_USER:
        {
            if (len != 6 || pl[0] < 1 || pl[0] > ST_MAX_STEPS) return 1;
            uint64_t m = 0;
            for (int i = 0; i < 5; i++) m |= (uint64_t)(pl[1 + i] & 0x7Fu) << (7 * i);
            s->user_len  = pl[0];
            s->user_mask = (uint32_t)m;
            return 0;
        }
        case ST_CMD_SELECT:
        {
            if (len != 2 || pl[0] >= ST_SRC_COUNT) return 1;
            const uint8_t src = pl[0], ix = pl[1];
            if (src == ST_SRC_BANK   && ix >= ST_BANK_COUNT) return 1;
            if (src == ST_SRC_EUCLID && ix > ST_MAX_STEPS)   return 1;
            if (src == ST_SRC_MORSE  && ix >= ST_WORDS)      return 1;
            p->source = src;
            if (src == ST_SRC_BANK)   p->bank = ix;
            if (src == ST_SRC_EUCLID) p->hits = ix;
            if (src == ST_SRC_MORSE)  p->word = ix;
            return 0;
        }
        case ST_CMD_SET_FIT:
        {
            if (len != 1 || pl[0] >= ST_FIT_COUNT) return 1;
            p->fit = pl[0];
            return 0;
        }
        default:
            return 1;
    }
}
