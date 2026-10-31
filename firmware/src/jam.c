/* SPDX-License-Identifier: GPL-3.0-only
 * Hortator Performer (fork: Meat Proxy, 2026): the JAM screen, the one place the fork's performance layer lives.
 * Included by ui_input.c (as ui_learn.c is); its ISR side is seq.c's mute routing (mute_latched, kb_mute, mute_bar)
 * and cue.c's pattern_end_dist. docs/PLAN_performer_v3.md has the design; the rules it keeps:
 *
 *   the door      GLO tapped opens JAM (latched); GLO held peeks at it while held (and, as upstream, G4 then turns
 *                 MIDI LEARN on / off; SAVE > DEVICE KNOB 4 too); inside, only GLO held 1 s (a bar fills) leaves. No other button leaves, nothing else in it stops the clock, edits a sound or records.
 *   one timing    tracks, OPEN and SNAP land as DEVICE's MUTE says (NOW, the next BAR, the pattern's END);
 *                 variations at the pattern's end. OCT+ tapped: everything waiting lands now; OCT+ held with a key:
 *                 that key's change lands now. OCT- tapped: everything waiting is cancelled. STOP cancels it too.
 *   one look      solid = sounding / on, outline = muted / off, hatched = about to change; the title says what waits
 *                 and where it lands. LEDs: steady = on, blinking with PLAY = about to change, breathing = pressing
 *                 this does something.
 *
 * Keys: white F3..F4 tracks 1-8, A4 FILL (while held), F5 SNAP (tap: back to it; held 1 s: store), G5 OPEN (every
 * track); black F#3..A#4 variations 1-8. Knobs: the PERFORM macros FILTER CRUSH THROW DEPTH; FX tapped: all four to 0;
 * FX held: PERFORM's key layer over JAM. SELECT: BPM. PLAY: start / stop. SAVE held: the cue map. The rest: "HOLD GLO
 * TO LEAVE". */
#define JAM_PEEK_MS 400u                 /* GLO held this long (or with a key): a peek, closed on its release */
#define GLO_HOLD_MS JAM_PEEK_MS          /* (upstream's name: ui_preset.c) */
#define JAM_LEAVE_MS 1000u               /* GLO held this long inside JAM: leave */
#define JAM_STORE_MS 1000u               /* F5 held this long: the snapshot stored */
#define JAM_RECALL_MS 300u               /* F5 let go sooner: back to the snapshot; between the two: nothing */
#define KEY_FILL 16u                     /* A4 */
#define KEY_SNAP 24u                     /* F5 */
#define KEY_OPEN 26u                     /* G5 */
#define JAM_MS(t) ((t) * 1000u * FM1_TICKS_PER_US)

static struct {
    uint8_t mutes, var, valid;           /* the snapshot: the tracks muted, the variation (RAM only) */
    int8_t slot;                         /* the slot it was taken in (another one loaded: none) */
    uint32_t f5_t0;                      /* F5 down: its time | 1; bit 1 stored */
    uint32_t up_t0;                      /* OCT+ down: its time | 1; bit 1 a key used it (no tap on the release) */
} jam = {0, 0, 0, -1, 0, 0};

static uint8_t mutes_now(void)                          /* the tracks muted now, a bit each */
{
    uint32_t k;
    uint8_t m = 0;
    for (k = 0; k < NTRK; k++)
        m |= (uint8_t)((trk[k].p[P_MUTE] != 0) << k);
    return m;
}
static uint8_t jam_mutes_after(void) { return (uint8_t)(mutes_now() ^ song.mute_q); }   /* once what waits lands */
static int jam_var_waits(void) { return cue.q_var && !cue.q_slot; }
static int jam_pending(void) { return song.mute_q || jam_var_waits(); }
static int jam_now_held(void) { return (fm1_in.buttons >> panel.btn[B_OCTUP]) & 1u; }

