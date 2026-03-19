/*
 * test_integ_stats.c — Integration tests for mod_stats
 *
 * Tests: RX/TX time tracking, per-user attribution, queue counting,
 *        persistence save/load, reset.
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

void test_integ_stats(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();

    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "stats", "persist", "on");
    kerchunk_config_set(cfg, "stats", "persist_file", "/tmp/kerchunk_test_stats.dat");
    kerchunk_config_set(cfg, "user.1", "name", "Brian");
    kerchunk_config_set(cfg, "user.1", "ctcss", "1000");
    kerchunk_config_set(cfg, "user.1", "access", "admin");
    kerchunk_config_set(cfg, "user.2", "name", "Alice");
    kerchunk_config_set(cfg, "user.2", "ctcss", "1318");
    kerchunk_config_set(cfg, "user.2", "access", "basic");
    kerchunk_user_init(cfg);

    mod_stats.load(&g_mock_core);
    stats_configure(cfg);

    /* 1. RX time tracked from COR cycle */
    test_begin("stats: RX time tracked");
    sim_cor_cycle(1000000, 3000000, 0);  /* 2 seconds */
    test_assert(g_ch.rx_count == 1, "rx_count wrong");
    test_assert(g_ch.rx_time_ms == 2000, "rx_time_ms wrong");
    test_end();

    /* 2. TX time tracked from PTT cycle */
    test_begin("stats: TX time tracked");
    sim_ptt_cycle(4000000, 5500000);  /* 1.5 seconds */
    test_assert(g_ch.tx_time_ms == 1500, "tx_time_ms wrong");
    test_end();

    /* 3. Per-user attribution */
    test_begin("stats: per-user attribution");
    sim_cor_cycle(6000000, 9000000, 1);  /* 3s, user 1 */
    sim_cor_cycle(10000000, 11000000, 2); /* 1s, user 2 */
    test_assert(g_user_count == 2, "user count wrong");
    {
        user_stats_t *u1 = find_or_create_user(1);
        user_stats_t *u2 = find_or_create_user(2);
        test_assert(u1 && u1->tx_count == 1, "user1 tx_count wrong");
        test_assert(u1 && u1->tx_time_ms == 3000, "user1 time wrong");
        test_assert(u2 && u2->tx_count == 1, "user2 tx_count wrong");
        test_assert(u2 && u2->tx_time_ms == 1000, "user2 time wrong");
    }
    test_end();

    /* 4. Longest/shortest RX tracked */
    test_begin("stats: longest/shortest RX");
    test_assert(g_ch.longest_rx_ms == 3000, "longest wrong");
    test_assert(g_ch.shortest_rx_ms == 1000, "shortest wrong");
    test_end();

    /* 5. Queue complete counted */
    test_begin("stats: queue items counted");
    mock_fire_simple(KERCHEVT_QUEUE_COMPLETE);
    mock_fire_simple(KERCHEVT_QUEUE_COMPLETE);
    test_assert(g_sys.queue_items == 2, "queue count wrong");
    test_assert(g_ch.tx_count == 2, "tx_count wrong");
    test_end();

    /* 6. TOT events counted */
    test_begin("stats: TOT counted");
    mock_fire_simple(KERCHEVT_TIMEOUT);
    test_assert(g_ch.tot_events == 1, "tot count wrong");
    test_end();

    /* 7. Reset clears everything */
    test_begin("stats: reset clears counters");
    {
        kerchunk_resp_t resp;
        resp_init(&resp);
        const char *argv[] = { "stats", "reset" };
        cli_stats(2, argv, &resp);
    }
    test_assert(g_ch.rx_count == 0, "rx not reset");
    test_assert(g_ch.tx_time_ms == 0, "tx_time not reset");
    test_assert(g_user_count == 0, "users not reset");
    test_assert(g_sys.queue_items == 0, "queue not reset");
    test_end();

    /* 8. Persistence: save and restore */
    test_begin("stats: persistence save/load");
    sim_cor_cycle(1000000, 6000000, 1);  /* 5s, user 1 */
    sim_ptt_cycle(7000000, 9000000);  /* 2s */
    mock_fire_simple(KERCHEVT_QUEUE_COMPLETE);
    test_assert(g_ch.rx_count == 1, "pre-save rx wrong");

    /* Save */
    save_stats();

    /* Clear in-memory */
    uint32_t saved_rx = g_ch.rx_count;
    uint64_t saved_rx_ms = g_ch.rx_time_ms;
    memset(&g_ch, 0, sizeof(g_ch));
    memset(&g_users, 0, sizeof(g_users));
    g_user_count = 0;
    memset(&g_sys, 0, sizeof(g_sys));

    /* Reload */
    g_persist = 1;
    load_stats();

    test_assert(g_ch.rx_count == saved_rx, "restored rx_count wrong");
    test_assert(g_ch.rx_time_ms == saved_rx_ms, "restored rx_time wrong");
    test_assert(g_user_count == 1, "restored user_count wrong");
    test_assert(g_sys.restarts == 1, "restart count wrong");
    test_end();

    /* 9. Per-user longest tracked */
    test_begin("stats: per-user longest TX");
    {
        user_stats_t *u = find_or_create_user(1);
        test_assert(u && u->longest_ms == 5000, "user longest wrong");
    }
    test_end();

    mod_stats.unload();
    kerchevt_shutdown();
    kerchunk_user_shutdown();
    kerchunk_config_destroy(cfg);

    unlink("/tmp/kerchunk_test_stats.dat");
}
