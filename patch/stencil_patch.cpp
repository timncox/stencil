/*
 * Stencil on the Daisy Patch -- a rhythmic gate mask.
 *
 *   IN 1 / IN 2     audio in, left / right (Input = Mono feeds IN 1 to both)
 *   OUT 1 / OUT 2   the gated signal
 *   OUT 3 / OUT 4   the holes: what the gate took away (input minus gated)
 *   CTRL 1..4       PATTERN, GATE, RELEASE, DEPTH (each summed with its CV in)
 *   GATE IN 1 / 2   clock / reset
 *   GATE OUT        a trigger on each open step
 *   CV OUT 1 / 2    the left / right envelope, 0..5 V
 *   MIDI            TRS and the Seed's micro-USB: clock + Start, and SysEx
 *                   from the Stencil web page (words, user pattern, select)
 *   encoder         turn = menu, push+turn = edit; click on Word = type a
 *                   word; push+turn on Mods = module picker
 *
 * Fit (menu) says how a Morse word sits in the bar: Free, Pad to the next
 * bar line, or stretched to loop in exactly 1, 2, 4 or 8 bars.
 *
 * PATTERN means what Source says: one of 16 factory patterns or the user
 * pattern (Bank), the number of hits (Euclid), which of 8 stored words is
 * spelled (Morse). The engine is core/stencil.c, shared with the Alchemy
 * Lab build; this file is the panel.
 *
 * SysEx: F0 7D 53 54 <cmd> <payload> F7 (7D = non-commercial ID, "ST"),
 * cmd and payload as st_apply_cmd() in core/stencil.h.
 */
#include "daisy_patch.h"
#include "hid/midi.h"
#include "hid/usb_midi.h"
#include "util/PersistentStorage.h"
#include "module_picker.h"
#include <cstdio>
#include <cstring>
#include <cmath>

#include "stencil.h"

#ifndef STENCIL_VERSION
#define STENCIL_VERSION "0.0.0"
#endif
#ifndef STENCIL_GIT_HASH
#define STENCIL_GIT_HASH "dev"
#endif

using namespace daisy;

DaisyPatch      hw;
MidiUsbHandler  usb_midi;
static stencil_t ST;

/* ------------------------------------------------------------------------ */
/* Persistent settings (QSPI, 7 MB + 128 KB in)                               */

static const char *const SRC_NAME[ST_SRC_COUNT] = {"Bank", "Euclid", "Morse"};
static const char *const POL_NAME[2]            = {"Gate", "Duck"};
static const char *const CURVE_NAME[3]          = {"Snappy", "Linear", "Slow"};
static const uint8_t     PPQN[4]                = {1, 2, 4, 24};
static const char *const INPUT_NAME[2]          = {"Stereo", "Mono"};

struct Settings
{
    uint32_t   magic;
    uint8_t    source, rate, steps, polarity, curve, ppqn, input, fit;
    int8_t     r_off;
    uint8_t    attack, filter, reso, swing, mix;  /* percent */
    uint16_t   bpm;
    st_store_t store;

    bool operator==(const Settings &o) const { return memcmp(this, &o, sizeof *this) == 0; }
    bool operator!=(const Settings &o) const { return !(*this == o); }
};
static constexpr uint32_t kMagic = 0x53544E32; /* 'STN2' (2: + fit) */
/* 7 MB + 128 KB: Relay owns the sector at 7 MB, and a shared one would reset
 * each firmware's settings (and these words) on every switch between them. */
static constexpr uint32_t kSettingsOffset = 0x00720000;

static Settings defaults()
{
    Settings s;
    memset(&s, 0, sizeof s);
    s.magic    = kMagic;
    s.source   = ST_SRC_BANK;
    s.rate     = ST_RATE_1_16;
    s.steps    = 16;
    s.polarity = ST_POL_GATE;
    s.curve    = 1;
    s.ppqn     = 2; /* 4 ppqn */
    s.input    = 0;
    s.fit      = ST_FIT_PAD;
    s.r_off    = 0;
    s.attack   = 2;
    s.filter   = 0;
    s.reso     = 30;
    s.swing    = 0;
    s.mix      = 100;
    s.bpm      = 120;
    st_default_store(&s.store);
    return s;
}

static PersistentStorage<Settings> storage(hw.seed.qspi);
static Settings                   *S;
static bool                        g_dirty       = false;
static uint32_t                    g_dirty_since = 0;
static bool                        g_rebuild     = true;

