/*
 * Stencil -- a rhythmic gate mask for the Hermetic Modular Alchemy Lab V2,
 * on the Alchemy SDK. The engine is core/stencil.c (shared with the Daisy
 * Patch build); this file is the panel.
 *
 *   PLAY            [P1 RATE]    B1   [P2 PATTERN]
 *                   [P3 GATE]    B2   [P4 ATTACK]
 *                   [P5 RELEASE] B3   [P6 DEPTH]
 *   SHIFT (hold B3) FILTER, STEPS, R OFFSET, SWING, MIX, TEMPO
 *
 *   B1   tap: source BANK -> EUCLID -> MORSE     hold: GATE <-> DUCK
 *   B2   tap: tap tempo                          hold: bypass (gate open)
 *   J1/J2 audio in L/R   J3 clock in   J4 reset in   J5 CV pattern
 *   J6 CV depth   J7 envelope out (0..5 V)   J8 step trigger out   J9/J10 out
 *
 * PATTERN means what the source says: one of 16 factory patterns or the user
 * pattern (BANK), the number of hits (EUCLID), or which of eight stored words
 * is spelled (MORSE). Its ring is the pattern itself: lit steps open, the
 * playhead white, a page of the pattern at a time when it is longer than
 * the ring.
 *
 * Words and the user pattern come from the web page (docs/index.html) over
 * HostLink: commands 0x60 (apply a Stencil command, core/stencil.h) and
 * 0x61 (read the words back). The web programmer and the card's file panel
 * keep working alongside -- HostLink is the default and only USB mode here.
 *
 * What the comments of break-alchemy (the template) explain -- pot catch,
 * the autosave rules, the J3 raw-ADC gate read -- is not repeated.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "daisy_seed.h"
#include "util/CpuLoadMeter.h"

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/host_link/extension.h"
#include "alchemy/host_link/host.h"
#include "alchemy/host_link/wire.h"
#include "alchemy/storage/fs_extension.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/cv_matrix.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/manual.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_button.h"
#include "alchemy/surface/virtual_knob.h"

#include "extras.h"
#include "picker.h"

#ifndef STENCIL_VERSION
#define STENCIL_VERSION "0.0.0"
#endif
#ifndef STENCIL_GIT_HASH
#define STENCIL_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

static constexpr uint32_t kBlockSize = 48u;
enum : uint8_t { kPagePlay = 0, kPageShift = 1, kNumAppPages = 2 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };

/* J3 / J4: clock and reset, read raw (CV indices 0, 1). J7 / J8: STM DAC. */
static constexpr uint8_t  kClockJack   = 0u;
static constexpr uint8_t  kResetJack   = 1u;
static constexpr uint8_t  kJackEnvOut  = 4u;
static constexpr uint8_t  kJackTrigOut = 5u;
static constexpr uint32_t kPulseFrames = 480u;   /* 10 ms */

/* Home preset slot. Taken: Break 4, seq 5, Meld 6, MI 7-11, Smack 12,
 * Mark 13, Belt 14, Relay 15; 0 is the migration slot. */
static constexpr uint8_t kHomeSlot = 3u;

/* ---- hardware + engine ---------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;
static stencil_t           ST;
static StencilExtras       extras;

/* control -> audio */
static st_params_t   g_params_next;
static volatile bool G_PARAMS_PENDING = false;
static volatile bool G_BYPASS = false;
static volatile bool G_MONO   = false;
static volatile uint8_t G_PPQN = 4;

/* audio -> control */
static volatile uint32_t G_STEP = 0;
static volatile uint32_t G_BEATS = 0;
static volatile float    G_ENV = 0.0f;

/* ---- colours -------------------------------------------------------------- */