/* ------------------------------------------------------------------ the queue --- */
static void jam_mute_now(uint8_t want)                  /* the tracks in want flip now, out of the queue */
{
    uint32_t k;
    fm1_irq_off();
    song.mute_q &= (uint8_t)~want;
    for (k = 0; k < NTRK; k++)
        if ((want >> k) & 1u) {
            trk[k].p[P_MUTE] = (int16_t)!trk[k].p[P_MUTE];
            if (trk[k].p[P_MUTE])
                panic_req |= (uint8_t)(1u << k);
        }
    fm1_irq_on();
}
static void jam_track(uint32_t k, int now)              /* a track key: a toggle (pressed again before it lands:
                                                         * back as it was) */
{
    if (now || !settings.mutebar)
        jam_mute_now((uint8_t)(1u << k));
    else
        track_mute_toggle(k);
}
static void jam_mutes_to(uint8_t target, int now)       /* every track to target's state: the same press again does
                                                         * the same thing (never a cancel) */
{
    uint8_t want = (uint8_t)(mutes_now() ^ target);
    if (now || !settings.mutebar) {
        jam_mute_now(want);
        fm1_irq_off();
        song.mute_q = 0;
        fm1_irq_on();
        return;
    }
    fm1_irq_off();
    song.mute_q = want;
    fm1_irq_on();
}
static void jam_var(uint32_t v, int now)                /* a black key: variation v at the pattern's end; again: no
                                                         * change (no mid-bar jump); with OCT+: at the next step */
{
    if (proj_loaded() >= 0 && !chain_busy() && cue.q_var == v + 1u && !now)
        return;
    if (cue_var(v) == 0 && now && cue.q_var == v + 1u)
        cue.now = 1;
}
static void jam_land_now(void)                          /* OCT+ tapped */
{
    if (!jam_pending()) {
        ui_message("NOTHING WAITING");
        return;
    }
    if (song.mute_q)
        jam_mute_now(song.mute_q);
    if (jam_var_waits())
        cue.now = 1;
    ui_message("NOW");
}
static void jam_cancel(void)                            /* OCT- tapped */
{
    if (!jam_pending()) {
        ui_message("NOTHING WAITING");
        return;
    }
    fm1_irq_off();
    song.mute_q = 0;
    fm1_irq_on();
    if (jam_var_waits())
        (void)cue_var(var_cur);                         /* (project.c: the playing one = the queue cancelled) */
    ui_message("CANCELLED");
}

/* --------------------------------------------------------------- the snapshot --- */
static void snap_take(void)
{
    jam.mutes = jam_mutes_after();
    jam.var = jam_var_waits() ? (uint8_t)(cue.q_var - 1u) : var_cur;
    jam.valid = 1;
    jam.slot = (int8_t)proj_loaded();
}
static void snap_recall(int now)
{
    if (!jam.valid) {
        ui_message("NO SNAP YET");
        return;
    }
    jam_mutes_to(jam.mutes, now);
    if (proj_loaded() >= 0 && !chain_busy()) {
        if (jam.var != var_cur)
            jam_var(jam.var, now);
        else if (jam_var_waits())
            (void)cue_var(var_cur);                     /* (the snapshot's variation plays: another one queued off) */
    }
    ui_message("BACK TO SNAP");
}

/* ------------------------------------------------------------ open and close --- */
static void jam_open(int latched)
{
    ui.mute = 1;
    mute_latched = (uint8_t)latched;
    ui.force = 1;
    grid_drop_holds();                                  /* a grid key held edits nothing under it */
    if (ui.ml.on && latched)
        ml_toggle();                                    /* (MIDI LEARN would take PLAY as its target; the peek keeps it:
                                                         * GLO held + G4 is its switch, as upstream's) */
    if (!jam.valid || jam.slot != (int8_t)proj_loaded())
        snap_take();                                    /* a "where I started", once per project */
}
static void jam_close(void)
{
    mute_latched = 0;
    ui.mute = 0;
    ui.force = 1;
    jam.f5_t0 = jam.up_t0 = 0;
}

