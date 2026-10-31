# Performer v2: implementation plan

Fork: Meat Proxy (`tahanimator/hortator_performer`), on Hortator 0.26.0 + MUTE SCREEN: LATCH.
Source of the requirements: four reviews of 2026-10-30 (fact check and editorial review of the field guide, a live
performer's firmware review, a Digitakt player's firmware review). This plan covers the changes both performers
agreed on, the fixes the guide needs, and what is deferred.

Goal: the latched mute screen becomes a **self-contained performance mode**, like a Digitakt's mute mode: tracks,
scenes, fills, the master macros and the timing are all reachable without closing it, one-handed, with nothing
silently left on.

---

## 1. The latched screen, after this plan

```
 black:  F#3 G#3 A#3   C#4 D#4   F#4 G#4 A#4   C#5   D#5     F#5
         V1  V2  V3    V4  V5    V6  V7  V8    NOW   BAR     PTN        <- variations 1-8 | mute timing
 white:  F3 G3 A3 B3 C4 D4 E4 F4   G4    A4    B4   C5 D5 E5   F5    G5
         T1 T2 T3 T4 T5 T6 T7 T8   LRN*  FILL  (·)  (· · ·)    SNAP  OPEN
 knobs:  KNOB 1-4 = PERFORM FILTER / CRUSH / THROW / DEPTH      OCT+ held 1 s = macros to 0
 * G4 = MIDI LEARN with GLO held only (as now)    (·) = kept free for later (B4: staged mutes, phase 5)
```

Everything new keeps the existing rules: white = tracks, black = scenes and timing, the far keys are the
"whole band" keys, and every mute-type change obeys NOW / NEXT BAR / PTN END.

---

## 2. Phases

Each phase ends with host tests passing, a CI build, the installer redeployed, and a short hardware check.
Phases are independent enough to ship one at a time.

### Phase 0: the guide's errors (docs only, no firmware)

From the fact check and the editorial review; ship first because the guide is in use now.

