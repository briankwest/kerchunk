/*
 * kerchunk_rrd.h — Lightweight round-robin database for repeater metrics
 *
 * Fixed-size mmap'd file with three ring buffers:
 *   Ring 1: 60 x 1-minute slots
 *   Ring 2: 24 x 1-hour slots (consolidated from minutes)
 *   Ring 3: 30 x 1-day slots (consolidated from hours)
 * Plus scalar counters and per-user stats.
 *
 * On startup: mmap the file, data is immediately available.
 * On each event: update in-place (page cache handles disk writes).
 * On shutdown: msync for safety.
 */

#ifndef KERCHUNK_RRD_H
#define KERCHUNK_RRD_H

#include <stdint.h>
#include <time.h>

#define RRD_MAGIC       0x4B525244  /* "KRRD" */
#define RRD_MINUTES     60
#define RRD_HOURS       24
#define RRD_DAYS        30
#define RRD_MAX_USERS   64

/* Per-slot data for ring buffers */
typedef struct {
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t rx_ms;
    uint32_t tx_ms;
} rrd_slot_t;

/* Per-user stats */
typedef struct {
    int32_t  user_id;
    char     name[32];
    uint32_t tx_count;
    uint64_t tx_time_ms;
    int64_t  last_heard;    /* unix timestamp */
    uint32_t longest_ms;
} rrd_user_t;

/* Scalar counters (cumulative, never reset by rotation) */
typedef struct {
    uint64_t rx_time_ms;
    uint64_t tx_time_ms;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t longest_rx_ms;
    uint32_t shortest_rx_ms;
    uint32_t kerchunk_count;
    uint32_t tot_events;
    uint32_t emergency_count;
    uint32_t access_denied;
    uint32_t dtmf_commands;
    uint32_t cwid_count;
    uint32_t pages_sent;
    uint32_t weather_count;
    uint32_t nws_alerts;
    uint32_t phone_calls;
    uint32_t queue_items;
    uint32_t cdr_records;
    uint32_t restarts;
    uint64_t total_uptime_ms;
    int64_t  start_time;
} rrd_counters_t;

/* The on-disk/mmap'd file layout */
typedef struct {
    uint32_t       magic;
    uint32_t       minute_idx;     /* current write pointer (0-59) */
    uint32_t       hour_idx;       /* current write pointer (0-23) */
    uint32_t       day_idx;        /* current write pointer (0-29) */
    int64_t        last_minute_ts; /* unix timestamp of last minute rotation */
    int64_t        last_hour_ts;
    int64_t        last_day_ts;
    rrd_slot_t     minutes[RRD_MINUTES];
    rrd_slot_t     hours[RRD_HOURS];
    rrd_slot_t     days[RRD_DAYS];
    rrd_counters_t counters;
    uint32_t       user_count;
    rrd_user_t     users[RRD_MAX_USERS];
} rrd_file_t;

/* Opaque handle */
typedef struct kerchunk_rrd kerchunk_rrd_t;

/* Open (or create) the RRD file. Returns NULL on failure. */
kerchunk_rrd_t *kerchunk_rrd_open(const char *path);

/* Close and msync. */
void kerchunk_rrd_close(kerchunk_rrd_t *rrd);

/* Call every ~1 second from the main tick. Handles minute/hour/day rotation. */
void kerchunk_rrd_tick(kerchunk_rrd_t *rrd);

/* Record an RX event (COR drop). dur_ms = duration of the transmission. */
void kerchunk_rrd_record_rx(kerchunk_rrd_t *rrd, uint32_t dur_ms, int user_id);

/* Record a TX event (queue complete). dur_ms = PTT duration. */
void kerchunk_rrd_record_tx(kerchunk_rrd_t *rrd, uint32_t dur_ms);

/* Increment a named counter. */
void kerchunk_rrd_inc(kerchunk_rrd_t *rrd, const char *counter);

/* Direct access to the mmap'd data (read-only for JSON/CLI output). */
const rrd_file_t *kerchunk_rrd_data(const kerchunk_rrd_t *rrd);

/* Get counters (mutable, for startup init). */
rrd_counters_t *kerchunk_rrd_counters(kerchunk_rrd_t *rrd);

/* Find or create a user entry. Returns NULL if full. */
rrd_user_t *kerchunk_rrd_user(kerchunk_rrd_t *rrd, int user_id, const char *name);

/* Reset all data to zero (preserves restart count). */
void kerchunk_rrd_reset(kerchunk_rrd_t *rrd);

/* Force sync to disk. */
void kerchunk_rrd_sync(kerchunk_rrd_t *rrd);

#endif