static void mark_dirty(void)
{
    g_dirty       = true;
    g_dirty_since = System::GetNow();
}

/* ------------------------------------------------------------------------ */
/* Knobs + params                                                             */

static float g_k[4];

static int pattern_count(void)
{
    switch(S->source)
    {
        case ST_SRC_EUCLID: return S->steps + 1;
        case ST_SRC_MORSE: return ST_WORDS;
        default: return ST_BANK_COUNT;
    }
}
static int pattern_index(void)
{
    const int n = pattern_count();
    int       i = (int)(g_k[0] * (float)n);
    return i >= n ? n - 1 : (i < 0 ? 0 : i);
}

static void read_knobs(void)
{
    for(int i = 0; i < 4; i++)
    {
        float v = hw.GetKnobValue((DaisyPatch::Ctrl)i);
        if(fabsf(v - g_k[i]) > 0.004f) g_k[i] = v;
    }
}

static st_params_t g_params_next;
static volatile bool G_PARAMS_PENDING = false;
static st_params_t g_built_for;

static st_params_t build_params(void)
{
    st_params_t p;
    st_default_params(&p);
    p.source    = S->source;
    p.steps     = S->steps;
    const int ix = pattern_index();
    p.bank      = S->source == ST_SRC_BANK ? (uint8_t)ix : 0;
    p.hits      = S->source == ST_SRC_EUCLID ? (uint8_t)ix : 5;
    p.word      = S->source == ST_SRC_MORSE ? (uint8_t)ix : 0;
    p.r_offset  = S->r_off;
    p.rate      = S->rate;
    p.swing     = S->swing / 100.0f;
    p.bpm       = (float)S->bpm;
    p.attack    = S->attack / 100.0f;
    p.gate      = 0.05f + 0.95f * g_k[1];
    p.release   = 4.0f * g_k[2] * g_k[2];
    p.curve     = (float)S->curve - 1.0f;
    p.depth     = g_k[3];
    p.filter    = S->filter / 100.0f;
    p.resonance = S->reso / 100.0f;
    p.polarity  = S->polarity;
    p.mix       = S->mix / 100.0f;
    p.fit       = S->fit;
    return p;
}

static bool pattern_inputs_differ(const st_params_t &a, const st_params_t &b)
{
    return a.source != b.source || a.bank != b.bank || a.steps != b.steps || a.hits != b.hits
           || a.word != b.word || a.r_offset != b.r_offset || a.fit != b.fit || a.rate != b.rate;
}

static void push_params(void)
{
    st_params_t p = build_params();
    if(!G_PARAMS_PENDING)
    {
        g_params_next    = p;
        G_PARAMS_PENDING = true;
    }
    if((g_rebuild || pattern_inputs_differ(p, g_built_for)) && st_pattern_writable(&ST))
    {
        st_build_pattern(&p, &S->store, st_back_pattern(&ST));
        st_commit_pattern(&ST);
        g_built_for = p;
        g_rebuild   = false;
    }
}

/* ------------------------------------------------------------------------ */
/* MIDI: clock, Start, SysEx                                                  */

static volatile uint32_t G_MIDI_TICKS = 0;  /* main -> audio, 24 ppqn */
static volatile bool     G_MIDI_START = false;
static uint32_t          g_midi_clock_ms = 0;
static uint32_t          g_sysex_until = 0;

static void sysex(const uint8_t *d, int n)
{
    /* 7D 53 54 cmd payload... */
    if(n < 4 || d[0] != 0x7D || d[1] != 0x53 || d[2] != 0x54) return;
    st_params_t p = build_params();
    if(st_apply_cmd(&S->store, &p, d[3], d + 4, (uint32_t)(n - 4)) != 0) return;
    if(d[3] == ST_CMD_SELECT)
    {
        /* the knob still rules PATTERN; a SELECT picks the source, and for a
         * word or bank says which -- shown until the knob moves */
        S->source = p.source;
    }
    S->fit = p.fit;
    g_rebuild     = true;
    g_sysex_until = System::GetNow() + 400;
    mark_dirty();
}

static void feed(MidiEvent &e)
{
    if(e.type == SystemRealTime)
    {
        if(e.srt_type == TimingClock)
        {
            G_MIDI_TICKS    = G_MIDI_TICKS + 1;
            g_midi_clock_ms = System::GetNow();
        }
        else if(e.srt_type == Start)
            G_MIDI_START = true;
    }
    else if(e.type == SystemCommon && e.sc_type == SystemExclusive)
    {
        SystemExclusiveEvent x = e.AsSystemExclusive();
        sysex(x.data, x.length);
    }
}

