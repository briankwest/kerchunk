/*
 * kerchunk_rrd.c — Lightweight round-robin database for repeater metrics
 *
 * mmap'd fixed-size file. Writes go through page cache — no explicit
 * save/load. msync on close for crash safety.
 */

#define _GNU_SOURCE
#include "kerchunk_rrd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

struct kerchunk_rrd {
    rrd_file_t *data;
    size_t      size;
    int         fd;
};

kerchunk_rrd_t *kerchunk_rrd_open(const char *path)
{
    if (!path) return NULL;

    size_t file_size = sizeof(rrd_file_t);
    int created = 0;

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }

    if ((size_t)st.st_size < file_size) {
        /* New or undersized file — extend and zero-fill */
        if (ftruncate(fd, (off_t)file_size) < 0) { close(fd); return NULL; }
        created = 1;
    }

    void *map = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return NULL; }

    kerchunk_rrd_t *rrd = calloc(1, sizeof(*rrd));
    if (!rrd) { munmap(map, file_size); close(fd); return NULL; }

    rrd->data = (rrd_file_t *)map;
    rrd->size = file_size;
    rrd->fd   = fd;

    if (created || rrd->data->magic != RRD_MAGIC) {
        /* Initialize fresh */
        memset(rrd->data, 0, file_size);
        rrd->data->magic = RRD_MAGIC;
        time_t now = time(NULL);
        rrd->data->last_minute_ts = now;
        rrd->data->last_hour_ts   = now;
        rrd->data->last_day_ts    = now;
        rrd->data->counters.start_time = now;
        msync(map, file_size, MS_SYNC);
    } else {
        /* Existing file — bump restart counter */
        rrd->data->counters.restarts++;
        /* Accumulate previous session uptime */
        time_t now = time(NULL);
        if (rrd->data->counters.start_time > 0) {
            int64_t prev_session = now - rrd->data->counters.start_time;
            if (prev_session > 0)
                rrd->data->counters.total_uptime_ms +=
                    (uint64_t)prev_session * 1000;
        }
        rrd->data->counters.start_time = now;
    }

    return rrd;
}

void kerchunk_rrd_close(kerchunk_rrd_t *rrd)
{
    if (!rrd) return;
    if (rrd->data) {
        msync(rrd->data, rrd->size, MS_SYNC);
        munmap(rrd->data, rrd->size);
    }
    if (rrd->fd >= 0) close(rrd->fd);
    free(rrd);
}

void kerchunk_rrd_sync(kerchunk_rrd_t *rrd)
{
    if (rrd && rrd->data)
        msync(rrd->data, rrd->size, MS_ASYNC);
}

/* ── Ring rotation ── */

static void consolidate_minutes_to_hour(rrd_file_t *d)
{
    /* Sum all 60 minute slots into the current hour slot */
    rrd_slot_t *dst = &d->hours[d->hour_idx];
    memset(dst, 0, sizeof(*dst));
    for (int i = 0; i < RRD_MINUTES; i++) {
        dst->rx_count += d->minutes[i].rx_count;
        dst->tx_count += d->minutes[i].tx_count;
        dst->rx_ms    += d->minutes[i].rx_ms;
        dst->tx_ms    += d->minutes[i].tx_ms;
    }
}

static void consolidate_hours_to_day(rrd_file_t *d)
{
    /* Sum all 24 hour slots into the current day slot */
    rrd_slot_t *dst = &d->days[d->day_idx];
    memset(dst, 0, sizeof(*dst));
    for (int i = 0; i < RRD_HOURS; i++) {
        dst->rx_count += d->hours[i].rx_count;
        dst->tx_count += d->hours[i].tx_count;
        dst->rx_ms    += d->hours[i].rx_ms;
        dst->tx_ms    += d->hours[i].tx_ms;
    }
}

void kerchunk_rrd_tick(kerchunk_rrd_t *rrd)
{
    if (!rrd || !rrd->data) return;
    rrd_file_t *d = rrd->data;
    time_t now = time(NULL);

    /* Minute rotation */
    if (now - d->last_minute_ts >= 60) {
        int elapsed = (int)(now - d->last_minute_ts) / 60;
        if (elapsed > RRD_MINUTES) elapsed = RRD_MINUTES;
        for (int i = 0; i < elapsed; i++) {
            d->minute_idx = (d->minute_idx + 1) % RRD_MINUTES;
            memset(&d->minutes[d->minute_idx], 0, sizeof(rrd_slot_t));
        }
        d->last_minute_ts = now;
    }

    /* Hour rotation */
    if (now - d->last_hour_ts >= 3600) {
        int elapsed = (int)(now - d->last_hour_ts) / 3600;
        if (elapsed > RRD_HOURS) elapsed = RRD_HOURS;
        for (int i = 0; i < elapsed; i++) {
            consolidate_minutes_to_hour(d);
            d->hour_idx = (d->hour_idx + 1) % RRD_HOURS;
            memset(&d->hours[d->hour_idx], 0, sizeof(rrd_slot_t));
        }
        d->last_hour_ts = now;
    }

    /* Day rotation */
    if (now - d->last_day_ts >= 86400) {
        int elapsed = (int)(now - d->last_day_ts) / 86400;
        if (elapsed > RRD_DAYS) elapsed = RRD_DAYS;
        for (int i = 0; i < elapsed; i++) {
            consolidate_hours_to_day(d);
            d->day_idx = (d->day_idx + 1) % RRD_DAYS;
            memset(&d->days[d->day_idx], 0, sizeof(rrd_slot_t));
        }
        d->last_day_ts = now;
    }
}

