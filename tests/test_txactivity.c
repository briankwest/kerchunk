/*
 * test_txactivity.c — Unit tests for kerchunk_txactivity (the fused
 * COS-bit + DTMF-active TX presence detector).
 *
 * The module under test is a pure function over a state struct, so
 * tests script a sequence of (cos_raw, dtmf_active) inputs and
 * verify the returned edge events + state transitions.
 */

#include "../include/kerchunk_txactivity.h"
#include <stdio.h>
#include <string.h>

extern void test_begin(const char *name);
extern void test_assert(int cond, const char *msg);
extern void test_end(void);

/* ── helpers ────────────────────────────────────────────────── */

/* Step the detector N ticks with constant inputs; return total
 * count of each event type seen across those ticks. */
static void run_ticks(kerchunk_txactivity_t *s, int n,
                      int cos_raw, int dtmf_active,
                      int *begins, int *ends)
{
    int b = 0, e = 0;
    for (int i = 0; i < n; i++) {
        kerchunk_txact_event_t ev =
            kerchunk_txactivity_tick(s, cos_raw, dtmf_active);
        if (ev == KERCHUNK_TXACT_BEGIN) b++;
        else if (ev == KERCHUNK_TXACT_END) e++;
    }
    if (begins) *begins = b;
    if (ends)   *ends   = e;
}

/* ── tests ──────────────────────────────────────────────────── */