static void pump_midi(void)
{
    hw.midi.Listen();
    while(hw.midi.HasEvents())
    {
        MidiEvent e = hw.midi.PopEvent();
        feed(e);
    }
    while(usb_midi.HasEvents())
    {
        MidiEvent e = usb_midi.PopEvent();
        feed(e);
    }
}

/* ------------------------------------------------------------------------ */
/* Word editor: click on Word. Turn = change the letter, click = next letter, */
/* push+turn = move the cursor, hold 1 s = done (saved).                      */

static const char kCharset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?!/-=+@";
static bool  g_editing = false;
static int   g_edit_slot = 0, g_cursor = 0;
static char  g_edit_buf[ST_WORD_MAX + 1];

static void edit_begin(int slot)
{
    g_editing   = true;
    g_edit_slot = slot;
    strncpy(g_edit_buf, S->store.words[slot], ST_WORD_MAX);
    g_edit_buf[ST_WORD_MAX] = 0;
    g_cursor = (int)strlen(g_edit_buf);
    if(g_cursor >= (int)ST_WORD_MAX) g_cursor = (int)ST_WORD_MAX - 1;
}
static void edit_end(void)
{
    int n = (int)strlen(g_edit_buf);
    while(n > 0 && g_edit_buf[n - 1] == ' ') g_edit_buf[--n] = 0;
    strcpy(S->store.words[g_edit_slot], g_edit_buf);
    g_editing = false;
    g_rebuild = true;
    mark_dirty();
}
static void edit_turn(int inc)
{
    int len = (int)strlen(g_edit_buf);
    if(g_cursor >= len)
    {
        /* past the end: a turn adds a letter there */
        g_edit_buf[g_cursor]     = ' ';
        g_edit_buf[g_cursor + 1] = 0;
    }
    const char *at = strchr(kCharset, g_edit_buf[g_cursor]);
    int         ci = at ? (int)(at - kCharset) : 0;
    const int   nc = (int)sizeof kCharset - 1;
    ci             = ((ci + inc) % nc + nc) % nc;
    g_edit_buf[g_cursor] = kCharset[ci];
}

/* ------------------------------------------------------------------------ */
/* Menu                                                                       */

enum MenuItem
{
    MI_SOURCE,
    MI_WORD,
    MI_FIT,
    MI_RATE,
    MI_STEPS,
    MI_ATTACK,
    MI_CURVE,
    MI_FILTER,
    MI_RESO,
    MI_MODE,
    MI_SWING,
    MI_ROFF,
    MI_MIX,
    MI_TEMPO,
    MI_CLKIN,
    MI_INPUT,
    MI_MODS,
    MI_COUNT
};
static const char *const MENU_LABEL[MI_COUNT]
    = {"Source", "Word", "Fit", "Rate", "Steps", "Attack", "Curve", "Filter", "Reso",
       "Mode",   "Swing", "R Off", "Mix", "Tempo", "ClkIn", "Input", "Mods"};
static constexpr int MENU_ROWS = 3;
static int           g_sel = MI_SOURCE, g_top = 0;
static bool          enc_down, enc_turned;
static uint32_t      enc_down_ms;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void menu_edit(int inc)
{
    switch(g_sel)
    {
        case MI_SOURCE: S->source = (uint8_t)clampi(S->source + inc, 0, ST_SRC_COUNT - 1); break;
        case MI_WORD: return; /* click to edit */
        case MI_FIT: S->fit = (uint8_t)clampi(S->fit + inc, 0, ST_FIT_COUNT - 1); break;
        case MI_RATE: S->rate = (uint8_t)clampi(S->rate + inc, 0, ST_RATE_COUNT - 1); break;
        case MI_STEPS: S->steps = (uint8_t)clampi(S->steps + inc, 1, ST_MAX_STEPS); break;
        case MI_ATTACK: S->attack = (uint8_t)clampi(S->attack + inc, 0, 100); break;
        case MI_CURVE: S->curve = (uint8_t)clampi(S->curve + inc, 0, 2); break;
        case MI_FILTER: S->filter = (uint8_t)clampi(S->filter + 5 * inc, 0, 100); break;
        case MI_RESO: S->reso = (uint8_t)clampi(S->reso + 5 * inc, 0, 90); break;
        case MI_MODE: S->polarity = (uint8_t)clampi(S->polarity + inc, 0, 1); break;
        case MI_SWING: S->swing = (uint8_t)clampi(S->swing + inc, 0, 50); break;
        case MI_ROFF: S->r_off = (int8_t)clampi(S->r_off + inc, -16, 16); break;
        case MI_MIX: S->mix = (uint8_t)clampi(S->mix + 5 * inc, 0, 100); break;
        case MI_TEMPO: S->bpm = (uint16_t)clampi(S->bpm + inc, 40, 240); break;
        case MI_CLKIN: S->ppqn = (uint8_t)clampi(S->ppqn + inc, 0, 3); break;
        case MI_INPUT: S->input = (uint8_t)clampi(S->input + inc, 0, 1); break;
        case MI_MODS: picker::run(hw); return;
    }
    mark_dirty();
}

