/*
 * test_integ_stats.c — Integration tests for mod_stats
 *
 * Tests: RX/TX time tracking, per-user attribution, queue counting,
 *        persistence via mmap'd RRD, reset.
 */

#include "test_integ_mock.h"
#include <unistd.h>

#include "../modules/mod_stats.c"

/* Helper: simulate a COR cycle with timestamps */
static void sim_cor_cycle(uint64_t start_us, uint64_t end_us, int user_id)
{
    kerchevt_t e1 = {
        .type = KERCHEVT_COR_ASSERT,
        .timestamp_us = start_us,
    };
    kerchevt_fire(&e1);

    if (user_id > 0) {
        kerchevt_t id = {
            .type = KERCHEVT_CALLER_IDENTIFIED,
            .caller = { .user_id = user_id, .method = 1 },
        };
        kerchevt_fire(&id);
    }

    kerchevt_t e2 = {
        .type = KERCHEVT_COR_DROP,
        .timestamp_us = end_us,
    };
    kerchevt_fire(&e2);
}

/* Helper: simulate a PTT cycle */
static void sim_ptt_cycle(uint64_t start_us, uint64_t end_us)
{
    kerchevt_t e1 = {
        .type = KERCHEVT_PTT_ASSERT,
        .timestamp_us = start_us,
    };
    kerchevt_fire(&e1);

    kerchevt_t e2 = {
        .type = KERCHEVT_PTT_DROP,
        .timestamp_us = end_us,
    };
    kerchevt_fire(&e2);
}

static const char *TEST_RRD_PATH = "/tmp/kerchunk_test_stats.rrd";

void test_integ_stats(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();

    /* Clean up any leftover test file */
    unlink(TEST_RRD_PATH);

    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "stats", "rrd_file", TEST_RRD_PATH);
    kerchunk_config_set(cfg, "user.1", "name", "Brian");
    kerchunk_config_set(cfg, "user.1", "ctcss", "1000");
    kerchunk_config_set(cfg, "user.1", "access", "admin");
    kerchunk_config_set(cfg, "user.2", "name", "Alice");
    kerchunk_config_set(cfg, "user.2", "ctcss", "1318");
    kerchunk_config_set(cfg, "user.2", "access", "basic");
    kerchunk_user_init(cfg);

    mod_stats.load(&g_mock_core);
    stats_configure(cfg);

    const rrd_file_t *d = kerchunk_rrd_data(g_rrd);
    const rrd_counters_t *c = &d->counters;

    /* 1. RX time tracked from COR cycle */
    test_begin("stats: RX time tracked");
    sim_cor_cycle(1000000, 3000000, 0);  /* 2 seconds */
    test_assert(c->rx_count == 1, "rx_count wrong");
    test_assert(c->rx_time_ms == 2000, "rx_time_ms wrong");
    test_end();

    /* 2. TX time tracked from PTT cycle (duty cycle only — PTT
     * doesn't increment tx_count, that comes from queue_complete) */
    test_begin("stats: PTT cycle recorded");
    sim_ptt_cycle(4000000, 5500000);  /* 1.5 seconds */
    /* PTT drop doesn't update rrd tx counters directly — duty_add_ms
     * tracks it in-memory.  Verify no crash and rx still correct. */
    test_assert(c->rx_count == 1, "rx_count changed after PTT");
    test_end();

    /* 3. Per-user attribution */
    test_begin("stats: per-user attribution");
    sim_cor_cycle(6000000, 9000000, 1);  /* 3s, user 1 */
    sim_cor_cycle(10000000, 11000000, 2); /* 1s, user 2 */
    test_assert(d->user_count == 2, "user count wrong");
    {
        rrd_user_t *u1 = kerchunk_rrd_user(g_rrd, 1, NULL);
        rrd_user_t *u2 = kerchunk_rrd_user(g_rrd, 2, NULL);
        test_assert(u1 && u1->tx_count == 1, "user1 tx_count wrong");
        test_assert(u1 && u1->tx_time_ms == 3000, "user1 time wrong");
        test_assert(u2 && u2->tx_count == 1, "user2 tx_count wrong");
        test_assert(u2 && u2->tx_time_ms == 1000, "user2 time wrong");
    }
    test_end();

    /* 4. Longest/shortest RX tracked */
    test_begin("stats: longest/shortest RX");
    test_assert(c->longest_rx_ms == 3000, "longest wrong");
    test_assert(c->shortest_rx_ms == 1000, "shortest wrong");
    test_end();

    /* 5. Queue complete counted */
    test_begin("stats: queue items counted");
    mock_fire_simple(KERCHEVT_QUEUE_COMPLETE);
    mock_fire_simple(KERCHEVT_QUEUE_COMPLETE);
    test_assert(c->tx_count == 2, "tx_count wrong");
    test_end();

    /* 6. TOT events counted */
    test_begin("stats: TOT counted");
    mock_fire_simple(KERCHEVT_TIMEOUT);
    test_assert(c->tot_events == 1, "tot count wrong");
    test_end();

    /* 7. Reset clears everything */
    test_begin("stats: reset clears counters");
    {
        kerchunk_resp_t resp;
        resp_init(&resp);
        const char *argv[] = { "stats", "reset" };
        cli_stats(2, argv, &resp);
    }
    test_assert(c->rx_count == 0, "rx not reset");
    test_assert(c->tx_time_ms == 0, "tx_time not reset");
    test_assert(d->user_count == 0, "users not reset");
    test_assert(c->queue_items == 0, "queue not reset");
    test_end();

    /* 8. Persistence: data survives close/reopen */
    test_begin("stats: persistence via RRD");
    sim_cor_cycle(1000000, 6000000, 1);  /* 5s, user 1 */
    mock_fire_simple(KERCHEVT_QUEUE_COMPLETE);

    uint32_t saved_rx = c->rx_count;
    uint64_t saved_rx_ms = c->rx_time_ms;

    /* Close and reopen the RRD */
    kerchunk_rrd_sync(g_rrd);
    kerchunk_rrd_close(g_rrd);
    g_rrd = kerchunk_rrd_open(TEST_RRD_PATH);
    test_assert(g_rrd != NULL, "RRD reopen failed");

    /* Re-read pointers after reopen */
    d = kerchunk_rrd_data(g_rrd);
    c = &d->counters;

    test_assert(c->rx_count == saved_rx, "restored rx_count wrong");
    test_assert(c->rx_time_ms == saved_rx_ms, "restored rx_time wrong");
    test_assert(d->user_count == 1, "restored user_count wrong");
    test_end();

    /* 9. Per-user longest tracked */
    test_begin("stats: per-user longest TX");
    {
        rrd_user_t *u = kerchunk_rrd_user(g_rrd, 1, NULL);
        test_assert(u && u->longest_ms == 5000, "user longest wrong");
    }
    test_end();

    mod_stats.unload();
    kerchevt_shutdown();
    kerchunk_user_shutdown();
    kerchunk_config_destroy(cfg);

    unlink(TEST_RRD_PATH);
}