/* GLO (replaces upstream's: its GLOBAL pages are SAVE's now, pages.c). Outside JAM: a tap (let go within
 * JAM_PEEK_MS, no key) opens it latched; held, or a key pressed with it, peeks (closed on the release). Inside: held
 * JAM_LEAVE_MS leaves (the title fills meanwhile); let go sooner: HOLD GLO TO LEAVE. Pressed in the menu, NAME, the
 * PRESETS list or a dialog: dead until let go; with PERFORM up or FX held: nothing */
#define GLO_DOWN 1u
#define GLO_OPEN 2u                      /* the peek is up */
#define GLO_DEAD 4u
#define GLO_PLAIN 8u
#define GLO_IN 16u                       /* pressed inside JAM: the way out */
#define GLO_EAT 32u                      /* done (left): the release does nothing */
static int glo_dead(void) { return ui.menu || ui.confirm || name_on() || pl_on(); }
static int cue_owns_keys(void);
static uint32_t jam_leave_frac(uint32_t now)            /* 0..8: how far GLO's hold out of JAM has come */
{
    uint32_t t = ui.glo_t0, e;
    if (!(t & GLO_IN) || (t & GLO_EAT))
        return 0;
    e = (now - (t & ~63u)) / (JAM_LEAVE_MS * 1000u * FM1_TICKS_PER_US / 8u);
    return e > 8u ? 8u : e;
}
static uint32_t jam_store_frac(uint32_t now)            /* 0..8: how far F5's hold to store has come */
{
    uint32_t e;
    if (!jam.f5_t0 || (jam.f5_t0 & 2u) || now - (jam.f5_t0 & ~3u) < JAM_MS(JAM_RECALL_MS))
        return 0;
    e = (now - (jam.f5_t0 & ~3u)) / (JAM_STORE_MS * 1000u * FM1_TICKS_PER_US / 8u);
    return e > 8u ? 8u : e;
}
static void jam_keys(uint32_t hits)                     /* the keys the ISR gave JAM (struck now) */
{
    uint32_t k;
    int now = jam_now_held();
    if (now && hits)
        jam.up_t0 |= 2u;                                /* (OCT+ used: its release lands nothing) */
    for (k = 0; k < NTRK; k++)
        if ((hits >> KEY_TRK_KEY[k]) & 1u)
            jam_track(k, now);
    if ((hits >> KEY_OPEN) & 1u) {
        jam_mutes_to(0, now);
        ui_message("ALL OPEN");
    }
    for (k = 0; k < 8u; k++)
        if ((hits >> CUE_BK[k]) & 1u)
            jam_var(k, now);
    if (((hits >> KEY_LEARN) & 1u) && !mute_latched && ((fm1_in.buttons >> panel.btn[B_GLO]) & 1u))
        ml_toggle();                                    /* the peek, GLO held + G4: MIDI LEARN on / off (upstream's) */
}
static void jam_held_keys(uint32_t now)                 /* A4 and F5: their holds */
{
    int a4 = ui.mute && ((kb_mute >> KEY_FILL) & 1u), f5 = ui.mute && ((kb_mute >> KEY_SNAP) & 1u);
    if (a4 != ((fill_on & FILL_KEY) != 0u))
        fill_set(FILL_KEY, a4);                         /* FILL: while held, only */
    if (f5 && !jam.f5_t0)
        jam.f5_t0 = now | 1u;
    if (f5 && !(jam.f5_t0 & 2u) && now - (jam.f5_t0 & ~3u) >= JAM_MS(JAM_STORE_MS)) {
        jam.f5_t0 |= 2u;
        snap_take();
        ui_message("SNAP STORED");
    }
    if (!f5 && jam.f5_t0) {
        uint32_t held = now - (jam.f5_t0 & ~3u);
        if (!(jam.f5_t0 & 2u) && ui.mute) {
            if (held < JAM_MS(JAM_RECALL_MS))
                snap_recall(jam_now_held());
            else
                ui_message("HOLD 1 S TO STORE");        /* (let go between: nothing, on purpose) */
        }
        jam.f5_t0 = 0;
    }
}
static void glo_input(uint32_t now, uint32_t notes)
{
    uint32_t bit = 1u << panel.btn[B_GLO], *t0 = &ui.glo_t0, hits = 0;
    if (kb_mute_hit && !cue_owns_keys()) {             /* (SAVE held: the cue map's, cue_input) */
        fm1_irq_off();
        hits = kb_mute_hit;
        kb_mute_hit = 0;
        fm1_irq_on();
    }
    if (fm1_in.buttons & bit) {
        if (!*t0)
            *t0 = (now & ~63u) | GLO_DOWN | (mute_latched ? GLO_IN : 0u);
        if (*t0 & GLO_IN) {
            if (!(*t0 & GLO_EAT) && now - (*t0 & ~63u) >= JAM_MS(JAM_LEAVE_MS)) {
                jam_close();
                ui_message("JAM CLOSED");
                *t0 |= GLO_EAT;
            }
        } else if (!(*t0 & (GLO_OPEN | GLO_DEAD | GLO_PLAIN))) {
            if (glo_dead())
                *t0 |= GLO_DEAD;
            else if (ui.layer || ui.fx_t0)
                *t0 |= GLO_PLAIN;
            else if (notes || hits || now - (*t0 & ~63u) >= JAM_MS(JAM_PEEK_MS)) {
                *t0 |= GLO_OPEN;
                jam_open(0);                            /* the peek */
            }
        }
    } else if (*t0) {
        uint32_t was = *t0;
        *t0 = 0;
        if (was & GLO_IN) {
            if (!(was & GLO_EAT))
                ui_message("HOLD GLO TO LEAVE");
        } else if (was & GLO_OPEN) {
            if (ui.mute && !mute_latched)
                jam_close();                            /* let go: the screen before */
        } else if (!(was & (GLO_DEAD | GLO_PLAIN)) && !glo_dead())
            jam_open(1);                                /* a tap: JAM, latched */
    }
    if (hits && ui.mute && !glo_dead())
        jam_keys(hits);
    jam_held_keys(now);
    if (jam.valid && jam.slot != (int8_t)proj_loaded() && !ui.mute)
        jam.valid = 0;                                  /* (another project: its own snapshot when JAM opens) */
    mute_mask = (mute_mask & ~bit) |                   /* (SAVE's bit: cue_input's) */
                ((*t0 & GLO_OPEN) || (!(*t0 & (GLO_DEAD | GLO_PLAIN)) && !glo_dead() && !ui.layer && !ui.fx_t0) ? bit
                                                                                                                : 0u);
}

