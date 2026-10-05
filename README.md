# Stencil

A rhythmic gate mask for two Daisy-based Eurorack modules, from one engine.
Feed it pads, strings or drones and it cuts them into rhythm: classic
staccato trance gates, sloppy sidechain pumping, filter-carved gaps, and
Morse code — type a word on the web page and the module gates your sound in
the shape of its dits and dahs.

- **Daisy Patch**: stereo in on IN 1/2, gated out on OUT 1/2, and the holes
  (what the gate took away) on OUT 3/4.
- **Hermetic Modular Alchemy Lab V2**: stereo in on J1/J2, out on J9/J10,
  with the pattern drawn on a pot ring.

Inspired by Bram Bos's *Perforator* (iOS AUv3, a rhythmic gate mask with
slice envelopes, an amp-EG and a 12 dB/oct lowpass gate, per-channel
patterns) and Audio Spices' *Ginger* (a Morse-code gate). Their manuals were
not available when this was written, so this is an instrument in their
spirit, not a clone of either. The name is a placeholder
(`/rename-project`).

## Status

**Built and unit-tested, never run on hardware** (2026-10-05). Neither
firmware has been flashed; nothing has been heard or measured. See "What is
unproven".

## Layout

```
core/     stencil.h, stencil.c      the engine (C99, no hardware)
test/     make -C test              native tests (ASan + UBSan), 256 checks
          make -C test web          the web page's JS vs the C engine, 70 checks
patch/    make -C patch             Daisy Patch -> build/stencil_patch.bin
alchemy/  make -C alchemy           Alchemy Lab -> build/stencil_alchemy.bin
docs/     index.html                the word page: type, audition, send
tools/    hostlink-fs.mjs           card access over HostLink (Alchemy)
```

Dependencies are taken by path from `~/tim-os/daisy-sdk`, as Relay does: the
Patch builds against `daisy-sdk/libDaisy`, the Lab against
`daisy-sdk/alchemy-sdk` and its vendored libDaisy. Both are `BOOT_SRAM` and
carry the family SD picker (`modules/` on the Patch card, `alchemy/` on the
Lab card).

## The engine

- **Pattern sources.** BANK: 16 factory 16-step patterns plus one user
  pattern (up to 32 steps, sent from the web page). EUCLID: K hits spread
  over N steps, starting on the one. MORSE: one of eight stored words in
  ITU timing — dit 1 step open, dah 3 open, 1 closed between elements, 3
  between letters, 7 between words and at the loop point (PARIS = 50 steps).
  Letters, digits and `.,?'!/()&:;=+-_"$@`; anything else is skipped. Up to
  48 characters, 1024 steps.
- **Stereo.** The left channel plays the pattern, the right plays it
  delayed by R OFFSET steps.
- **Slice envelope**, in fractions of a step: ATTACK 0–100 %, GATE 5–100 %
  (how long the step holds open), RELEASE 0–400 % (past 100 % it spills into
  the following steps, which is the "sloppy pump"), CURVE snappy / linear /
  slow. Linear segments with a 1 ms floor, so nothing clicks. In Morse a run
  of open steps (a dah) holds through as one element.
- **The gate.** DEPTH 0–100 % (how far closed steps close). FILTER 0 = VCA,
  100 % = a 12 dB/oct TPT state-variable lowpass that sweeps 30 Hz–18 kHz
  with the envelope, in between = both. RESONANCE for the lowpass.
- **GATE / DUCK.** GATE opens on open steps. DUCK inverts it: the level
  dips on each open step and recovers over RELEASE — bank 4 (quarters) in
  DUCK with a short GATE and a long RELEASE is four-on-the-floor sidechain.
- **Timing.** RATE 1/32 to 1/1 including triplets, SWING delays every second
  step by up to half a step. Free-running at TEMPO, or locked to a clock:
  the step position is hard-synced on each edge and the tempo measured from
  the edge period; 1, 2, 4 or 24 ppqn. No edge for 2 s (or 4 periods) →
  back to TEMPO. Edges closer than 2 ms are ignored as bounce.
- **Reset.** Within 5 ms after a clock edge it makes that edge step 1;
  later, the pattern holds at step 1 until the next edge.
- **MIX** dry/wet.

## Daisy Patch

| Panel | Role |
|---|---|
| IN 1 / IN 2 | audio in L / R (Input = Mono feeds IN 1 to both) |
| OUT 1 / OUT 2 | gated L / R |
| OUT 3 / OUT 4 | the holes: input minus gated, L / R |
| CTRL 1 + CV 1 | PATTERN (bank / hits / word, per Source) |
| CTRL 2 + CV 2 | GATE |
| CTRL 3 + CV 3 | RELEASE (square law, 0–400 %) |
| CTRL 4 + CV 4 | DEPTH |
| GATE IN 1 / 2 | clock / reset |
| GATE OUT | a 10 ms trigger on each open step (left) |
| CV OUT 1 / 2 | the L / R envelope, 0–5 V |
| TRS MIDI, Seed micro-USB | MIDI clock (24 ppqn, overrides GATE IN 1 while it runs), Start = reset, SysEx |
| encoder | turn = menu, push+turn = edit, click on Word = type a word, push+turn on Mods = picker |