/* ── Recording ── */

void kerchunk_rrd_record_rx(kerchunk_rrd_t *rrd, uint32_t dur_ms, int user_id)
{
    if (!rrd || !rrd->data) return;
    rrd_file_t *d = rrd->data;

    /* Current minute slot */
    d->minutes[d->minute_idx].rx_count++;
    d->minutes[d->minute_idx].rx_ms += dur_ms;

    /* Scalar counters */
    d->counters.rx_count++;
    d->counters.rx_time_ms += dur_ms;
    if (dur_ms > d->counters.longest_rx_ms)
        d->counters.longest_rx_ms = dur_ms;
    if (d->counters.shortest_rx_ms == 0 || dur_ms < d->counters.shortest_rx_ms)
        d->counters.shortest_rx_ms = dur_ms;
    if (dur_ms < 1000)
        d->counters.kerchunk_count++;

    /* Per-user */
    if (user_id > 0) {
        for (uint32_t i = 0; i < d->user_count; i++) {
            if (d->users[i].user_id == user_id) {
                d->users[i].tx_count++;
                d->users[i].tx_time_ms += dur_ms;
                d->users[i].last_heard = time(NULL);
                if (dur_ms > d->users[i].longest_ms)
                    d->users[i].longest_ms = dur_ms;
                return;
            }
        }
    }
}

void kerchunk_rrd_record_tx(kerchunk_rrd_t *rrd, uint32_t dur_ms)
{
    if (!rrd || !rrd->data) return;
    rrd_file_t *d = rrd->data;

    d->minutes[d->minute_idx].tx_count++;
    d->minutes[d->minute_idx].tx_ms += dur_ms;

    d->counters.tx_count++;
    d->counters.tx_time_ms += dur_ms;
    d->counters.queue_items++;
}

void kerchunk_rrd_inc(kerchunk_rrd_t *rrd, const char *counter)
{
    if (!rrd || !rrd->data || !counter) return;
    rrd_counters_t *c = &rrd->data->counters;

    if      (strcmp(counter, "dtmf")      == 0) c->dtmf_commands++;
    else if (strcmp(counter, "cwid")      == 0) c->cwid_count++;
    else if (strcmp(counter, "page")      == 0) c->pages_sent++;
    else if (strcmp(counter, "weather")   == 0) c->weather_count++;
    else if (strcmp(counter, "nws")       == 0) c->nws_alerts++;
    else if (strcmp(counter, "phone")     == 0) c->phone_calls++;
    else if (strcmp(counter, "tot")       == 0) c->tot_events++;
    else if (strcmp(counter, "emergency") == 0) c->emergency_count++;
    else if (strcmp(counter, "denied")    == 0) c->access_denied++;
    else if (strcmp(counter, "cdr")       == 0) c->cdr_records++;
}

const rrd_file_t *kerchunk_rrd_data(const kerchunk_rrd_t *rrd)
{
    return rrd ? rrd->data : NULL;
}

rrd_counters_t *kerchunk_rrd_counters(kerchunk_rrd_t *rrd)
{
    return (rrd && rrd->data) ? &rrd->data->counters : NULL;
}

rrd_user_t *kerchunk_rrd_user(kerchunk_rrd_t *rrd, int user_id, const char *name)
{
    if (!rrd || !rrd->data || user_id <= 0) return NULL;
    rrd_file_t *d = rrd->data;

    for (uint32_t i = 0; i < d->user_count; i++) {
        if (d->users[i].user_id == user_id)
            return &d->users[i];
    }

    if (d->user_count >= RRD_MAX_USERS) return NULL;

    rrd_user_t *u = &d->users[d->user_count++];
    memset(u, 0, sizeof(*u));
    u->user_id = user_id;
    if (name)
        snprintf(u->name, sizeof(u->name), "%s", name);
    else
        snprintf(u->name, sizeof(u->name), "user_%d", user_id);

    return u;
}

void kerchunk_rrd_reset(kerchunk_rrd_t *rrd)
{
    if (!rrd || !rrd->data) return;
    uint32_t restarts = rrd->data->counters.restarts;
    memset(rrd->data, 0, sizeof(rrd_file_t));
    rrd->data->magic = RRD_MAGIC;
    time_t now = time(NULL);
    rrd->data->last_minute_ts = now;
    rrd->data->last_hour_ts   = now;
    rrd->data->last_day_ts    = now;
    rrd->data->counters.start_time = now;
    rrd->data->counters.restarts = restarts;
    msync(rrd->data, rrd->size, MS_SYNC);
}