| Page | Change |
|---|---|
| 05 Tap or hold | Fix "Tap HOME, it always brings you back" (HOME is swallowed while latched; the menu needs a hold). HOME's hold is 0.7 s, not 0.4. Drop the four "·" rows (ARP, LFO, SEL, PLAY) to stop the page overflowing (~200 px). ARP row: "MODE, X, Y, CHAOS". |
| 07 Latched mutes | "A tap on GLO still opens GLOBAL" only when not latched. Add learned PERFORM effects and mutes to "close it first for". Move "taps act on release" here from 05. |
| 06 Build too much | OCT+ needs LEN raised first. Add "Stop first" to saving. Add step 7 "Make a second scene" (variations). Warn: REC held on a SEQ page clears the track. |
| 08 Playing it | Reorder move 1 (latch, D#, mute, then PLAY). "Cues need a saved slot." Replace the repeated close-first list with "see page 07". |
| 09 Clock | Rewrite the four blocks situation-first. USB AUDIO: 10 CH MIX / 10 CH FX, device "Hortator 8ch". Remove the claim that footswitch PERFORM works latched. |
| 10 Recipes | Lub-dub: "REVERB SIZE up" (no decay knob on the reverb). |
| 02 Hello | Add "First five minutes" box; move "What this build adds" to 11. Drop "doors" metaphor (keep "drummers"). |
| 03 Panel | Add "Words we use" box (TRACKS/HOME, STEPS, KEYS, variation, cue). |
| 04 Keys | Move after 07 as reference; legend line; title "What the keys do". |
| 12 Quick card | OCT+ also ends MIDI LEARN. |
| All | Chart labels and Silkscreen kickers ≥ 13 px; remove the near-black accent option; one name for page 11. |

The guide is then revised again in phase 6 for the new features.

### Phase 1: the latched screen becomes self-contained (S)

**1a. Variation cues on the black keys.**
- `ui_input.c mute_keys()`: for `k < 8`, `(hits >> CUE_BK[k]) & 1` → `cue_var(k)` (project.c; it already handles
  "again = next step", "the playing one = cancel", refusals with a message).
- Only while the mute screen is up (held or latched): the black keys are already routed there by
  `seq.c keyboard_block` (`mute_mask || mute_latched`), so no ISR change.
- `ui_input.c ui_leds()`, the `ui.mute` branch: add the same variation LEDs as HOME (`cue.q_var` blinking,
  `var_cur` lit), i.e. reuse the `bk` block by setting `bk` when `ui.mute`.
- `cue_dead()` stays as is (it guards the SAVE-held cue map, not `cue_var`).
- `ui_draw.c draw_mute()`: show the variation in the footer (`V3`, `V3>V5` while queued) and `SAVE TO A SLOT FIRST` /
  `STOP SONG TO CUE` via the existing `ui_message`.

**1b. Fill on A4 (hold).**
- Make `fill_on` a bit set: `FILL_REC 1`, `FILL_KEY 2`, `FILL_LATCH 4` (seq.c `step_plays` already tests `!= 0`).
  `rec_fill()` sets/clears only `FILL_REC`; writers use IRQ-off read-modify-write.
- Latched: A4 (`key 16`) held → `FILL_KEY` while `(kb_mute >> 16) & 1` (kb_mute already tracks held keys and their
  release). Set in `glo_input()` each frame; cleared on unlatch.
- Top line shows `FILL` as it does for REC.

**1c. The knobs = PERFORM macros, and nothing stays on unseen.**
- Factor the macro turn out of `fx_layer()` into `perf_knob_turn(k, s)` (clamp, FILTER's stop at 0, `ml_pick_act`).
- `ui_input()` latched block: before `enc_drop()`, `perf_knob_turn()` for EN_K1..K4.
- `draw_mute()`: when any `perf_k[]` ≠ 0, the footer reads `FX F-40 C12 T0 D0` (inverted). FX button LED lit while any
  macro is on, on every screen (`ui_leds`).
- Latched, OCT+ held 1 s → `perf_k[] = 0`, message `FX CLEARED` (OCT+ does nothing else latched today).

Tests (tests/ui_test.c, a `test_latch_v2_*` group): black key queues variation n and its LED blinks, again = next
step; refused without a saved slot; A4 held plays FILL steps and release stops them; knobs move `perf_k` while
latched and not before; OCT+ held clears; FX LED reflects macros; existing latch, mute-screen and G4 tests unchanged.

### Phase 2: stage safety and feedback (S)

**2a. REC can't start REC ALL by accident while playing.**
With MUTE SCREEN = LATCH (the performance setting), a REC tap while the transport runs off TRACKS shows
`REC ALL? TAP AGAIN` and only a second tap within 1.5 s starts REC ALL (`rec_tap()` gate, `ui.arm`-style timer).
Stopped, or HOLD mode: as today. (Decision D1 below.)

**2b. Where the downbeat is.**
`draw_mute()`: a 16-pip bar strip in the free band (y 146..164) with the current sixteenth lit, plus `PTN 3/4`
(the longest track's step / length) at the right. Separate small canvas, redrawn only when the step changes
(sig = step index), so the screen's redraw cost stays low.

Tests: REC tap while latched-mode playing doesn't arm; second tap does; stopped is unchanged. Redraw test: the strip
redraws once a step, not every frame (tests/redraw_test.c).

### Phase 3: timing that agrees with the pattern (S–M)

**3a. One helper for "where does the pattern end".**
Move the "end of the longest track's pattern" search out of `cue.c cue_tick()` into
`pattern_end_dist(adv, &best)` so mutes, fills and cues share it. `cue_tick()` behaviour unchanged (cue_test.c).

**3b. A third mute timing: PTN END.**
- `settings.mutebar`: 0 NOW, 1 NEXT BAR, 2 PTN END. Persisted as today in `zoom` bit 1 (= mutebar != 0) plus the
  palette word's **bit 16** for PTN END, so an older build reads PTN END as NEXT BAR.
- `seq.c mute_bar()`: mode 2 applies `mute_q` when `pattern_end_dist() <= adv (+ lead)`, the same way cues land.
- Latched key: F#5 (key 25) = PTN END; C#5 / D#5 as now. GLOBAL 3/3 MUTE knob gets three values (params.c).
- Audit every `settings.mutebar` truth-test (ui.c `track_mute_toggle`, ui_draw.c, ui_input.c LEDs) for the third value.

**3c. NEXT BAR lines up after a cue.**
Today the bar is `(gclk.cnt + 1) & 15` counted from PLAY, so after a pattern whose length isn't a multiple of 16
sixteenths the bar drifts from the pattern start. Add `bar_org` (seq.c), set in `cue_switch()` to the sixteenth on
which the switch lands; `mute_bar()` and METRO's accent test `((gclk.cnt + 1 - bar_org) & 15) == 0`. Grids keeps
`gclk` untouched (its phase is its own).

**3d. Fill tap-latch.**
A4 tapped (released < 250 ms) → `FILL_LATCH` until the next pattern end (cleared by the ISR via
`pattern_end_dist()`); tapped again → cancelled. Held → as phase 1. Footer `FILL>END`.

Tests (cue_test.c, ui_test.c): PTN END lands with a cue on a 12-step pattern; NEXT BAR after a 12-step variation
lands on the new pattern's step 0; METRO accent follows; fill latch ends at pattern end; old projects/settings read
correctly; settings round-trip.

### Phase 4: the jam snapshot (M)

Digitakt's Temp Save / Reload, reduced to what a mute jam needs.
- RAM `snap = {mutes, var}`; taken automatically when the screen is first latched after PLAY (so there's always a
  "where I started"), replaced by holding **F5** 1 s (`SNAP STORED`).
- **F5 tap**: queue the snapshot: `mute_q = current ^ snap.mutes` (so it obeys NOW / BAR / PTN, and a second tap
  before it lands cancels it), plus `cue_var(snap.var)` if different. Footer `SNAP>`.
- **G5 tap**: queue all open (`mute_q = muted mask`). This replaces the "anchor" key: the snapshot *is* the anchor.
- Not saved to flash (decision D2).

Tests: snapshot auto-taken at first latch; F5 hold stores; F5 tap restores mutes and variation on the boundary;
G5 opens all; both cancel on a second tap; nothing written to flash.

### Phase 5: later (each optional, not started without a go-ahead)

| Item | Notes | Effort |
|---|---|---|
| Staged mutes (hold B4, tap tracks, release to commit) | removes the "tap again cancels" trap for Digitakt hands | S |
| Trig conditions PRE/!PRE, NEI/!NEI, !A:B, LAST | params.c COND_*, seq.c `step_plays`; older firmware plays them as 8/8 | M |
| PROB randomness option (reseed per PLAY, as now, or free-running) | seq.c:594 is deliberate upstream; make it a GLOBAL setting | S |
| Temp-jump (hold a black key: that variation for one cycle) | cue.c return slot | M |
| Toggle-mute / variation cue as MIDI LEARN actions; mute CC out | footswitch-driven mutes; the SMC-Mixer already toggles | M |
| Mutes not saved with SAVE (option) | see D3 | S |

### Phase 6: the guide, for v2

Keys page: a new latched map (the diagram in §1). Latched mutes page: the self-contained mode, knobs, fill,
snapshot. Playing it: the five moves without leaving the latch. Quick card: the new keys. Changelog and README
fork note.

---

## 3. Decisions for the owner (defaults in bold)

- **D1 REC while playing in LATCH mode:** **tap twice to start REC ALL** / block it entirely / leave as is.
- **D2 Snapshot:** **RAM only, auto-taken at first latch** / also saved with the project.
- **D3 Mutes in SAVE:** **saved as they are (today)** / saved all open / a menu option.
- **D4 Fill tap-latch length:** **until the pattern ends** / one bar (16 steps).
- **D5 PTN END key:** **F#5** (the spare top black key) / only on the GLOBAL 3/3 knob.

## 4. Risks and how they're handled

- **Audio ISR budget.** Phases 1–2 are main-loop only. 3b/3c/3d add one comparison per block in `mute_bar` and a
  shared `pattern_end_dist()` that `cue_tick()` already computes; `tests/target_budget.py` and the CPU tests guard it
  (they run on macOS / in the fork's CI only; the Linux host suite skips instruction counts).
- **Settings format.** New bits only in unused positions (palette bit 16); older firmware ignores them.
- **Muscle memory.** No existing gesture changes meaning, except: knobs now act while latched (they did nothing),
  black keys now cue while latched (they did nothing), OCT+ hold while latched (did nothing).
- **Testing on hardware.** Each phase: the host suite, the CI package, then on the FM-1: latch → play → each new key,
  a cue with odd lengths, a fill, power-cycle, the 30 s rule.

## 5. Order and size

| Phase | Size | Ships |
|---|---|---|
| 0 Guide fixes | S | docs |
| 1 Self-contained latch (variations, fill hold, knobs) | S | firmware + tests |
| 2 REC safety, bar strip | S | firmware + tests |
| 3 Pattern-end timing, bar re-phase, fill latch | S–M | firmware + tests |
| 4 Snapshot / all open | M | firmware + tests |
| 6 Guide v2 | S | docs |
| 5 Later items | each S–M | on request |