Menu: Source, Word, Rate, Steps, Attack, Curve, Filter, Reso, Mode
(Gate/Duck), Swing, R Off, Mix, Tempo, ClkIn (ppqn), Input, Mods. Settings
and the eight words persist in QSPI (7 MB + 128 KB in, clear of Relay's
sector at 7 MB) two seconds after the last edit.

**Typing a word on the Patch.** Click (push and release) on the Word row:
turn changes the letter under the cursor, click moves to the next letter,
push+turn moves the cursor, hold one second to finish and save. The
letter's Morse is shown below it. It edits the word PATTERN currently
points at (in Morse mode), or word 1.

## Alchemy Lab

PLAY: RATE, PATTERN, GATE, ATTACK, RELEASE, DEPTH.
SHIFT (hold B3): FILTER, STEPS, R OFFSET, SWING, MIX, TEMPO.
Settings (B2+B3 two seconds): P1 brightness, P2 Curve, P3/P4 the SDK's
preset slot and action, P5 Clock In (1/2/4/24 ppqn), P6 Input (Stereo /
Mono L); page 2 is the SD firmware picker.

PATTERN's ring is the pattern: one LED per step, a ring's length at a time,
open steps lit in the source's colour, the playhead white.

| Button | Tap | Hold |
|---|---|---|
| B1 (amber bank / green Euclid / cyan Morse) | next source | GATE ↔ DUCK |
| B2 (grey free / blue locked, white beat) | tap tempo | bypass (gate held open) |
| B3 (violet GATE / orange DUCK, pulses with the envelope) | — | SHIFT page |

| J1 | J2 | J3 | J4 | J5 | J6 | J7 | J8 | J9 | J10 |
|---|---|---|---|---|---|---|---|---|---|
| in L | in R | clock | reset | CV → PATTERN | CV → DEPTH | envelope 0–5 V | step trigger | out L | out R |

J3/J4 are read raw from the ADC in the audio callback (as Belt's J3), with
1.5 V / 0.5 V thresholds on the deviation from the jack's calibrated 0 V.
Home preset slot **3** (autosaved 5 s after the last change); Break 4, seq 5,
Meld 6, MI 7–11, Smack 12, Mark 13, Belt 14, Relay 15.

## The web page (`docs/index.html`)

Type a word into one of eight slots and see it spelled (letters, dits and
dahs, and a paper-tape strip of the steps); audition it in the browser over
a pad, drone or noise with the same envelope rules; browse the bank, set
Euclid hits, draw a user pattern; send to the module. Open it from a file
or `localhost` in Chrome or Edge (WebSerial and WebMIDI need a secure
context).

- **Lab**: WebSerial to the front USB-C, HostLink framing (COBS, CRC32).
  HELLO must answer `stencil_alchemy`; then command `0x61` reads the eight
  words back and `0x60` carries a Stencil command (below). The web
  programmer and the card's file panel keep working on the same port —
  just not from two tabs at once.
- **Patch**: WebMIDI SysEx `F0 7D 53 54 <cmd> <payload> F7` on the Seed's
  micro-USB (or any MIDI interface into the TRS jack).

Commands (`st_apply_cmd()`, shared by both):

| cmd | payload | |
|---|---|---|
| 1 SET_WORD | slot 0–7, ASCII bytes (≤ 48) | lower case is raised; non-printables dropped |
| 2 SET_USER | length 1–32, 5 bytes of 7-bit mask, LSB first | |
| 3 SELECT | source (0 bank, 1 Euclid, 2 Morse), index | Lab: PATTERN pot catches from there. Patch: switches Source; the knob keeps choosing |

`docs/stencil-core.js` holds the page's Morse table, patterns and both wire
formats; `make -C test web` proves it spells every test word bit-for-bit as
the C engine does and that its payloads parse.

## What is unproven (2026-10-05)

Nothing has run on hardware. Roughly in order of "would surprise me":

1. **Alchemy HostLink custom commands.** 0x60/0x61 go through the SDK's
   `IHostlinkExtension`, which the filesystem block uses the same way; never
   exercised. Nor has a page other than the web programmer spoken HostLink
   to this board.
2. **Patch USB MIDI + SysEx from Chrome.** Relay's USB MIDI on the Seed's
   micro-USB is itself unheard; SysEx through libDaisy's USB parser is
   untested (128-byte buffer; the longest Stencil message is 57 bytes).
3. **J3/J4 clock on the Lab** read as raw ADC once per 1 ms block: clock
   jitter up to 1 ms, pulses under 1 ms missed. Same for the Patch's
   GATE INs (polled per block).
4. **Lab J1/J2 levels**: AC-coupled codec ins; the Lab's input gain for
   line-level vs modular-level sources not checked.
5. **CPU**: the lowpass computes `tanf`/`exp2f` per sample per channel when
   FILTER > 0. Light by estimate, not measured.
6. **Patch DTCM at 82 %** (libDaisy MIDI queues + picker), Lab DTCM 57 %,
   SRAM 53 %. Builds; not exercised.
7. The browser audition approximates the engine with WebAudio ramps; the
   firmware is the reference.

## Ideas not built

Per-step accents (Perforator's amp levels per step). A CV input that
switches source. Morse at "Farnsworth" spacing (letters fast, gaps long).
Sending the knob state from the page. A Launchpad step editor on the Lab.