static constexpr LedPanel::Rgb kColRate   = {0x40, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColPat    = {0xFF, 0xA0, 0x20};
static constexpr LedPanel::Rgb kColGate   = {0x40, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColAtt    = {0xFF, 0x60, 0xC0};
static constexpr LedPanel::Rgb kColRel    = {0xC0, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColDepth  = {0xFF, 0x40, 0x20};
static constexpr LedPanel::Rgb kColFiltA  = {0xFF, 0x40, 0x20};
static constexpr LedPanel::Rgb kColFiltF  = {0x20, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColNotch  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColSteps  = {0x40, 0xFF, 0xC0};
static constexpr LedPanel::Rgb kColOffL   = {0x80, 0x40, 0xFF};
static constexpr LedPanel::Rgb kColOffR   = {0xFF, 0x40, 0xC0};
static constexpr LedPanel::Rgb kColSwing  = {0xFF, 0xC0, 0x40};
static constexpr LedPanel::Rgb kColMix    = {0x40, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColTempo  = {0xFF, 0xC0, 0x40};
static constexpr LedPanel::Rgb kColDim    = {0x18, 0x18, 0x18};

static constexpr LedPanel::Rgb kSrcCol[ST_SRC_COUNT] = {
    {0xFF, 0xA0, 0x20},   /* BANK   amber */
    {0x20, 0xFF, 0x40},   /* EUCLID green */
    {0x20, 0xD0, 0xFF},   /* MORSE  cyan  */
};
static constexpr LedPanel::Rgb kGateCol = {0x30, 0x10, 0x40};
static constexpr LedPanel::Rgb kDuckCol = {0xFF, 0x50, 0x00};
static constexpr LedPanel::Rgb kWhite   = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kGrey    = {0x60, 0x60, 0x60};
static constexpr LedPanel::Rgb kBlue    = {0x00, 0x50, 0xFF};
static constexpr LedPanel::Rgb kRed     = {0xFF, 0x00, 0x00};
static constexpr LedPanel::Rgb kOff     = {0x00, 0x00, 0x00};

/* ---- pages ---------------------------------------------------------------- */

static void PatternOverdraw(LedPanel& panel, uint8_t pot, const ArcGeometry& geo,
                            float norm, uint32_t t_ms, void* ctx);

static VirtualKnob kn_rate = VirtualKnob(kPotTopLeft, "Rate")
    .Selector(ST_RATE_COUNT).Labels(st_rate_name).Ident("rate")
    .Ring(SelectorRing(kColRate, kColDim, ST_RATE_COUNT));
static VirtualKnob kn_pat = VirtualKnob(kPotTopRight, "Pattern")
    .Linear(0.f, 1.f).Ident("pattern").Ring(Level(kColPat));
static VirtualKnob kn_gate = VirtualKnob(kPotMiddleLeft, "Gate")
    .Linear(5.f, 100.f).Ident("gate").Unit("%").Ring(Level(kColGate));
static VirtualKnob kn_att = VirtualKnob(kPotMiddleRight, "Attack")
    .Linear(0.f, 100.f).Ident("attack").Unit("%").Ring(Level(kColAtt));
static VirtualKnob kn_rel = VirtualKnob(kPotBottomLeft, "Release")
    .Linear(0.f, 400.f).Ident("release").Unit("%").Ring(Level(kColRel));
static VirtualKnob kn_depth = VirtualKnob(kPotBottomRight, "Depth")
    .Linear(0.f, 100.f).Ident("depth").Unit("%").Ring(Level(kColDepth));

static VirtualKnob kn_filter = VirtualKnob(kPotTopLeft, "Filter")
    .Linear(0.f, 100.f).Ident("filter").Unit("%")
    .Ring(Bipolar(kColFiltF, kColFiltA, kColNotch));
static VirtualKnob kn_steps = VirtualKnob(kPotTopRight, "Steps")
    .Linear(1.f, 32.f).Ident("steps").Ring(Level(kColSteps));
static VirtualKnob kn_off = VirtualKnob(kPotMiddleLeft, "R Offset")
    .Linear(-8.f, 8.f).Ident("r_offset").Ring(Bipolar(kColOffR, kColOffL, kColNotch));
static VirtualKnob kn_swing = VirtualKnob(kPotMiddleRight, "Swing")
    .Linear(0.f, 50.f).Ident("swing").Unit("%").Ring(Level(kColSwing));
static VirtualKnob kn_mix = VirtualKnob(kPotBottomLeft, "Mix")
    .Linear(0.f, 100.f).Ident("mix").Unit("%").Ring(Level(kColMix));
static VirtualKnob kn_tempo = VirtualKnob(kPotBottomRight, "Tempo")
    .Linear(40.f, 240.f).Ident("tempo").Unit("BPM").Ring(Level(kColTempo));

static Page play_page = Page(kPagePlay).Name("Play").Color("#ffa020")
    .Knobs(kn_rate, kn_pat, kn_gate, kn_att, kn_rel, kn_depth);
static Page shift_page = Page(kPageShift).Name("Shift").Color("#c060ff")
    .Knobs(kn_filter, kn_steps, kn_off, kn_swing, kn_mix, kn_tempo);

/* ---- descriptor metadata -------------------------------------------------- */

static Jack jk_in_l  ("J1",  "In L",        JackSig::AudioIn);
static Jack jk_in_r  ("J2",  "In R",        JackSig::AudioIn);
static Jack jk_clk   ("J3",  "Clock",       JackSig::Trig);
static Jack jk_rst   ("J4",  "Reset",       JackSig::Trig);
static Jack jk_cvpat ("J5",  "CV Pattern",  JackSig::CvBi);
static Jack jk_cvdep ("J6",  "CV Depth",    JackSig::CvBi);
static Jack jk_env   ("J7",  "Env Out",     JackSig::CvUni);
static Jack jk_trig  ("J8",  "Step Trig",   JackSig::Trig);
static Jack jk_out_l ("J9",  "Out L",       JackSig::AudioOut);
static Jack jk_out_r ("J10", "Out R",       JackSig::AudioOut);

static VirtualButton bt_b1 = VirtualButton("b1", "Source")
    .Action("Tap", "Bank -> Euclid -> Morse")
    .Action("Hold", "Gate <-> Duck");
static VirtualButton bt_b2 = VirtualButton("b2", "Tap")
    .Action("Tap", "Tap tempo")
    .Action("Hold", "Bypass: gate held open");
static VirtualButton bt_b3 = VirtualButton("b3", "Shift")
    .Action("Hold", "Show the Shift page");

static Manual manual = Manual()
    .Tagline("Rhythmic gate mask: trance gate, sidechain pump, Morse words")
    .Preamble(
        "**Stencil** cuts sustained sound into rhythm. A step pattern opens and "
        "closes a gate on each channel; GATE, ATTACK and RELEASE shape every "
        "opening. FILTER turns the VCA gate into a 12 dB/oct lowpass that "
        "carves the highs away instead. B1 picks the pattern source -- a "
        "factory **bank** pattern, a **Euclidean** spread, or a **Morse** "
        "word -- and, held, swaps GATE for DUCK (the level dips on each "
        "open step: the pumping-sidechain sound). Words and a user pattern "
        "come from the Stencil web page. Clock J3, reset J4, envelope out "
        "J7, a trigger on each open step J8.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
static Pager       pager = Pager(kNumAppPages, kNumPots)
                               .Shift(hw.buttons[kButtonB3], kPageShift);
static Presets     presets(hw.seed.qspi);
static Settings    settings(hw, &pager);
static CvMatrix    cv_matrix(kNumCvInputs);
static SdCard      sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "stencil_alchemy", "Stencil", STENCIL_VERSION, STENCIL_GIT_HASH);

static const char* const kCurveLabels[3] = {"Snappy", "Linear", "Slow"};
static const char* const kPpqnLabels[4]  = {"1 ppqn", "2 ppqn", "4 ppqn", "24 ppqn"};
static const uint8_t     kPpqn[4]        = {1, 2, 4, 24};
static const char* const kInputLabels[2] = {"Stereo", "Mono L"};
static SelectorHandle curve_sel;
static SelectorHandle ppqn_sel;
static SelectorHandle input_sel;

static bool     g_dirty = false;
static uint32_t g_dirty_since = 0, g_dirty_first = 0;
static bool     g_store_changed = true;    /* rebuild the pattern */

static void mark_dirty(uint32_t now)
{
    if (!g_dirty) g_dirty_first = now;
    g_dirty = true;
    g_dirty_since = now;
}

/* ---- PATTERN: what the pot means per source ------------------------------ */

static uint8_t steps_now(void)
{
    int s = (int)lroundf(kn_steps.Value());
    return (uint8_t)(s < 1 ? 1 : s > 32 ? 32 : s);
}

static int pattern_count(uint8_t source)
{
    switch (source)
    {
        case ST_SRC_EUCLID: return steps_now() + 1;   /* 0..steps hits */
        case ST_SRC_MORSE:  return ST_WORDS;
        default:            return ST_BANK_COUNT;
    }
}

static int pattern_index(float norm, uint8_t source)
{
    const int n = pattern_count(source);
    int i = (int)(norm * (float)n);
    return i >= n ? n - 1 : (i < 0 ? 0 : i);
}

static float pattern_norm(int index, uint8_t source)
{
    return ((float)index + 0.5f) / (float)pattern_count(source);
}

/* ---- HostLink: the web page's words and patterns --------------------------- */

enum : uint8_t { kCmdStencil = 0x60, kCmdGetWords = 0x61 };

class StencilExtension : public hostlink::IHostlinkExtension
{
  public:
    uint8_t FirstCmd() const override { return kCmdStencil; }
    uint8_t LastCmd() const override { return kCmdGetWords; }

    void Handle(const hostlink::ParsedFrame& f, hostlink::FrameWriter& w,
                uint32_t now_ms) override
    {
        if (f.type == kCmdGetWords)
        {
            w.U8((uint8_t)hostlink::Status::Ok);
            w.U8(ST_WORDS);
            for (unsigned i = 0; i < ST_WORDS; i++)
            {
                const uint8_t n = (uint8_t)strnlen(extras.store.words[i], ST_WORD_MAX);
                w.U8(n);
                w.Bytes((const uint8_t*)extras.store.words[i], n);
            }
            w.U8(extras.source);
            w.U8((uint8_t)pattern_index(kn_pat.Norm(), extras.source));
            w.U8(extras.store.user_len);
            w.U32(extras.store.user_mask);
            return;
        }
        if (f.len < 1)
        {
            w.U8((uint8_t)hostlink::Status::BadArgs);
            return;
        }
        st_params_t p;
        st_default_params(&p);
        p.source = extras.source;
        const uint8_t cmd = f.body[0];
        if (st_apply_cmd(&extras.store, &p, cmd, f.body + 1, (uint32_t)f.len - 1u) != 0)
        {
            w.U8((uint8_t)hostlink::Status::BadArgs);
            return;
        }
        if (cmd == ST_CMD_SELECT)
        {
            extras.source = p.source;
            const int ix = p.source == ST_SRC_BANK ? p.bank
                         : p.source == ST_SRC_EUCLID ? p.hits : p.word;
            /* the pot catches from here, so the selection holds until it moves */
            pager.SetStored(kPagePlay, kPotTopRight, pattern_norm(ix, p.source), loop.Phys());
        }
        g_store_changed = true;
        mark_dirty(now_ms);
        w.U8((uint8_t)hostlink::Status::Ok);
    }

    const char* DescriptorRootJson() const override { return "\"stencil\":{\"v\":1}"; }
};
static StencilExtension st_ext;

/* ---- clock + reset on J3 / J4, raw ADC (as Belt's J3) ---------------------- */

struct RawGate
{
    uint8_t  jack;
    uint16_t zero = 32768u;
    int      hi = 4915, lo = 1638;
    bool     state = false;

    void Calibrate()
    {
        const V2Calibration& cal = hw.Calibration();
        float vdda = cal.vdda_at_cal;
        if (!(vdda > 2.5f && vdda < 3.6f)) vdda = kV2VddaDesign;
        uint16_t z = cal.jack[jack].adc_zero_code;
        if (z < 20000u || z > 45000u) z = 32768u;
        const float cpv = kV2CvInGainDesign * 65535.0f / vdda;
        zero = z;
        hi = (int)(1.5f * cpv);
        lo = (int)(0.5f * cpv);
    }
    /* true on a rising edge */
    bool Poll()
    {
        const uint16_t raw = hw.seed.adc.Get(kCvAdcOffset + jack);
        int dev = (int)zero - (int)raw;
        if (dev < 0) dev = -dev;
        if (!state) { if (dev > hi) { state = true; return true; } }
        else if (dev < lo) state = false;
        return false;
    }
};
static RawGate g_clock{kClockJack}, g_reset{kResetJack};

/* ---- buttons (1 ms poll) ---------------------------------------------------- */

#define LONG_PRESS_MS 600u
#define BYPASS_HOLD_MS 250u

static uint8_t g_btn_swallow = 0;
static bool btn_live(uint8_t b)
{
    const bool    down = hw.buttons[b].Pressed();
    const uint8_t bit  = (uint8_t)(1u << b);
    if (g_btn_swallow & bit)
    {
        if (down) return false;
        g_btn_swallow = (uint8_t)(g_btn_swallow & ~bit);
    }
    return down;
}

static bool     b1_down = false, b1_fired = false;
static uint32_t b1_t0 = 0;
static bool     b2_down = false;
static uint32_t b2_t0 = 0;
static uint32_t g_taps[4];
static int      g_ntaps = 0;
static uint32_t g_tap_flash_until = 0;
static uint32_t g_src_flash_until = 0;

static float bpm_to_norm(float bpm) { return (bpm - 40.0f) / 200.0f; }

static void tap_tempo(uint32_t now)
{
    if (g_ntaps && now - g_taps[(g_ntaps - 1) & 3] > 2000u) g_ntaps = 0;
    g_taps[g_ntaps & 3] = now;
    g_ntaps++;
    g_tap_flash_until = now + 60u;
    if (g_ntaps < 2 || st_locked(&ST)) return;
    const int n = g_ntaps < 4 ? g_ntaps : 4;
    const uint32_t span = g_taps[(g_ntaps - 1) & 3] - g_taps[(g_ntaps - n) & 3];
    const float bpm = 60000.0f * (float)(n - 1) / (float)span;
    if (bpm < 40.0f || bpm > 240.0f) return;
    extras.free_bpm = bpm;
    pager.SetStored(kPageShift, kPotBottomRight, bpm_to_norm(bpm), loop.Phys());
    mark_dirty(now);
}

static void OnPoll(uint32_t now)
{
    if (settings.IsActive())
    {
        G_BYPASS = false;
        b1_down = b2_down = false;
        g_btn_swallow = (uint8_t)((1u << kButtonB1) | (1u << kButtonB2) | (1u << kButtonB3));
        return;
    }

    const bool p1 = btn_live(kButtonB1);
    if (p1 && !b1_down) { b1_down = true; b1_fired = false; b1_t0 = now; }
    if (b1_down && p1 && !b1_fired && now - b1_t0 > LONG_PRESS_MS)
    {
        extras.polarity = extras.polarity == ST_POL_GATE ? ST_POL_DUCK : ST_POL_GATE;
        b1_fired = true;
        mark_dirty(now);
    }
    if (b1_down && !p1)
    {
        b1_down = false;
        if (!b1_fired)
        {
            /* keep PATTERN's place in the new source's range */
            extras.source = (uint8_t)((extras.source + 1u) % ST_SRC_COUNT);
            g_store_changed = true;
            g_src_flash_until = now + 150u;
            mark_dirty(now);
        }
    }

    const bool p2 = btn_live(kButtonB2);
    if (p2 && !b2_down) { b2_down = true; b2_t0 = now; }
    if (b2_down && p2 && now - b2_t0 > BYPASS_HOLD_MS) G_BYPASS = true;
    if (b2_down && !p2)
    {
        b2_down = false;
        if (!G_BYPASS) tap_tempo(now);
        G_BYPASS = false;
    }
}

/* ---- control frame (~60 Hz) ------------------------------------------------- */

static st_params_t g_built_for;   /* what the current pattern was built from */
static bool        prev_settings = false;
static float       g_saved_peak = 0.0f;

static bool pattern_inputs_differ(const st_params_t& a, const st_params_t& b)
{
    return a.source != b.source || a.bank != b.bank || a.steps != b.steps
        || a.hits != b.hits || a.word != b.word || a.r_offset != b.r_offset;
}

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();

    /* PLAY-page changes are worth saving */
    {
        static float seen[kNumPots];
        static bool  primed = false;
        for (uint8_t p = 0; p < kNumPots; p++)
        {
            const float s = pager.Stored(kPagePlay, p) + pager.Stored(kPageShift, p);
            if (primed && fabsf(s - seen[p]) > 0.01f) mark_dirty(now);
            seen[p] = s;
        }
        primed = true;
    }

    st_params_t p;
    st_default_params(&p);
    p.source   = extras.source;
    p.steps    = steps_now();
    const int ix = pattern_index(kn_pat.Norm(), p.source);
    p.bank     = p.source == ST_SRC_BANK ? (uint8_t)ix : 0;
    p.hits     = p.source == ST_SRC_EUCLID ? (uint8_t)ix : 5;
    p.word     = p.source == ST_SRC_MORSE ? (uint8_t)ix : 0;
    p.r_offset = (int8_t)lroundf(kn_off.Value());
    p.rate     = (uint8_t)kn_rate.Value();
    p.swing    = kn_swing.Value() / 100.0f;
    p.bpm      = kn_tempo.Value();
    p.attack   = kn_att.Value() / 100.0f;
    p.gate     = kn_gate.Value() / 100.0f;
    p.release  = kn_rel.Value() / 100.0f;
    p.curve    = (float)((int)curve_sel.Value() - 1);
    p.depth    = G_BYPASS ? 0.0f : kn_depth.Value() / 100.0f;
    p.filter   = kn_filter.Value() / 100.0f;
    p.resonance = 0.3f;
    p.polarity = extras.polarity;
    p.mix      = kn_mix.Value() / 100.0f;

    /* the tempo pot moves the free tempo; tap and lock write it too */
    if (fabsf(p.bpm - extras.free_bpm) >= 0.5f) { extras.free_bpm = p.bpm; mark_dirty(now); }
    p.bpm = extras.free_bpm;

    if (!G_PARAMS_PENDING) { g_params_next = p; G_PARAMS_PENDING = true; }
    G_PPQN = kPpqn[(int)ppqn_sel.Value() & 3];
    G_MONO = (int)input_sel.Value() == 1;

    if ((g_store_changed || pattern_inputs_differ(p, g_built_for)) && st_pattern_writable(&ST))
    {
        st_build_pattern(&p, &extras.store, st_back_pattern(&ST));
        st_commit_pattern(&ST);
        g_built_for = p;
        g_store_changed = false;
    }

    {
        float mx = cpu.GetMaxCpuLoad();
        if (mx > 1.0f) mx = 1.0f;
        if (mx > extras.cpu_peak) extras.cpu_peak = mx;
        if (extras.cpu_peak > g_saved_peak + 0.05f) mark_dirty(now);
    }

    const bool sact = settings.IsActive();
    if (prev_settings && !sact) mark_dirty(now);
    prev_settings = sact;

    if (g_dirty && (now - g_dirty_since >= 5000u || now - g_dirty_first >= 30000u)
        && !sact && !picker::Busy()
        && !hw.buttons[kButtonB1].Pressed() && !hw.buttons[kButtonB2].Pressed()
        && !hw.buttons[kButtonB3].Pressed())
    {
        presets.Save(kHomeSlot);
        g_dirty = false;
        g_saved_peak = extras.cpu_peak;
    }
}

/* ---- LEDs ------------------------------------------------------------------- */

/* PATTERN's ring is the pattern: one LED per step, a page of the ring's
 * length at a time, open steps in the source's colour, the playhead white. */
static void PatternOverdraw(LedPanel& panel, uint8_t pot, const ArcGeometry& geo,
                            float norm, uint32_t t_ms, void* ctx)
{
    (void)norm; (void)t_ms; (void)ctx;
    const st_pattern_t* pt = st_front_pattern(&ST);
    const uint32_t n    = geo.arc_leds ? geo.arc_leds : 1u;
    const uint32_t step = G_STEP;
    const uint32_t base = (step / n) * n;
    const LedPanel::Rgb on = panel.ScaleGlobal(kSrcCol[extras.source % ST_SRC_COUNT]);
    for (uint32_t k = 0; k < n; k++)
    {
        const uint32_t i = base + k;
        LedPanel::Rgb c = kOff;
        if (i < pt->len) c = st_pattern_open(pt, 0, i) ? on : panel.ScaleGlobal(kColDim);
        if (i == step) c = panel.ScaleGlobal(kWhite);
        const float hour = geo.start_hour + geo.step_hours * (float)k;
        panel.SetRingByHour(pot, fmodf(hour, 12.0f), c);
    }
}

static void OnRender(uint32_t t_ms)
{
    LedPanel& L = hw.leds;
    if (settings.IsActive()) return;

    /* B1: the source; white flash when it changes */
    LedPanel::Rgb s = kSrcCol[extras.source % ST_SRC_COUNT];
    if (t_ms < g_src_flash_until) s = kWhite;
    L.SetButtonPair(kButtonB1, L.ScaleGlobal(s));

    /* B2: the clock. blue = J3 locked, grey = free, white flash on each beat
     * (free) and each tap, white while bypassed, red when the audio runs hot. */
    const bool locked = st_locked(&ST);
    LedPanel::Rgb ck = locked ? kBlue : kGrey;
    static uint32_t last_beats = 0, flash_until = 0;
    if (G_BEATS != last_beats) { last_beats = G_BEATS; flash_until = t_ms + 40u; }
    if (t_ms < flash_until && !locked) ck = kWhite;
    if (t_ms < g_tap_flash_until || G_BYPASS) ck = kWhite;
    if (cpu.GetAvgCpuLoad() > 0.80f) ck = kRed;
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(ck));

    /* B3: GATE (dim violet) or DUCK (orange), brightness following the envelope */
    if (pager.Page() != kPageShift)
    {
        const LedPanel::Rgb base = extras.polarity == ST_POL_DUCK ? kDuckCol : kGateCol;
        const float e = 0.35f + 0.65f * G_ENV;
        const LedPanel::Rgb c = {(uint8_t)(base.r * e), (uint8_t)(base.g * e), (uint8_t)(base.b * e)};
        L.SetButtonPair(kButtonB3, L.ScaleGlobal(c));
    }
}

/* ---- audio ------------------------------------------------------------------- */

static float    g_zero[kBlockSize];
static uint32_t g_tr_left = 0;
static uint32_t g_last_starts = 0;
static uint32_t g_last_beat = 0xFFFFFFFFu;

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    cpu.OnBlockStart();

    if (G_PARAMS_PENDING)
    {
        st_set_params(&ST, &g_params_next);
        G_PARAMS_PENDING = false;
    }
    if (g_reset.Poll()) st_reset(&ST);
    if (g_clock.Poll()) st_clock_edge(&ST, G_PPQN);

    if (size > kBlockSize)
    {
        for (size_t i = 0; i < size; i++) out[0][i] = out[1][i] = 0.0f;
        cpu.OnBlockEnd();
        return;
    }

    const float* inr = G_MONO ? in[0] : in[1];
    st_process(&ST, in[0], inr, out[0], out[1], nullptr, nullptr, (uint32_t)size);
    (void)g_zero;

    /* J7: the left envelope. J8: a pulse on each open step. */
    G_ENV = st_env(&ST, 0);
    hw.cv_jacks[kJackEnvOut].SetVolts(5.0f * G_ENV);
    if (ST.step_starts != g_last_starts) { g_last_starts = ST.step_starts; g_tr_left = kPulseFrames; }
    {
        static bool tr_hi = false;
        const bool tr = g_tr_left > 0;
        if (tr != tr_hi) { tr_hi = tr; hw.cv_jacks[kJackTrigOut].SetVolts(tr ? 5.0f : 0.0f); }
        g_tr_left = g_tr_left > size ? g_tr_left - (uint32_t)size : 0u;
    }

    G_STEP = st_step(&ST);
    const uint32_t beat = (uint32_t)(ST.pos / (double)st_rate_steps_per_beat[ST.p.rate % ST_RATE_COUNT]);
    if (beat != g_last_beat) { g_last_beat = beat; G_BEATS = G_BEATS + 1; }

    cpu.OnBlockEnd();
}

/* ---- boot --------------------------------------------------------------------- */

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlockSize);
    cpu.Init(hw.SampleRate(), (int)hw.BlockSize());
    st_init(&ST, hw.SampleRate());
    g_clock.Calibrate();
    g_reset.Calibrate();

    hw.cv_jacks[kJackEnvOut].EnableCvOutput();
    hw.cv_jacks[kJackEnvOut].SetVolts(0.0f);
    hw.cv_jacks[kJackTrigOut].EnableCvOutput();
    hw.cv_jacks[kJackTrigOut].SetVolts(0.0f);

    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);
    /* Pot 0 is brightness, 2 and 3 the SDK's preset pair. */
    curve_sel = settings.Page(kSettingsMain).Pot(1)
        .Selector(kCurveLabels).Default(1)
        .Ident("curve").Name("Curve")
        .Help("The slice envelope's shape. **Snappy** rises fast and falls "
              "slow, **Slow** the reverse, **Linear** in between.");
    ppqn_sel = settings.Page(kSettingsMain).Pot(4)
        .Selector(kPpqnLabels).Default(2)
        .Ident("clk.ppqn").Name("Clock In")
        .Help("Pulses per quarter note on J3: **1** quarter notes, **4** "
              "sixteenths (most sequencers' clock out), **24** MIDI-rate clock.");
    input_sel = settings.Page(kSettingsMain).Pot(5)
        .Selector(kInputLabels).Default(0)
        .Ident("input").Name("Input")
        .Help("**Mono L** feeds J1 to both channels, so a mono source still "
              "gets the stereo R OFFSET pattern.");

    /* CV: J3/J4 are read raw (clock/reset); J5 -> PATTERN, J6 -> DEPTH. */
    cv_matrix.Jack(0).Off();
    cv_matrix.Jack(1).Off();
    cv_matrix.Jack(2).To(kn_pat);
    cv_matrix.Jack(3).To(kn_depth);
    cv_matrix.Jack(4).Off();
    cv_matrix.Jack(5).Off();

    kn_pat.Overdraw(PatternOverdraw);

    host.Product("Stencil")
        .BootSlot(kHomeSlot)
        .Pages(play_page, shift_page)
        .Jacks(jk_in_l, jk_in_r, jk_clk, jk_rst, jk_cvpat, jk_cvdep, jk_env, jk_trig, jk_out_l, jk_out_r)
        .Buttons(bt_b1, bt_b2, bt_b3)
        .Attach(manual)
        .Extend(fs_ext)
        .Extend(st_ext);

    presets.Manage(pager);
    presets.Manage(settings);
    presets.Manage(extras);
    presets.Init();
    const bool had_home = presets.HasValid(kHomeSlot);
    presets.BootLoad();
    if (had_home) presets.Load(kHomeSlot);

    float phys[kNumPots];
    for (int i = 0; i < 8; i++) { hw.ProcessAllControls(); System::Delay(1); }
    for (uint8_t p = 0; p < kNumPots; p++) phys[p] = hw.pots[p].Value();

    if (!had_home)
    {
        /* first boot: a classic sixteenth trance gate at 120 */
        pager.SetStored(kPagePlay,  kPotTopLeft,     ((float)ST_RATE_1_16 + 0.5f) / ST_RATE_COUNT, phys);
        pager.SetStored(kPagePlay,  kPotTopRight,    pattern_norm(0, ST_SRC_BANK), phys);
        pager.SetStored(kPagePlay,  kPotMiddleLeft,  (50.f - 5.f) / 95.f, phys);  /* gate 50 %    */
        pager.SetStored(kPagePlay,  kPotMiddleRight, 0.02f,               phys);  /* attack 2 %   */
        pager.SetStored(kPagePlay,  kPotBottomLeft,  15.f / 400.f,        phys);  /* release 15 % */
        pager.SetStored(kPagePlay,  kPotBottomRight, 1.0f,                phys);  /* depth 100 %  */
        pager.SetStored(kPageShift, kPotTopLeft,     0.0f,                phys);  /* VCA gate     */
        pager.SetStored(kPageShift, kPotTopRight,    15.f / 31.f,         phys);  /* 16 steps     */
        pager.SetStored(kPageShift, kPotMiddleLeft,  0.5f,                phys);  /* no offset    */
        pager.SetStored(kPageShift, kPotMiddleRight, 0.0f,                phys);  /* no swing     */
        pager.SetStored(kPageShift, kPotBottomLeft,  1.0f,                phys);  /* all wet      */
        pager.SetStored(kPageShift, kPotBottomRight, bpm_to_norm(120.0f), phys);
    }
    extras.cpu_peak = 0.0f;

    hw.StartAudio(AudioCallback);
    cpu.Reset();

    loop.Use(pager)
        .Use(settings)
        .Use(cv_matrix)
        .Use(play_page)
        .Use(shift_page)
        .Use(host)
        .OnFrame(OnFrame)
        .OnPoll(OnPoll)
        .OnRender(OnRender);

    for (;;) loop.Tick();
}