/* the panel while JAM is up (ui_input, after PERFORM's fx_layer: FX held is its own) */
static void play_press(void);
static void perf_knob_turn(uint32_t k, int32_t s);
static void jam_panel(uint32_t pressed, uint32_t now)
{
    uint32_t k, dead = (1u << panel.btn[B_REC]) | (1u << panel.btn[B_HOME]) | (1u << panel.btn[B_EDIT]) |
                       (1u << panel.btn[B_SEQ]) | (1u << panel.btn[B_LFO]) | (1u << panel.btn[B_ARP]) |
                       (1u << panel.btn[B_ENV]) | (1u << panel.btn[B_SCL]);
    int32_t s;
    if ((pressed >> panel.btn[B_PLAY]) & 1u)
        play_press();
    if (pressed & dead)
        ui_message("HOLD GLO TO LEAVE");
    if (ui.home_t0)
        ui.home_t0 |= 2u;                               /* (no tap, no hold: no menu from JAM) */
    if (ui.rec_t0)
        ui.rec_t0 |= 2u;
    if ((pressed >> panel.btn[B_OCTDN]) & 1u)
        jam_cancel();
    if ((pressed >> panel.btn[B_OCTUP]) & 1u)
        jam.up_t0 = now | 1u;
    if (jam.up_t0 && !jam_now_held()) {
        if (!(jam.up_t0 & 2u))
            jam_land_now();
        jam.up_t0 = 0;
    }
    for (k = 0; k < 4u; k++)
        if ((s = panel_enc(EN_K1 + k)) != 0 && !ui.ml.on)   /* (LEARN on, the peek: the knobs pick nothing) */
            perf_knob_turn(k, s);
    if ((s = panel_enc(EN_SELECT)) != 0) {             /* SELECT: the tempo */
        char b[8];
        if (song.g[G_CLOCK]) {
            ui_say("CLK ", N_CLOCK[song.g[G_CLOCK]]);
        } else {
            song.g[G_BPM] = (int16_t)clamp(song.g[G_BPM] + accel(EN_SELECT, s, 200), GP[G_BPM].min, GP[G_BPM].max);
            fmt_int(b, song.g[G_BPM]);
            ui_say("BPM ", b);
        }
    }
    enc_drop();
}
static void jam_fx_tap(void)                            /* FX tapped in JAM: the macros back to 0 */
{
    if (!perf_macros_on()) {
        ui_message("FX: ALL AT 0");
        return;
    }
    perf_k[0] = perf_k[1] = perf_k[2] = perf_k[3] = 0;
    ui_message("FX CLEARED");
}