void test_txactivity(void)
{
    /* 1. init clamps end_silence_dtmf below end_silence to be >= it,
     *    and stores config faithfully. */
    test_begin("txactivity: init stores + clamps config");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 1);
        test_assert(s.end_silence_ticks == 15,      "end_silence_ticks");
        test_assert(s.end_silence_dtmf_ticks == 50, "end_silence_dtmf_ticks");
        test_assert(s.dtmf_grace_ticks == 150,      "dtmf_grace_ticks");
        test_assert(s.trust_cos_bit == 1,           "trust_cos_bit");
        test_assert(s.published == 0,               "starts unpublished");

        /* Clamp: dtmf < voice should be raised to voice */
        kerchunk_txactivity_init(&s, 15, 5, 100, 1);
        test_assert(s.end_silence_dtmf_ticks == 15,
            "dtmf clamp up to voice value");
    }
    test_end();

    /* 2. BEGIN fires on cos=1 alone */
    test_begin("txactivity: BEGIN on cos=1 alone");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 1);
        kerchunk_txact_event_t ev = kerchunk_txactivity_tick(&s, 1, 0);
        test_assert(ev == KERCHUNK_TXACT_BEGIN, "first cos=1 -> BEGIN");
        test_assert(s.published == 1,           "now published");
        /* second tick same input — no new event */
        ev = kerchunk_txactivity_tick(&s, 1, 0);
        test_assert(ev == KERCHUNK_TXACT_NONE, "no re-fire while held");
    }
    test_end();

    /* 3. BEGIN fires on dtmf_active=1 alone (the Retevis case where
     *    COS dropped during DTMF tone). */
    test_begin("txactivity: BEGIN on dtmf=1 alone");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 1);
        kerchunk_txact_event_t ev = kerchunk_txactivity_tick(&s, 0, 1);
        test_assert(ev == KERCHUNK_TXACT_BEGIN,
            "dtmf=1 with cos=0 -> BEGIN");
        test_assert(s.dtmf_seen_ago == 0, "dtmf_seen_ago reset on tone");
    }
    test_end();

    /* 4. cos_bit_sticky: HID -1 returns retain prior value. */
    test_begin("txactivity: HID -1 keeps last cos value");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 1);

        /* establish cos=1 */
        kerchunk_txactivity_tick(&s, 1, 0);
        test_assert(s.published == 1, "begin via cos=1");
        /* HID returns -1 — cos stays 1, no END */
        kerchunk_txact_event_t ev = kerchunk_txactivity_tick(&s, -1, 0);
        test_assert(ev == KERCHUNK_TXACT_NONE, "still receiving on -1");
        test_assert(s.cos_bit_sticky == 1, "sticky remained 1");

        /* now cos=0 explicitly — silent_ticks accumulates, no end yet */
        ev = kerchunk_txactivity_tick(&s, 0, 0);
        test_assert(ev == KERCHUNK_TXACT_NONE, "first silent tick no end");
        test_assert(s.silent_ticks == 1, "silent_ticks++");
    }
    test_end();

    /* 5. END fires after end_silence_ticks of all-quiet (voice mode). */
    test_begin("txactivity: END after end_silence_ticks (voice mode)");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 1);

        /* Begin */
        kerchunk_txact_event_t ev = kerchunk_txactivity_tick(&s, 1, 0);
        test_assert(ev == KERCHUNK_TXACT_BEGIN, "begin");

        /* Voice mode: dtmf_seen_ago is huge (init), so we use end_silence_ticks=15.
         * Push 14 quiet ticks → no END. */
        int b, e;
        run_ticks(&s, 14, 0, 0, &b, &e);
        test_assert(b == 0, "no spurious begins during silence");
        test_assert(e == 0, "no end before threshold");
        test_assert(s.published == 1, "still published");

        /* 15th tick should fire END */
        ev = kerchunk_txactivity_tick(&s, 0, 0);
        test_assert(ev == KERCHUNK_TXACT_END, "END at 15 ticks");
        test_assert(s.published == 0, "now unpublished");
        test_assert(s.silent_ticks == 0, "silent_ticks reset");
    }
    test_end();

    /* 6. DTMF-patient: while dtmf_seen_ago < grace, end_silence_dtmf
     *    is in effect — voice's 15-tick silence is NOT enough. */
    test_begin("txactivity: dtmf-patient extends silence window");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 1);

        /* Begin via dtmf */
        kerchunk_txact_event_t ev = kerchunk_txactivity_tick(&s, 0, 1);
        test_assert(ev == KERCHUNK_TXACT_BEGIN, "begin via dtmf");
        test_assert(kerchunk_txactivity_active_silence_ticks(&s) == 50,
            "dtmf-patient silence in effect after dtmf");

        /* Push 49 silent ticks — still in DTMF mode (seen 1 tick ago,
         * accumulating) and well within grace. */
        int b, e;
        run_ticks(&s, 49, 0, 0, &b, &e);
        test_assert(e == 0, "no end before dtmf-patient threshold");
        test_assert(s.published == 1, "still published at 49 ticks");

        /* 50th silent tick fires END */
        ev = kerchunk_txactivity_tick(&s, 0, 0);
        test_assert(ev == KERCHUNK_TXACT_END, "END at 50 dtmf-patient ticks");
    }
    test_end();

    /* 7. After dtmf_grace expires we revert to fast voice mode. */
    test_begin("txactivity: dtmf grace expires -> voice mode");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 1);

        /* Brief DTMF burst, then long quiet, then a new keyup */
        kerchunk_txactivity_tick(&s, 0, 1);                 /* begin via dtmf */
        kerchunk_txactivity_tick(&s, 0, 0);                 /* 1 quiet tick */
        /* Push past grace (150 ticks since last dtmf=1).
         * Expect END to fire at the 50-tick dtmf-patient mark
         * (dtmf still recent then). */
        int b, e;
        run_ticks(&s, 49, 0, 0, &b, &e);  /* 50 silent now total */
        test_assert(e == 1, "END at dtmf-patient threshold");
        test_assert(s.published == 0, "back to idle");

        /* Continue silence past grace expiry */
        run_ticks(&s, 200, 0, 0, &b, &e);  /* dtmf_seen_ago grows past grace */
        test_assert(s.dtmf_seen_ago > s.dtmf_grace_ticks,
            "grace has expired");
        test_assert(kerchunk_txactivity_active_silence_ticks(&s)
                    == s.end_silence_ticks,
            "voice mode active silence after grace");

        /* New cos-only keyup; brief silence; should END at voice timing. */
        kerchunk_txact_event_t ev = kerchunk_txactivity_tick(&s, 1, 0);
        test_assert(ev == KERCHUNK_TXACT_BEGIN, "new keyup BEGIN");
        run_ticks(&s, 14, 0, 0, &b, &e);
        test_assert(e == 0, "no END before voice threshold");
        ev = kerchunk_txactivity_tick(&s, 0, 0);
        test_assert(ev == KERCHUNK_TXACT_END, "END at voice threshold (15)");
    }
    test_end();

    /* 8. trust_cos_bit=0: cos input is ignored, only dtmf_active drives. */
    test_begin("txactivity: trust_cos_bit=0 ignores COS");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 0 /* trust_cos = false */);

        kerchunk_txact_event_t ev = kerchunk_txactivity_tick(&s, 1, 0);
        test_assert(ev == KERCHUNK_TXACT_NONE, "cos=1 ignored when trust=0");
        test_assert(s.cos_bit_sticky == 0, "sticky stays 0 when distrusted");

        ev = kerchunk_txactivity_tick(&s, 1, 1);
        test_assert(ev == KERCHUNK_TXACT_BEGIN,
            "BEGIN via dtmf (cos still ignored)");
    }
    test_end();

    /* 9. dtmf-active sustains presence even when cos=0 every tick
     *    (the textbook Retevis case). */
    test_begin("txactivity: dtmf sustains over cos chop");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 1);
        /* Begin via cos=1 */
        kerchunk_txactivity_tick(&s, 1, 0);
        /* Now cos=0 for many ticks, but dtmf_active=1 the whole time
         * (long held tone). No END should fire. */
        int b, e;
        run_ticks(&s, 200, 0, 1, &b, &e);
        test_assert(e == 0, "dtmf=1 prevents END indefinitely");
        test_assert(s.published == 1, "still receiving");
    }
    test_end();

    /* 10. Reassert during silence resets the counter AND pins the
     *     patient window (the 0→1 reassert edge IS the flap signal —
     *     see test 11a for the rationale). */
    test_begin("txactivity: reassert during silence resets counter");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 1);

        kerchunk_txactivity_tick(&s, 1, 0);  /* begin */
        /* 10 quiet ticks */
        int b, e;
        run_ticks(&s, 10, 0, 0, &b, &e);
        test_assert(s.silent_ticks == 10, "10 silent");

        /* reassert — this also triggers flap detection (cos 0→1 mid
         * session), so the silence window extends to patient (50). */
        kerchunk_txactivity_tick(&s, 1, 0);
        test_assert(s.silent_ticks == 0, "silent_ticks reset on reassert");
        test_assert(s.cos_flapped_session == 1, "reassert flags flap");

        /* Drop again — with flap flag set, window is now 50 ticks. */
        run_ticks(&s, 49, 0, 0, &b, &e);
        test_assert(e == 0, "no END before patient threshold (re-counted)");
        kerchunk_txact_event_t ev = kerchunk_txactivity_tick(&s, 0, 0);
        test_assert(ev == KERCHUNK_TXACT_END,
            "END at 50 ticks after reassert (patient window from flap)");
    }
    test_end();

    /* 11a. COS flap detection — Retevis-style CTCSS chop mid-keyup.
     *      Sequence: BEGIN on cos=1, cos drops to 0 (voice pause, CTCSS
     *      lost lock), cos comes back 1 (CTCSS re-locked), user still
     *      keying. The 0→1 transition should pin the session into the
     *      dtmf-patient silence window. Subsequent cos=0 then takes
     *      the longer window before END fires. */
    test_begin("txactivity: cos 0→1 mid-session pins patient window");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 1);
        kerchunk_txact_event_t ev;

        /* BEGIN on cos=1 */
        ev = kerchunk_txactivity_tick(&s, 1, 0);
        test_assert(ev == KERCHUNK_TXACT_BEGIN, "BEGIN");
        test_assert(s.cos_flapped_session == 0, "no flap yet");

        /* cos drops; NOT a flap yet (cos went 1→0, not 0→1).
         * Voice window still in effect. */
        test_assert(kerchunk_txactivity_active_silence_ticks(&s) == 15,
            "voice mode before any flap");
        ev = kerchunk_txactivity_tick(&s, 0, 0);
        test_assert(ev == KERCHUNK_TXACT_NONE, "still tracking");
        test_assert(s.cos_flapped_session == 0,
            "single 1→0 is not yet evidence of flap");

        /* cos comes back 1 — THIS is the flap-back-up. Pin patient. */
        ev = kerchunk_txactivity_tick(&s, 1, 0);
        test_assert(ev == KERCHUNK_TXACT_NONE, "still in same session");
        test_assert(s.cos_flapped_session == 1,
            "flap flag set on 0→1 mid-session");
        test_assert(kerchunk_txactivity_active_silence_ticks(&s) == 50,
            "silence window extended to dtmf-patient (50 ticks)");

        /* cos drops again; voice window (15 ticks) is NOT enough now.
         * Push 30 silent ticks — should still NOT END. */
        int b, e;
        run_ticks(&s, 30, 0, 0, &b, &e);
        test_assert(e == 0, "no END before patient threshold");
        test_assert(s.published == 1, "still asserted");

        /* Push to 50 silent ticks total — END. */
        run_ticks(&s, 20, 0, 0, &b, &e);
        test_assert(e == 1, "END at 50 ticks (patient window)");
        test_assert(s.cos_flapped_session == 0,
            "flap flag cleared on TX_END");
    }
    test_end();

    /* 11b. Clean radio (no flap) keeps the fast voice tail. */
    test_begin("txactivity: no flap → voice window preserved");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 1);
        kerchunk_txact_event_t ev;

        ev = kerchunk_txactivity_tick(&s, 1, 0);   /* BEGIN */
        test_assert(ev == KERCHUNK_TXACT_BEGIN, "BEGIN");

        /* cos drops clean at real unkey. No prior 0→1. */
        int b, e;
        run_ticks(&s, 14, 0, 0, &b, &e);
        test_assert(e == 0, "voice threshold not yet met");
        ev = kerchunk_txactivity_tick(&s, 0, 0);
        test_assert(ev == KERCHUNK_TXACT_END,
            "END at 15 ticks — fast voice tail preserved when no flap");
    }
    test_end();

    /* 11c. Flap flag is per-session: after END, next session starts
     *      fresh in voice mode. */
    test_begin("txactivity: flap flag clears between sessions");
    {
        kerchunk_txactivity_t s;
        kerchunk_txactivity_init(&s, 15, 50, 150, 1);

        /* Session 1: flap, then END. */
        kerchunk_txactivity_tick(&s, 1, 0);      /* BEGIN */
        kerchunk_txactivity_tick(&s, 0, 0);      /* drop */
        kerchunk_txactivity_tick(&s, 1, 0);      /* back up — flap set */
        test_assert(s.cos_flapped_session == 1, "flap set in session 1");
        int b, e;
        run_ticks(&s, 50, 0, 0, &b, &e);
        test_assert(e == 1, "session 1 END");
        test_assert(s.cos_flapped_session == 0, "cleared on END");

        /* Session 2: clean keyup, should get the fast window. */
        kerchunk_txactivity_tick(&s, 1, 0);      /* BEGIN */
        test_assert(kerchunk_txactivity_active_silence_ticks(&s) == 15,
            "session 2 starts fresh in voice mode");
    }
    test_end();

    /* 11. NULL-safe */
    test_begin("txactivity: NULL state is no-op");
    {
        kerchunk_txact_event_t ev = kerchunk_txactivity_tick(NULL, 1, 1);
        test_assert(ev == KERCHUNK_TXACT_NONE, "NULL returns NONE");
        kerchunk_txactivity_init(NULL, 0, 0, 0, 0);  /* no crash */
        test_assert(kerchunk_txactivity_active_silence_ticks(NULL) == 0,
            "NULL active_silence returns 0");
    }
    test_end();
}
