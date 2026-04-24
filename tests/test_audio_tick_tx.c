/*
 * test_audio_tick_tx.c — Unit tests for kerchunk_audio_tick_tx.
 *
 * Covers the TX sub-tick lifted from audio_thread_fn in Phase 3 of
 * PLAN-AUDIO-TICK.md. The critical items covered here:
 *   - item #3 (ARCH-COR-DTMF.md §12): queue-pause guard applies
 *     regardless of software_relay mode
 *   - tx_delay / tx_tail silence budgets + chunking
 *   - one-frame-per-tick drain invariant
 *   - FIRE_QUEUE_DRAIN on first drain, FIRE_QUEUE_COMPLETE at tail
 *   - tail-cancellation on courtesy-tone requeue (multi-pass protocol)
 *   - PTT release only after tail + ring drained + 3 hold ticks
 */

#include "../include/kerchunk_audio_tick.h"
#include <stdio.h>
#include <string.h>

extern void test_begin(const char *name);
extern void test_assert(int cond, const char *msg);
extern void test_end(void);

/* ── helpers ────────────────────────────────────────────────── */

static void init(kerchunk_audio_state_t *s, int software_relay)
{
    memset(s, 0, sizeof(*s));
    s->software_relay = software_relay;
    s->tx_delay_ms    = 100;
    s->tx_tail_ms     = 200;
    s->relay_drain_ms = 500;
    s->tx_tail_rem    = -1;
}

static int count_kind(const kerchunk_audio_tick_tx_out_t *o,
                      kerchunk_tx_action_kind_t kind)
{
    int n = 0;
    for (int i = 0; i < o->count; i++)
        if (o->actions[i].kind == kind) n++;
    return n;
}

static int sum_samples(const kerchunk_audio_tick_tx_out_t *o,
                       kerchunk_tx_action_kind_t kind)
{
    int s = 0;
    for (int i = 0; i < o->count; i++)
        if (o->actions[i].kind == kind) s += o->actions[i].samples;
    return s;
}

static int first_index(const kerchunk_audio_tick_tx_out_t *o,
                       kerchunk_tx_action_kind_t kind)
{
    for (int i = 0; i < o->count; i++)
        if (o->actions[i].kind == kind) return i;
    return -1;
}

/* Build a plain input snapshot with sensible defaults. */
static kerchunk_audio_tick_tx_in_t mk_in(void)
{
    kerchunk_audio_tick_tx_in_t in = {
        .relay_active      = 0,
        .ptt_held          = 0,
        .queue_depth       = 0,
        .queue_is_draining = 0,
        .play_writable     = 262143,   /* full ring writable */
        .play_pending      = 0,
        .now_us            = 1000000,
    };
    return in;
}

/* ── tests ──────────────────────────────────────────────────── */