/* ---------------------------------------------------------------------- LEDs --- */
static int jam_beat(void)                               /* the blink "about to change": with PLAY's, on the beat */
{
    return song.playing ? ((song.tick / 64u) & 1u) == 0u
                        : (int)((fm1_ticks() / (250u * 1000u * FM1_TICKS_PER_US)) & 1u);
}
static void jam_leds(uint8_t *nl, uint8_t *nb)
{
    uint32_t k;
    int beat = jam_beat();
    {                                                   /* (the page under JAM is not where you are: its button dark) */
        uint8_t q = led_pos[panel.btn[FAM_BTN[cur_fam()]]];
        if (q != 0xFF)
            nl[q >> 3] &= (uint8_t)~(1u << (q & 7u));
    }
    led_put(nl, panel.btn[B_GLO], 1);
    for (k = 0; k < NTRK; k++)
        led_put(nl, 14u + KEY_TRK_KEY[k], (song.mute_q >> k) & 1u ? beat : !trk[k].p[P_MUTE]);
    if (proj_loaded() >= 0)
        for (k = 0; k < 8u; k++)
            led_put(nl, 14u + CUE_BK[k], jam_var_waits() && cue.q_var == k + 1u ? beat : k == var_cur);
    led_put(nl, 14u + KEY_FILL, fill_on != 0u);
    led_put(nl, 14u + KEY_LEARN, ui.ml.on);             /* (the peek: G4 is LEARN's switch) */
    led_put(nb, 14u + KEY_SNAP, jam.valid && !jam.f5_t0);
    led_put(nl, 14u + KEY_SNAP, jam.f5_t0 != 0u);
    led_put(nb, 14u + KEY_OPEN, jam_mutes_after() != 0u);
}

/* -------------------------------------------------------------------- screen --- */
/* 240 x 240: the top bar (draw_head) as everywhere; then two canvases, each drawn only when what it shows changes.
 *   A, y 16..139: the title band (JAM / PEEK; what waits and where it lands, a message, or a bar filling while GLO or
 *                 F5 is held) and the 8 tracks, two rows of 4: solid = sounding, outline = muted, hatched = about to
 *                 change. The track's number big, its model under it.
 *   B, y 140..239: the bar strip (where the one is; the landing step marked), the variations 1-8 (playing solid,
 *                 queued hatched), the four macros (one left on: its cell solid, a meter), FILL and SNAP chips. */