static void encoder(void)
{
    const uint32_t now = System::GetNow();
    if(hw.encoder.RisingEdge())
    {
        enc_down    = true;
        enc_turned  = false;
        enc_down_ms = now;
    }
    int inc = hw.encoder.Increment();

    if(g_editing)
    {
        if(inc)
        {
            if(enc_down && hw.encoder.Pressed())
            {
                g_cursor = clampi(g_cursor + inc, 0, ST_WORD_MAX - 1);
                if(g_cursor > (int)strlen(g_edit_buf)) g_cursor = (int)strlen(g_edit_buf);
                enc_turned = true;
            }
            else
                edit_turn(inc);
        }
        if(enc_down && !enc_turned && hw.encoder.Pressed() && now - enc_down_ms > 1000)
        {
            edit_end();
            enc_turned = true; /* swallow the release */
        }
        if(enc_down && !hw.encoder.Pressed())
        {
            enc_down = false;
            if(!enc_turned && g_editing)
                g_cursor = clampi(g_cursor + 1, 0, ST_WORD_MAX - 1);
        }
        return;
    }

    if(inc)
    {
        if(enc_down && hw.encoder.Pressed())
        {
            menu_edit(inc);
            enc_turned = true;
        }
        else
        {
            g_sel = clampi(g_sel + inc, 0, MI_COUNT - 1);
            if(g_sel < g_top) g_top = g_sel;
            if(g_sel >= g_top + MENU_ROWS) g_top = g_sel - MENU_ROWS + 1;
        }
    }
    if(enc_down && !hw.encoder.Pressed())
    {
        enc_down = false;
        if(!enc_turned && g_sel == MI_WORD)
            edit_begin(S->source == ST_SRC_MORSE ? pattern_index() : 0);
    }
}

/* ------------------------------------------------------------------------ */
/* Display                                                                    */

static volatile uint32_t G_STEP = 0;
static volatile float    G_ENV[2];

static void draw_pattern(int y)
{
    /* 32 cells of 4 px, a page of 32 steps at a time */
    const st_pattern_t *pt   = st_front_pattern(&ST);
    const uint32_t      step = G_STEP;
    const uint32_t      base = (step / 32u) * 32u;
    for(uint32_t k = 0; k < 32; k++)
    {
        const uint32_t i  = base + k;
        const int      x0 = (int)k * 4;
        if(i >= pt->len) break;
        const bool open = st_pattern_open(pt, 0, i);
        if(i == step) hw.display.DrawRect(x0, y, x0 + 2, y + 7, true, true);
        else if(open) hw.display.DrawRect(x0, y + 2, x0 + 2, y + 5, true, true);
        else hw.display.DrawPixel(x0 + 1, y + 4, true);
    }
}

static void draw_editor(void)
{
    char line[32];
    hw.display.Fill(false);
    snprintf(line, sizeof line, "WORD %d", g_edit_slot + 1);
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);
    hw.display.SetCursor(0, 56);
    hw.display.WriteString("turn chr  hold:done", Font_6x8, true);
    /* 18 characters of 7 px per row; scroll so the cursor is in view */
    const int cols = 18;
    const int len  = (int)strlen(g_edit_buf);
    int       first = g_cursor - cols + 2;
    if(first < 0) first = 0;
    for(int c = 0; c < cols; c++)
    {
        const int i = first + c;
        char      ch[2] = {i < len ? g_edit_buf[i] : ' ', 0};
        if(ch[0] == ' ' && i < len) ch[0] = '_';
        hw.display.SetCursor(c * 7, 20);
        hw.display.WriteString(ch, Font_7x10, true);
        if(i == g_cursor) hw.display.DrawLine(c * 7, 31, c * 7 + 5, 31, true);
    }
    /* the letter under the cursor in Morse */
    const char *m = g_cursor < len ? st_morse_char(g_edit_buf[g_cursor]) : nullptr;
    hw.display.SetCursor(0, 40);
    hw.display.WriteString(m ? m : "", Font_7x10, true);
    hw.display.Update();
}

