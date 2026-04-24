/*
 * test_tx_state.c — unit tests for kerchunk_tx_state_compute.
 *
 * Pure-function tests against scripted input structs. No core,
 * no mocks, no event bus. Covers every transition edge plus full-
 * cycle walks for the common TX scenarios.
 */

#include "../include/kerchunk_tx_state.h"
#include <stdio.h>
#include <string.h>

extern void test_begin(const char *name);
extern void test_assert(int cond, const char *msg);
extern void test_end(void);

/* Helpers — build an input struct with defaults + overrides. */
static kerchunk_tx_state_inputs_t mk_idle(void)
{
    kerchunk_tx_state_inputs_t in = {
        .cor_active = 0, .software_relay = 0,
        .queue_ptt = 0, .tx_delay_rem = 0, .tx_tail_rem = -1,
        .relay_drain = 0, .ptt_held = 0,
    };
    return in;
}

static const char *compute_name(const kerchunk_tx_state_inputs_t *in)
{
    return kerchunk_tx_state_name(kerchunk_tx_state_compute(in));
}

void test_tx_state(void)
{
    /* 1. IDLE by default */
    test_begin("tx_state: all-zero inputs → IDLE");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        test_assert(kerchunk_tx_state_compute(&in) == KERCHUNK_TX_IDLE,
                    "expected IDLE");
    }
    test_end();

    /* 2. RELAY on cor_active + software_relay */
    test_begin("tx_state: cor_active + software_relay → RELAY");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        in.cor_active = 1;
        in.software_relay = 1;
        test_assert(kerchunk_tx_state_compute(&in) == KERCHUNK_TX_RELAY,
                    "expected RELAY");
    }
    test_end();

    /* 3. cor_active alone (software_relay=0) is NOT RELAY */
    test_begin("tx_state: cor_active without software_relay → IDLE");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        in.cor_active = 1;
        in.software_relay = 0;
        test_assert(kerchunk_tx_state_compute(&in) == KERCHUNK_TX_IDLE,
                    "expected IDLE (hardware relay mode — radio handles it)");
    }
    test_end();

    /* 4. relay_drain > 0 → TAIL (even if software_relay is off now) */
    test_begin("tx_state: relay_drain > 0 → TAIL");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        in.relay_drain = 1000;
        test_assert(kerchunk_tx_state_compute(&in) == KERCHUNK_TX_TAIL,
                    "expected TAIL");
    }
    test_end();

    /* 5. queue_ptt + tx_delay_rem > 0 → DELAY */
    test_begin("tx_state: queue_ptt + tx_delay_rem > 0 → DELAY");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        in.queue_ptt = 1;
        in.tx_delay_rem = 4800;
        test_assert(kerchunk_tx_state_compute(&in) == KERCHUNK_TX_DELAY,
                    "expected DELAY");
    }
    test_end();

    /* 6. queue_ptt, delay drained, no tail → DRAIN */
    test_begin("tx_state: queue_ptt after tx_delay → DRAIN");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        in.queue_ptt = 1;
        in.tx_delay_rem = 0;
        in.tx_tail_rem  = -1;
        test_assert(kerchunk_tx_state_compute(&in) == KERCHUNK_TX_DRAIN,
                    "expected DRAIN");
    }
    test_end();

    /* 7. queue_ptt + tx_tail_rem >= 0 → TAIL */
    test_begin("tx_state: queue_ptt + tx_tail_rem >= 0 → TAIL");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        in.queue_ptt = 1;
        in.tx_delay_rem = 0;
        in.tx_tail_rem  = 9600;
        test_assert(kerchunk_tx_state_compute(&in) == KERCHUNK_TX_TAIL,
                    "expected TAIL");

        /* Also: tx_tail_rem = 0 (just crossed threshold) still TAIL,
         * not yet HOLD. HOLD requires queue_ptt=0. */
        in.tx_tail_rem = 0;
        test_assert(kerchunk_tx_state_compute(&in) == KERCHUNK_TX_TAIL,
                    "expected TAIL (tx_tail_rem=0, queue_ptt still 1)");
    }
    test_end();

    /* 8. queue_ptt=0, ptt still held → HOLD */
    test_begin("tx_state: queue released but ptt_held → HOLD");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        in.queue_ptt = 0;
        in.ptt_held  = 1;
        test_assert(kerchunk_tx_state_compute(&in) == KERCHUNK_TX_HOLD,
                    "expected HOLD");
    }
    test_end();

    /* 9. HOLD → IDLE when PTT fully releases */
    test_begin("tx_state: ptt_held=0 → IDLE");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        in.queue_ptt = 0;
        in.ptt_held  = 0;
        test_assert(kerchunk_tx_state_compute(&in) == KERCHUNK_TX_IDLE,
                    "expected IDLE");
    }
    test_end();

    /* 10. RELAY takes precedence over queue-driven states */
    test_begin("tx_state: RELAY precedence over queue_ptt");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        in.cor_active = 1;
        in.software_relay = 1;
        in.queue_ptt = 1;       /* hypothetical — queue-pause guard
                                 * prevents this in practice */
        in.tx_delay_rem = 4800;
        test_assert(kerchunk_tx_state_compute(&in) == KERCHUNK_TX_RELAY,
                    "RELAY wins over DELAY when both conditions true");
    }
    test_end();

    /* 11. NULL input → IDLE (safe default, no crash) */
    test_begin("tx_state: NULL input is safe, returns IDLE");
    {
        test_assert(kerchunk_tx_state_compute(NULL) == KERCHUNK_TX_IDLE,
                    "NULL input → IDLE");
    }
    test_end();

    /* 12. Names are accurate + distinct */
    test_begin("tx_state: name table complete + distinct");
    {
        const char *ni  = kerchunk_tx_state_name(KERCHUNK_TX_IDLE);
        const char *nr  = kerchunk_tx_state_name(KERCHUNK_TX_RELAY);
        const char *nde = kerchunk_tx_state_name(KERCHUNK_TX_DELAY);
        const char *ndr = kerchunk_tx_state_name(KERCHUNK_TX_DRAIN);
        const char *nt  = kerchunk_tx_state_name(KERCHUNK_TX_TAIL);
        const char *nh  = kerchunk_tx_state_name(KERCHUNK_TX_HOLD);
        test_assert(!strcmp(ni,  "TX_IDLE"),  "IDLE name");
        test_assert(!strcmp(nr,  "TX_RELAY"), "RELAY name");
        test_assert(!strcmp(nde, "TX_DELAY"), "DELAY name");
        test_assert(!strcmp(ndr, "TX_DRAIN"), "DRAIN name");
        test_assert(!strcmp(nt,  "TX_TAIL"),  "TAIL name");
        test_assert(!strcmp(nh,  "TX_HOLD"),  "HOLD name");

        /* Out-of-range → TX_UNKNOWN (not NULL). */
        const char *nu = kerchunk_tx_state_name((kerchunk_tx_state_t)99);
        test_assert(nu != NULL && !strcmp(nu, "TX_UNKNOWN"),
                    "out-of-range → TX_UNKNOWN");
    }
    test_end();

    /* 13. Full queue-driven TX lifecycle walk */
    test_begin("tx_state: full queue TX lifecycle IDLE→DELAY→DRAIN→TAIL→HOLD→IDLE");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        test_assert(!strcmp(compute_name(&in), "TX_IDLE"), "start IDLE");

        /* Queue asserts PTT, starts tx_delay countdown */
        in.queue_ptt = 1;
        in.tx_delay_rem = 4800;
        in.ptt_held = 1;  /* core refcount bumped on request_ptt */
        test_assert(!strcmp(compute_name(&in), "TX_DELAY"), "→ DELAY");

        /* tx_delay done, no tail yet, queue has audio */
        in.tx_delay_rem = 0;
        in.tx_tail_rem  = -1;
        test_assert(!strcmp(compute_name(&in), "TX_DRAIN"), "→ DRAIN");

        /* Queue emptied, tail silence starts */
        in.tx_tail_rem = 9600;
        test_assert(!strcmp(compute_name(&in), "TX_TAIL"), "→ TAIL");

        /* Tail silence drained, queue releases PTT ref */
        in.tx_tail_rem = 0;
        in.queue_ptt = 0;  /* release_ptt("queue") just fired */
        /* core refcount 0 on our ref but another module (mod_repeater
         * tail, say) might still hold — here we simulate the
         * ptt_hold_ticks window where we've dropped our ref but core
         * refcount stays above zero thanks to OUR previous request_ptt
         * ref still being pending release through ptt_hold_ticks. */
        /* Actually ptt_held==1 represents "someone still holds" */
        in.ptt_held = 1;
        test_assert(!strcmp(compute_name(&in), "TX_HOLD"), "→ HOLD");

        /* Hardware flushed, PTT released, refcount→0 */
        in.ptt_held = 0;
        test_assert(!strcmp(compute_name(&in), "TX_IDLE"), "→ IDLE");
    }
    test_end();

    /* 14. Full relay TX lifecycle walk */
    test_begin("tx_state: full relay TX lifecycle IDLE→RELAY→TAIL→IDLE");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        in.software_relay = 1;
        test_assert(!strcmp(compute_name(&in), "TX_IDLE"), "start IDLE");

        /* COR asserts; relay starts */
        in.cor_active = 1;
        in.ptt_held = 1;  /* request_ptt in relay path */
        test_assert(!strcmp(compute_name(&in), "TX_RELAY"), "→ RELAY");

        /* User unkeys, COR drops, relay_drain starts */
        in.cor_active = 0;
        in.relay_drain = 4800;
        test_assert(!strcmp(compute_name(&in), "TX_TAIL"), "→ TAIL");

        /* relay_drain exhausts */
        in.relay_drain = 0;
        in.ptt_held = 0;
        test_assert(!strcmp(compute_name(&in), "TX_IDLE"), "→ IDLE");
    }
    test_end();

    /* 15. Relay → queue handoff (courtesy tone after relay drain) */
    test_begin("tx_state: relay then queue transition");
    {
        kerchunk_tx_state_inputs_t in = mk_idle();
        in.software_relay = 1;
        in.cor_active = 1;
        in.ptt_held = 1;
        test_assert(!strcmp(compute_name(&in), "TX_RELAY"), "RELAY");

        /* COR drops; relay drain starts */
        in.cor_active = 0;
        in.relay_drain = 4800;
        test_assert(!strcmp(compute_name(&in), "TX_TAIL"), "relay drain tail");

        /* Queue starts while still in relay drain — queue-pause
         * guard should prevent this in practice, but the FSM
         * label falls through relay_drain first. */
        in.queue_ptt = 1;
        in.tx_delay_rem = 4800;
        test_assert(!strcmp(compute_name(&in), "TX_TAIL"),
                    "relay_drain still wins");

        /* Relay drain completes, queue takes over */
        in.relay_drain = 0;
        test_assert(!strcmp(compute_name(&in), "TX_DELAY"),
                    "now DELAY");
    }
    test_end();
}