#define JAM_TW 56
#define JAM_TH 46
static void cv_hatch(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)   /* diagonal stripes, 2 of every 6 px */
{
    int32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (((i + j) % 6) < 2)
                cv_pset(x + i, y + j, c);
}
static void jam_chip(int32_t x, int32_t y, const char *s, int solid)   /* a word in a box: solid = on */
{
    int32_t w = md_text_w(&MD_F3, 2, s) + 8;
    if (solid)
        cv_rect(x, y, w, 15, C_INK);
    else
        cv_frame(x, y, w, 15, C_INK);
    md_text(x + 4, y + 3, &MD_F3, 2, s, solid ? C_BG : C_INK);
}
static uint32_t jam_longest(void)                       /* the track whose pattern ends the others' (cue.c) */
{
    uint32_t k, b = 0, bn = 0, bd = 1;
    for (k = 0; k < NTRK; k++) {
        uint32_t d, n = chain_beats(&trk[k], &d);
        if (n * bd > bn * d) {
            b = k;
            bn = n;
            bd = d;
        }
    }
    return b;
}
static void jam_status(char *b, uint32_t n)             /* what waits and where it lands, or the timing */
{
    static const char *const WHEN[3] = {"", " > BAR", " > END"};
    uint32_t k, len = 0, cnt = 0;
    uint8_t q = song.mute_q;
    b[0] = 0;
    if (!jam_pending()) {
        str_cpy(b, settings.mutebar == 2u ? "MUTES AT END" : settings.mutebar ? "MUTES ON BAR" : "MUTES NOW", n);
        return;
    }
    for (k = 0; k < NTRK; k++)
        cnt += (q >> k) & 1u;
    if (cnt > 3u) {
        b[len++] = (char)('0' + cnt);
        str_cpy(b + len, " TRK", n - len);
        len = str_len(b);
    } else
        for (k = 0; k < NTRK; k++)
            if ((q >> k) & 1u) {
                if (len)
                    b[len++] = ' ';
                b[len++] = 'T';
                b[len++] = (char)('1' + k);
            }
    if (q) {
        str_cpy(b + len, WHEN[settings.mutebar < 3u ? settings.mutebar : 1u], n - len);
        len = str_len(b);
    }
    if (jam_var_waits()) {
        str_cpy(b + len, len ? " V" : "V", n - len);
        len = str_len(b);
        b[len++] = (char)('0' + cue.q_var);
        b[len] = 0;
        str_cpy(b + len, " > END", n - len);
    }
}
static void jam_draw_a(uint32_t now)
{
    static uint32_t sig_a = 1u;
    uint32_t k, lf = jam_leave_frac(now), sf = jam_store_frac(now), sig;
    char b[28];
    if (ui.msg_t)
        str_cpy(b, ui.msg, sizeof b);
    else
        jam_status(b, sizeof b);
    sig = str_hash(17u, b) + mute_latched * 7u + lf * 131u + sf * 1009u + (ui.msg_t != 0) * 7919u;
    for (k = 0; k < NTRK; k++)
        sig = (sig ^ ((uint32_t)(trk[k].p[P_MUTE] != 0) + ((song.mute_q >> k) & 1u) * 2u +
                      (uint32_t)trk[k].p[P_MODEL] * 4u)) * 16777619u;
    if (!ui.force && sig == sig_a)
        return;
    sig_a = sig;
    cv_begin_at(240, 124, 16, C_BG);
    cv_oy = 0;
    cv_rect(0, 0, 1, 124, C_INK);
    cv_rect(239, 0, 1, 124, C_INK);
    jam_chip(4, 3, mute_latched ? "JAM" : "PEEK", mute_latched);
    if (lf || sf) {                                    /* a hold on its way: a bar filling, what it will do */
        const char *what = lf ? "LEAVING" : "STORING";
        uint32_t f = lf ? lf : sf, i;
        md_text(48, 6, &MD_F3, 2, what, C_INK);
        for (i = 0; i < 8u; i++) {                     /* 8 blocks, filling: the words stay clear of them */
            int32_t bx = 128 + (int32_t)i * 13;
            if (i < f)
                cv_rect(bx, 3, 11, 15, C_INK);
            else
                cv_frame(bx, 3, 11, 15, C_INK);
        }
    } else if (ui.msg_t) {                             /* a message: inverted, the loudest thing on the screen */
        cv_rect(44, 3, 192, 15, C_INK);
        md_text(140 - md_text_w(&MD_F3, 2, b) / 2, 6, &MD_F3, 2, b, C_BG);
    } else
        md_text(140 - md_text_w(&MD_F3, 2, b) / 2, 6, &MD_F3, 2, b, C_INK);
    for (k = 0; k < NTRK; k++) {
        int32_t x = 4 + (int32_t)(k % 4u) * 58, y = 24 + (int32_t)(k / 4u) * 50;
        int on = !trk[k].p[P_MUTE], wait = (song.mute_q >> k) & 1u;
        const char *mn = N_MODEL[(uint32_t)trk[k].p[P_MODEL] % NMODELS];
        uint16_t pen = on ? C_BG : C_INK;
        char d[2] = {(char)('1' + k), 0};
        if (on)
            cv_rect(x, y, JAM_TW, JAM_TH, C_INK);
        else {
            cv_frame(x, y, JAM_TW, JAM_TH, C_INK);
            cv_frame(x + 1, y + 1, JAM_TW - 2, JAM_TH - 2, C_INK);
        }
        if (wait) {                                    /* about to change: hatched, the number on a clear patch */
            cv_hatch(x + 3, y + 3, JAM_TW - 6, JAM_TH - 6, pen);
            cv_rect(x + JAM_TW / 2 - 10, y + 4, 20, 20, on ? C_INK : C_BG);
            cv_rect(x + 4, y + 29, JAM_TW - 8, 13, on ? C_INK : C_BG);
        }
        md_text(x + (JAM_TW - md_text_w(&MD_BIG, 2, d)) / 2, y + 6, &MD_BIG, 2, d, pen);
        md_text(x + (JAM_TW - md_text_w(&MD_F3, 2, mn)) / 2, y + 31, &MD_F3, 2, mn, pen);
    }
    cv_blit(0, 16);
}
static void jam_draw_b(void)
{
    static uint32_t sig_b = 1u;
    uint32_t k, best = jam_longest(), len = (uint32_t)clamp(trk[best].p[P_SLEN], 1, NSTEP), idx, bar, bars, pip,
                pips = 16u, mark = 99u, sig;
    char b[8];
    idx = song.playing ? (uint32_t)trk[best].seq_idx % len : 0u;
    bars = (len + 15u) / 16u;
    bar = idx / 16u;
    if (settings.mutebar == 2u) {                      /* END: the pattern's own steps; the last one marked */
        pip = song.playing ? idx % 16u : 99u;
        pips = bar + 1u < bars ? 16u : len - bar * 16u;
        if (bar + 1u == bars && song.mute_q)
            mark = pips - 1u;
    } else {                                            /* BAR (and NOW): the bar NEXT BAR counts */
        pip = song.playing ? bar16() : 99u;
        if (settings.mutebar && song.mute_q)
            mark = 15u;
    }
    sig = pip * 7u + pips * 131u + mark * 977u + bar * 7919u + bars * 104729u + var_cur * 3u + cue.q_var * 17u +
          (uint32_t)(proj_loaded() + 1) * 59u + fill_on * 1299709u + jam.valid * 15485863u +
          (uint32_t)(uint8_t)perf_k[0] * 2654435761u + (uint32_t)(uint8_t)perf_k[1] * 40503u +
          (uint32_t)(uint8_t)perf_k[2] * 65537u + (uint32_t)(uint8_t)perf_k[3] * 257u;
    if (!ui.force && sig == sig_b)
        return;
    sig_b = sig;
    cv_begin_at(240, 100, 140, C_BG);
    cv_oy = 0;
    cv_rect(0, 0, 1, 100, C_INK);
    cv_rect(239, 0, 1, 100, C_INK);
    cv_rect(0, 99, 240, 1, C_INK);
    for (k = 0; k < pips; k++) {                       /* the strip: every 4th pip taller (the beats) */
        int32_t x = 12 + (int32_t)k * 11, h = (k & 3u) ? 6 : 10;
        if (k == pip)
            cv_rect(x - 1, 2, 10, 16, C_INK);          /* (the playhead: wide and tall) */
        else
            cv_frame(x, 10 - h / 2, 8, h, C_INK);
        if (k == mark)
            cv_rect(x, 19, 8, 3, C_INK);               /* where what waits lands */
    }
    b[0] = (char)('1' + (bar < 8u ? bar : 8u));
    b[1] = '/';
    b[2] = (char)('0' + (bars < 9u ? bars : 9u));
    b[3] = 0;
    md_text(232 - md_text_w(&MD_F3, 2, b), 5, &MD_F3, 2, b, C_INK);
    if (proj_loaded() >= 0) {                          /* the variations: playing solid, queued hatched */
        for (k = 0; k < 8u; k++) {
            int32_t x = 4 + (int32_t)k * 29;
            int cur = k == var_cur, q = jam_var_waits() && cue.q_var == k + 1u;
            char d[2] = {(char)('1' + k), 0};
            if (cur)
                cv_rect(x, 26, 27, 20, C_INK);
            else
                cv_frame(x, 26, 27, 20, C_INK);
            if (q) {
                cv_hatch(x + 2, 28, 23, 16, C_INK);
                cv_rect(x + 8, 30, 11, 12, C_BG);
            }
            md_text(x + (27 - md_text_w(&MD_F3, 2, d)) / 2, 31, &MD_F3, 2, d, cur ? C_BG : C_INK);
        }
    } else
        md_text(120 - md_text_w(&MD_F3, 2, "SAVE A SLOT FOR VARIATIONS") / 2, 31, &MD_F3, 2,
                "SAVE A SLOT FOR VARIATIONS", C_INK);
    for (k = 0; k < 4u; k++) {                         /* the macros: a cell each, solid while not at 0 */
        static const char *const MN[4] = {"FILT", "CRSH", "THRW", "DPTH"};
        int32_t x = 4 + (int32_t)k * 58, v = perf_k[k], w;
        uint16_t pen = v ? C_BG : C_INK;
        if (v)
            cv_rect(x, 50, JAM_TW, 26, C_INK);
        else
            cv_frame(x, 50, JAM_TW, 26, C_INK);
        md_text(x + (JAM_TW - md_text_w(&MD_F3, 2, MN[k])) / 2, 53, &MD_F3, 2, MN[k], pen);
        w = (v < 0 ? -v : v) * 24 / 100;
        if (!k)                                         /* FILTER: from the middle, left LPF, right HPF */
            cv_rect(v < 0 ? x + 28 - w : x + 28, 67, w ? w : 1, 5, pen);
        else
            cv_rect(x + 4, 67, w * 2, 5, pen);
    }
    if (fill_on)
        jam_chip(4, 80, "FILL", 1);
    jam_chip(fill_on ? 52 : 4, 80, "SNAP", 0);
    if (!jam.valid)
        cv_dither(fill_on ? 52 : 4, 80, md_text_w(&MD_F3, 2, "SNAP") + 8, 15, C_BG);
    md_text(236 - md_text_w(&MD_F3, 2, "GLO HOLD: LEAVE"), 83, &MD_F3, 2, "GLO HOLD: LEAVE", C_INK);
    cv_blit(0, 140);
}
static void jam_draw(void)
{
    uint32_t now = fm1_ticks();
    draw_head();
    jam_draw_a(now);
    jam_draw_b();
    ui.force = 0;
    if (ui.msg_t)
        ui.msg_t--;
    if (ui.bpm_t)
        ui.bpm_t--;
}