static void draw(void)
{
    if(g_editing)
    {
        draw_editor();
        return;
    }
    char     line[32], num[24];
    uint32_t now = System::GetNow();
    hw.display.Fill(false);

    /* Row 0: what plays, and the tempo */
    const int ix = pattern_index();
    if(S->source == ST_SRC_EUCLID) snprintf(line, sizeof line, "EUCLID %d/%d", ix, S->steps);
    else if(S->source == ST_SRC_MORSE)
    {
        static const char *const FIT_SHORT[ST_FIT_COUNT] = {"free", "pad", "1bar", "2bar", "4bar", "8bar"};
        snprintf(line, sizeof line, "MORSE %d %s", ix + 1, FIT_SHORT[S->fit % ST_FIT_COUNT]);
    }
    else if(ix == ST_USER_PATTERN) snprintf(line, sizeof line, "BANK user");
    else snprintf(line, sizeof line, "BANK %d", ix + 1);
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);
    snprintf(line, sizeof line, "%s%3d", st_locked(&ST) ? "clk" : "bpm", (int)lroundf(st_bpm(&ST)));
    hw.display.SetCursor(128 - 6 * 6, 0);
    hw.display.WriteString(line, Font_6x8, true);
    if(now < g_sysex_until)
    {
        hw.display.SetCursor(128 - 6 * 10, 0);
        hw.display.WriteString("USB", Font_6x8, true);
    }

    /* Row 1: the pattern */
    draw_pattern(10);

    /* Row 2: the word (Morse) or the shape; envelope bars right */
    if(S->source == ST_SRC_MORSE)
        snprintf(line, sizeof line, "%.17s", S->store.words[ix]);
    else
        snprintf(line, sizeof line, "%s %s", POL_NAME[S->polarity], st_rate_name[S->rate]);
    hw.display.SetCursor(0, 21);
    hw.display.WriteString(line, Font_6x8, true);
    for(int c = 0; c < 2; c++)
    {
        const int w = (int)(G_ENV[c] * 12.0f);
        hw.display.DrawRect(110, 20 + c * 5, 110 + 13, 23 + c * 5, true, false);
        if(w > 0) hw.display.DrawRect(111, 21 + c * 5, 111 + w - 1, 22 + c * 5, true, true);
    }

    /* Rows 4-6: the menu */
    for(int i = 0; i < MENU_ROWS; i++)
    {
        int row = g_top + i;
        if(row >= MI_COUNT) break;
        const char *value = num;
        switch(row)
        {
            case MI_SOURCE: value = SRC_NAME[S->source]; break;
            case MI_WORD:
                snprintf(num, sizeof num, "%.12s", S->store.words[S->source == ST_SRC_MORSE ? ix : 0]);
                break;
            case MI_FIT: value = st_fit_name[S->fit]; break;
            case MI_RATE: value = st_rate_name[S->rate]; break;
            case MI_STEPS: snprintf(num, sizeof num, "%d", S->steps); break;
            case MI_ATTACK: snprintf(num, sizeof num, "%d%%", S->attack); break;
            case MI_CURVE: value = CURVE_NAME[S->curve]; break;
            case MI_FILTER:
                if(S->filter == 0) snprintf(num, sizeof num, "VCA");
                else if(S->filter == 100) snprintf(num, sizeof num, "LPF");
                else snprintf(num, sizeof num, "%d%% LPF", S->filter);
                break;
            case MI_RESO: snprintf(num, sizeof num, "%d%%", S->reso); break;
            case MI_MODE: value = POL_NAME[S->polarity]; break;
            case MI_SWING: snprintf(num, sizeof num, "%d%%", S->swing); break;
            case MI_ROFF: snprintf(num, sizeof num, "%+d", S->r_off); break;
            case MI_MIX: snprintf(num, sizeof num, "%d%%", S->mix); break;
            case MI_TEMPO: snprintf(num, sizeof num, "%d", S->bpm); break;
            case MI_CLKIN: snprintf(num, sizeof num, "%d ppqn", PPQN[S->ppqn]); break;
            case MI_INPUT: value = INPUT_NAME[S->input]; break;
            case MI_MODS: snprintf(num, sizeof num, "%s %.7s", STENCIL_VERSION, STENCIL_GIT_HASH); break;
        }
        snprintf(line, sizeof line, "%c%-6s %s", row == g_sel ? '>' : ' ', MENU_LABEL[row], value);
        hw.display.SetCursor(0, 32 + i * 9);
        hw.display.WriteString(line, Font_6x8, true);
    }
    hw.display.Update();
}

