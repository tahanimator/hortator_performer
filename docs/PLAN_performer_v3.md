# Hortator Performer v3: JAM

v3 replaces v2's latched mute screen with one performance mode, JAM. Three design reviews agreed on the direction: a
performer's, an interaction designer's and a firmware architect's. The latched screen began as a peek and gained jam
keys, and most review findings sat on the seam between "peek" and "mode". v3 removes that seam.

## The organising idea

**JAM is a room with an easy door in and a deliberate door out.** Inside, every control is a performance control.
Nothing in it stops the clock, edits a sound, records or saves.

## The rules

1. **The door.**
   - GLO tapped opens JAM, latched.
   - GLO held peeks at it and closes on release.
   - Inside, only GLO held for 1 s leaves; the title band fills while you hold.
   - No other button leaves. A dead button says `HOLD GLO TO LEAVE`.
2. **One timing rule.**
   - Track changes, OPEN and SNAP land as SAVE › DEVICE › MUTE says: NOW, BAR (the next bar) or END (the pattern's
     end).
   - Variations always switch at the pattern's end.
   - OCT+ tapped: everything waiting lands now. OCT+ held with a key: that key's change lands now.
   - OCT− tapped: everything waiting is cancelled. STOP cancels it too.
3. **Toggles persist, gestures don't.**
   - Mutes and variations stay as set.
   - FILL (A4) plays only while held.
   - The four macro knobs stay where they are left. They are always shown, and FX tapped resets them.
4. **Pressing again never surprises.**
   - A track key toggles, so a second press before the change lands puts the track back.
   - OPEN and SNAP do the same thing however many times they are pressed.
   - A queued variation's key pressed again does nothing.
5. **One look.**
   - Screen: solid = sounding or on; outline = muted or off; hatched = about to change; dither = unavailable.
   - The title says what waits and where it lands (`T2 T3 > BAR`, `V3 > END`).
   - Messages are drawn in the title band, inverted.
6. **One LED language.**
   - Steady = on.
   - Blinking in time with PLAY = about to change.
   - Breathing = pressing this would do something (SNAP, OPEN, OCT±).
   - Dark = off.
7. **Setup lives elsewhere.**
   - GLO's GLOBAL / SYSTEM pages and the device page (MUTE timing, METRO, THRU, MIDI LEARN) are SAVE's, after COPY.
   - CLK changes are refused while playing.
   - MIDI LEARN starts from DEVICE › KNOB 4 and is switched off when JAM opens, so PLAY stays PLAY.

## The map

| Control | In JAM |
|---|---|
| White F3–F4 | tracks 1–8 (toggle) |
| A4 | FILL while held |
| F5 | SNAP: tap = back to it; hold 1 s = store (bar fills; let go between 0.3 s and 1 s: nothing) |
| G5 | OPEN: every track |
| Black F#3–A#4 | variations 1–8 |
| Knobs 1–4 | FILTER CRUSH THROW DEPTH |
| FX | tap: macros to 0 · hold: PERFORM key layer over JAM |
| SELECT | BPM |
| OCT+ / OCT− | land now / cancel |
| PLAY | start / stop |
| SAVE held | the cue map (other slots) |
| GLO | hold 1 s: leave |
| REC HOME EDIT SEQ LFO ARP ENV SCL | nothing (`HOLD GLO TO LEAVE`) |

The snapshot is taken automatically when JAM opens if none exists for the loaded project. It is RAM only, and it is
never re-taken over a stored one.

## Structure

- `firmware/src/jam.c` holds the whole layer: GLO, the keys, the panel, the LEDs and the screen. ui_input.c includes
  it, as it includes ui_learn.c.
- The hooks into upstream are small:
  - `ui_input` hands the panel to `jam_panel()`.
  - `ui_leds` calls `jam_leds()`.
  - `oct_leds` checks for pending changes.
  - `ui_draw` calls `jam_draw()`.
  - `fx_layer` gets the FX tap and allows the layer over JAM.
  - `cue_input` allows the cue map over JAM.
  - `seq_stop` clears the mute queue.
  - `pages.c` moves the GLO pages under SAVE.
  - DEVICE gains G_LEARN.
- The ISR side is unchanged from v2: mute routing, `mute_bar` with END via `cue.c pattern_end_dist`, and `bar_org`.
- Removed:
  - MENU › MUTE SCREEN (HOLD/LATCH)
  - the REC double-tap
  - the C#5/D#5/F#5 timing keys
  - A4's tap-latched fill
  - the v2 draw code in ui_draw.c (upstream's `draw_mute` is back, unused)
- Tests: `test_jam` in tests/ui_test.c, and the upstream mute tests rewritten for the door.

## Open: test on hardware

- Whether OCT+ held together with a key registers reliably on the FM-1.
- Whether the hatched tiles and the title band read well at arm's length on stage.