void test_audio_tick_tx(void)
{
    const int rate = 48000;
    const int fs   = 960;

    /* 1. Idle state with empty queue → no actions. */
    test_begin("tick_tx: idle + empty queue is no-op");
    {
        kerchunk_audio_state_t s;
        init(&s, 0);
        kerchunk_audio_tick_tx_in_t in = mk_in();
        kerchunk_audio_tick_tx_out_t out;
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(out.count == 0, "no actions emitted");
        test_assert(s.queue_ptt == 0, "no PTT asserted");
        test_assert(out.rerun_this_tick == 0, "no rerun");
    }
    test_end();

    /* 2. queue-pause guard regardless of software_relay (ITEM #3). */
    test_begin("tick_tx: queue paused by COR when software_relay=0");
    {
        kerchunk_audio_state_t s;
        init(&s, 0 /* HARDWARE relay */);
        kerchunk_audio_tick_tx_in_t in = mk_in();
        in.queue_depth  = 1;
        in.relay_active = 1;   /* user is keyed up */
        kerchunk_audio_tick_tx_out_t out;
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_ASSERT_PTT) == 0,
            "no ASSERT while COR active (hw-relay setup)");
        test_assert(s.queue_ptt == 0, "queue_ptt stays 0");
    }
    test_end();

    test_begin("tick_tx: queue paused by COR when software_relay=1");
    {
        kerchunk_audio_state_t s;
        init(&s, 1 /* SOFTWARE relay */);
        kerchunk_audio_tick_tx_in_t in = mk_in();
        in.queue_depth  = 1;
        in.relay_active = 1;
        kerchunk_audio_tick_tx_out_t out;
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_ASSERT_PTT) == 0,
            "no ASSERT while COR active (sw-relay setup)");
    }
    test_end();

    test_begin("tick_tx: queue paused while relay drain > 0");
    {
        kerchunk_audio_state_t s;
        init(&s, 1);
        s.relay_drain = 1000;
        kerchunk_audio_tick_tx_in_t in = mk_in();
        in.queue_depth = 1;
        kerchunk_audio_tick_tx_out_t out;
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_ASSERT_PTT) == 0,
            "no ASSERT during relay drain");
    }
    test_end();

    /* 3. ASSERT + tx_delay when queue has items and COR is off. */
    test_begin("tick_tx: COR clear → ASSERT_PTT + tx_delay setup");
    {
        kerchunk_audio_state_t s;
        init(&s, 0);
        kerchunk_audio_tick_tx_in_t in = mk_in();
        in.queue_depth = 1;
        in.ptt_held    = 0;    /* no one else holds PTT */
        kerchunk_audio_tick_tx_out_t out;
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_ASSERT_PTT) == 1,
            "one ASSERT emitted");
        test_assert(s.queue_ptt == 1, "queue_ptt := 1");
        /* tx_delay_ms=100 → 4800 samples at 48kHz */
        /* After this tick's silence writes, remaining should be
         * 4800 - 4*960 = 4800 - 3840 = 960 (only 1 frame left) */
        test_assert(sum_samples(&out, KERCHUNK_TX_ACT_SILENCE) +
                    s.tx_delay_rem == (rate * 100) / 1000,
            "silence + tx_delay_rem = full tx_delay budget");
    }
    test_end();

    /* 4. ASSERT skips tx_delay when PTT already held by someone else. */
    test_begin("tick_tx: skip tx_delay when PTT already held");
    {
        kerchunk_audio_state_t s;
        init(&s, 0);
        kerchunk_audio_tick_tx_in_t in = mk_in();
        in.queue_depth = 1;
        in.ptt_held    = 1;    /* mod_repeater tail, say */
        kerchunk_audio_tick_tx_out_t out;
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_ASSERT_PTT) == 1,
            "still asserts our ref");
        test_assert(s.tx_delay_rem == 0, "tx_delay set to 0");
    }
    test_end();

    /* 5. Drain emits FIRE_DRAIN + DRAIN on first frame, then DRAIN only. */
    test_begin("tick_tx: drain fires QUEUE_DRAIN once, then DRAIN only");
    {
        kerchunk_audio_state_t s;
        init(&s, 0);
        s.queue_ptt      = 1;      /* already past assert */
        s.tx_delay_rem   = 0;      /* past delay */
        kerchunk_audio_tick_tx_in_t in = mk_in();
        in.queue_depth = 5;
        in.ptt_held    = 1;        /* our ref is held */
        in.now_us      = 1000000;

        /* Tick 1 */
        kerchunk_audio_tick_tx_out_t out;
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_FIRE_DRAIN) == 1,
            "FIRE_DRAIN emitted once");
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_DRAIN) == 1,
            "one DRAIN emitted");
        test_assert(first_index(&out, KERCHUNK_TX_ACT_FIRE_DRAIN) <
                    first_index(&out, KERCHUNK_TX_ACT_DRAIN),
            "FIRE_DRAIN comes before DRAIN");
        test_assert(s.queue_drain_start_us == 1000000,
            "drain start timestamp captured");

        /* Tick 2 — still draining, same frame count */
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_FIRE_DRAIN) == 0,
            "FIRE_DRAIN does NOT re-fire");
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_DRAIN) == 1,
            "still exactly one DRAIN per tick");
    }
    test_end();

    /* 6. Queue empty → tail transition fires FIRE_COMPLETE + rerun. */
    test_begin("tick_tx: queue empty → FIRE_COMPLETE + rerun");
    {
        kerchunk_audio_state_t s;
        init(&s, 0);
        s.queue_ptt         = 1;
        s.queue_fired_drain = 1;        /* pretend we already drained audio */
        s.queue_drain_start_us = 500000;
        s.tx_delay_rem      = 0;
        kerchunk_audio_tick_tx_in_t in = mk_in();
        in.queue_depth      = 0;
        in.queue_is_draining= 0;
        in.now_us           = 1500000;
        in.ptt_held         = 1;

        kerchunk_audio_tick_tx_out_t out;
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_FIRE_COMPLETE) == 1,
            "FIRE_COMPLETE emitted");
        test_assert(out.rerun_this_tick == 1,
            "rerun set so shell re-checks queue_depth after fire");
        /* Tail budget established */
        test_assert(s.tx_tail_rem == (rate * 200) / 1000,
            "tx_tail_rem set to full budget (200ms)");
        test_assert(s.queue_fired_drain == 0,
            "queue_fired_drain cleared (consumed by FIRE_COMPLETE)");
        /* Duration was 1500000 - 500000 = 1_000_000 us = 1000 ms */
        int found_complete = 0;
        for (int i = 0; i < out.count; i++) {
            if (out.actions[i].kind == KERCHUNK_TX_ACT_FIRE_COMPLETE) {
                test_assert(out.actions[i].duration_ms == 1000,
                    "duration_ms=1000ms");
                found_complete = 1;
            }
        }
        test_assert(found_complete, "FIRE_COMPLETE action found");
    }
    test_end();

    /* 7. Multi-pass tail-cancellation when subscribers requeue. */
    test_begin("tick_tx: tail cancels when queue refills mid-event");
    {
        kerchunk_audio_state_t s;
        init(&s, 0);
        s.queue_ptt         = 1;
        s.queue_fired_drain = 1;
        s.queue_drain_start_us = 500000;
        s.tx_delay_rem      = 0;

        /* Pass 1: queue empty → fire complete, rerun */
        kerchunk_audio_tick_tx_in_t in = mk_in();
        in.queue_depth = 0;
        in.ptt_held    = 1;
        in.now_us      = 1500000;
        kerchunk_audio_tick_tx_out_t out;
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(out.rerun_this_tick == 1, "pass 1: rerun");
        test_assert(s.tx_tail_rem > 0, "tail started");

        /* Simulate subscriber enqueuing — shell re-samples. */
        in.queue_depth = 2;
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(out.rerun_this_tick == 1, "pass 2: cancel rerun");
        test_assert(s.tx_tail_rem == -1, "tail cancelled");
        test_assert(s.queue_fired_drain == 1,
            "queue_fired_drain re-set so new DRAIN doesn't re-fire");
        test_assert(out.count == 0, "pass 2 emits no actions");

        /* Pass 3: drain proceeds. */
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(out.rerun_this_tick == 0, "pass 3: done");
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_FIRE_DRAIN) == 0,
            "no duplicate FIRE_DRAIN after cancel");
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_DRAIN) == 1,
            "drained one frame of the newly-queued item");
    }
    test_end();

    /* 8. Tail silence feeds AFTER the empty-queue tick (pass 1 rerun
     *    with queue still empty → second call drains tail silence). */
    test_begin("tick_tx: tail silence written after FIRE_COMPLETE + no requeue");
    {
        kerchunk_audio_state_t s;
        init(&s, 0);
        s.queue_ptt         = 1;
        s.queue_fired_drain = 1;
        s.queue_drain_start_us = 500000;
        s.tx_delay_rem      = 0;
        kerchunk_audio_tick_tx_in_t in = mk_in();
        in.queue_depth = 0;
        in.ptt_held    = 1;
        in.now_us      = 1500000;

        /* Pass 1: fires complete */
        kerchunk_audio_tick_tx_out_t out;
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(out.rerun_this_tick == 1, "pass 1 asks for rerun");

        /* Pass 2: queue still empty → feed tail silence */
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        /* tail budget 9600 samples; chunked in 960s → expect up to 10 silences.
         * actions array is size 8 so clamp happens; verify at least 6
         * silences emitted and no release yet. */
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_SILENCE) >= 6,
            "multiple tail silence chunks emitted");
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_RELEASE_PTT) == 0,
            "no release yet (ring not drained)");
    }
    test_end();

    /* 9. RELEASE fires only after tail done + play_pending=0 + 3 hold ticks. */
    test_begin("tick_tx: RELEASE_PTT requires hold_ticks=3");
    {
        kerchunk_audio_state_t s;
        init(&s, 0);
        s.queue_ptt      = 1;
        s.tx_delay_rem   = 0;
        s.tx_tail_rem    = 0;       /* tail done */
        s.queue_fired_drain = 0;    /* no pending fire */
        kerchunk_audio_tick_tx_in_t in = mk_in();
        in.queue_depth   = 0;
        in.play_pending  = 0;       /* ring drained */
        in.ptt_held      = 1;
        in.now_us        = 2000000;

        kerchunk_audio_tick_tx_out_t out;
        /* Tick 1: hold_ticks 0 → 1, no release */
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(s.ptt_hold_ticks == 1, "hold_ticks=1");
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_RELEASE_PTT) == 0,
            "no release after tick 1");

        /* Tick 2: 1 → 2 */
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(s.ptt_hold_ticks == 2, "hold_ticks=2");
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_RELEASE_PTT) == 0,
            "no release after tick 2");

        /* Tick 3: 2 → 3 → release */
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_RELEASE_PTT) == 1,
            "RELEASE_PTT on tick 3");
        test_assert(s.queue_ptt == 0, "queue_ptt cleared");
        test_assert(s.ptt_hold_ticks == 0, "hold_ticks reset");
    }
    test_end();

    /* 10. hold_ticks reset when play_pending > 0 (ring not drained). */
    test_begin("tick_tx: hold_ticks resets while ring not drained");
    {
        kerchunk_audio_state_t s;
        init(&s, 0);
        s.queue_ptt      = 1;
        s.tx_delay_rem   = 0;
        s.tx_tail_rem    = 0;
        s.ptt_hold_ticks = 2;       /* already accumulated */
        kerchunk_audio_tick_tx_in_t in = mk_in();
        in.queue_depth   = 0;
        in.play_pending  = 1000;    /* ring still has samples */
        in.ptt_held      = 1;

        kerchunk_audio_tick_tx_out_t out;
        kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
        test_assert(s.ptt_hold_ticks == 0,
            "hold_ticks reset when ring has pending samples");
        test_assert(count_kind(&out, KERCHUNK_TX_ACT_RELEASE_PTT) == 0,
            "no release");
    }
    test_end();

    /* 11. Full happy-path sequence: assert → delay → drain → complete → release.
     *     Tracks totals across the whole lifecycle rather than per-tick
     *     ordering (the tick function packs multiple actions per tick
     *     when play_writable allows). */
    test_begin("tick_tx: end-to-end transmit lifecycle");
    {
        kerchunk_audio_state_t s;
        init(&s, 0);
        s.tx_delay_ms = 40;
        s.tx_tail_ms  = 40;
        kerchunk_audio_tick_tx_in_t in = mk_in();
        in.queue_depth = 1;
        in.now_us      = 100;

        kerchunk_audio_tick_tx_out_t out;
        int asserts = 0, fire_drains = 0, drains = 0, fire_completes = 0, releases = 0;

        /* Simulate many ticks. Subscriber re-queues nothing. Loop until
         * we've seen a release, or we hit a hard cap. */
        for (int tick = 0; tick < 60 && releases == 0; tick++) {
            /* Queue empties once we've seen a drain. */
            if (drains >= 1) in.queue_depth = 0;
            /* Ring is drained after tail starts. */
            if (fire_completes >= 1) in.play_pending = 0;
            in.ptt_held = (s.queue_ptt || tick > 0) ? 1 : 0;
            in.now_us  = 100 + (uint64_t)tick * 20000;

            /* Execute up to 3 passes per tick (like the shell does) */
            for (int pass = 0; pass < 3; pass++) {
                kerchunk_audio_tick_tx(&s, &in, rate, fs, &out);
                asserts        += count_kind(&out, KERCHUNK_TX_ACT_ASSERT_PTT);
                fire_drains    += count_kind(&out, KERCHUNK_TX_ACT_FIRE_DRAIN);
                drains         += count_kind(&out, KERCHUNK_TX_ACT_DRAIN);
                fire_completes += count_kind(&out, KERCHUNK_TX_ACT_FIRE_COMPLETE);
                releases       += count_kind(&out, KERCHUNK_TX_ACT_RELEASE_PTT);
                if (!out.rerun_this_tick) break;
            }
        }
        test_assert(asserts == 1, "exactly one ASSERT over the lifecycle");
        test_assert(fire_drains == 1, "exactly one FIRE_DRAIN");
        test_assert(drains >= 1, "at least one DRAIN");
        test_assert(fire_completes == 1, "exactly one FIRE_COMPLETE");
        test_assert(releases == 1, "exactly one RELEASE_PTT");
        test_assert(s.queue_ptt == 0, "queue_ptt cleared after release");
    }
    test_end();

    /* 12. NULL-safe. */
    test_begin("tick_tx: NULL inputs are safe no-ops");
    {
        kerchunk_audio_tick_tx_out_t out = { .count = 99, .rerun_this_tick = 1 };
        kerchunk_audio_tick_tx(NULL, NULL, 48000, 960, &out);
        test_assert(out.count == 0, "NULL clears count");
        test_assert(out.rerun_this_tick == 0, "NULL clears rerun");
        /* NULL out shouldn't crash */
        kerchunk_audio_tick_tx(NULL, NULL, 48000, 960, NULL);
    }
    test_end();
}