/* ------------------------------------------------------------------------ */
/* Audio                                                                      */

static uint32_t g_tr_left = 0, g_last_starts = 0, g_ticks_seen = 0;
static constexpr uint32_t kPulseFrames = 480; /* 10 ms */
static volatile uint8_t G_PPQN = 4;
static volatile bool    G_MIDI_CLOCKED = false;
static volatile bool    G_MONO = false;

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    if(G_PARAMS_PENDING)
    {
        st_set_params(&ST, &g_params_next);
        G_PARAMS_PENDING = false;
    }
    /* reset first, so a clock edge in the same block is beat 1 */
    if(hw.gate_input[1].Trig()) st_reset(&ST);
    if(G_MIDI_START)
    {
        G_MIDI_START = false;
        st_reset(&ST);
    }
    const bool gate_clock = hw.gate_input[0].Trig();
    if(G_MIDI_CLOCKED)
    {
        const uint32_t t = G_MIDI_TICKS;
        while(g_ticks_seen != t)
        {
            g_ticks_seen++;
            st_clock_edge(&ST, 24);
        }
    }
    else
    {
        g_ticks_seen = G_MIDI_TICKS;
        if(gate_clock) st_clock_edge(&ST, G_PPQN);
    }

    const float *inr = G_MONO ? in[0] : in[1];
    st_process(&ST, in[0], inr, out[0], out[1], out[2], out[3], (uint32_t)size);

    G_ENV[0] = st_env(&ST, 0);
    G_ENV[1] = st_env(&ST, 1);
    G_STEP   = st_step(&ST);
    if(ST.step_starts != g_last_starts)
    {
        g_last_starts = ST.step_starts;
        g_tr_left     = kPulseFrames;
    }
    g_tr_left = g_tr_left > size ? g_tr_left - (uint32_t)size : 0u;
}

static void write_outputs(void)
{
    hw.seed.dac.WriteValue(DacHandle::Channel::ONE, (uint16_t)(G_ENV[0] * 4095.0f));
    hw.seed.dac.WriteValue(DacHandle::Channel::TWO, (uint16_t)(G_ENV[1] * 4095.0f));
    hw.gate_output.Write(g_tr_left > 0);
}

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(48);
    storage.Init(defaults(), kSettingsOffset);
    S = &storage.GetSettings();
    if(S->magic != kMagic) *S = defaults();

    st_init(&ST, hw.AudioSampleRate());
    for(int i = 0; i < 4; i++) g_k[i] = 0.0f;
    hw.StartAdc();
    System::Delay(5);
    read_knobs();
    push_params();
    st_set_params(&ST, &g_params_next);
    G_PARAMS_PENDING = false;

    hw.midi.StartReceive();
    MidiUsbHandler::Config ucfg;
    ucfg.transport_config.periph = MidiUsbTransport::Config::INTERNAL;
    usb_midi.Init(ucfg);
    usb_midi.StartReceive();

    hw.StartAudio(AudioCallback);

    uint32_t last_draw = 0;
    for(;;)
    {
        const uint32_t now = System::GetNow();
        hw.ProcessAllControls();
        encoder();
        read_knobs();
        pump_midi();
        G_MIDI_CLOCKED = g_midi_clock_ms && now - g_midi_clock_ms < 500;
        G_PPQN         = PPQN[S->ppqn & 3];
        G_MONO         = S->input == 1;
        push_params();
        write_outputs();

        if(now - last_draw >= 33)
        {
            last_draw = now;
            draw();
        }
        /* save two seconds after the last edit, never mid-edit */
        if(g_dirty && !g_editing && now - g_dirty_since > 2000)
        {
            storage.Save();
            g_dirty = false;
        }
    }
}
