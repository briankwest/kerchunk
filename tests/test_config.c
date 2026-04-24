/*
 * test_config.c — INI config parser tests
 */

#include "kerchunk_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void test_begin(const char *name);
extern void test_end(void);
extern void test_assert(int cond, const char *msg);

static const char *write_temp_config(void)
{
    static char path[] = "/tmp/test_kerchunk_XXXXXX";
    memcpy(path, "/tmp/test_kerchunk_XXXXXX", sizeof(path));
    int fd = mkstemp(path);
    if (fd < 0) return NULL;

    const char *content =
        "[general]\n"
        "callsign = WRZZ123\n"
        "sample_rate = 8000\n"
        "log_level = debug\n"
        "\n"
        "# Comment line\n"
        "[audio]\n"
        "capture_device = plughw:1,0\n"
        "preemphasis = on\n"
        "preemphasis_alpha = 0.95\n"
        "\n"
        "[user.1]\n"
        "name = Brian\n"
        "ctcss = 1000\n"
        "access = 2\n"
        "\n"
        "[user.2]\n"
        "name = Alice\n"
        "ctcss = 1318\n"
        "access = 1\n";

    ssize_t n = write(fd, content, strlen(content));
    (void)n;
    close(fd);
    return path;
}

void test_config(void)
{
    const char *path = write_temp_config();

    test_begin("load INI file");
    kerchunk_config_t *cfg = kerchunk_config_load(path);
    test_assert(cfg != NULL, "load returned NULL");
    test_end();

    test_begin("get string value");
    const char *cs = kerchunk_config_get(cfg, "general", "callsign");
    test_assert(cs && strcmp(cs, "WRZZ123") == 0, "callsign mismatch");
    test_end();

    test_begin("get int value");
    int rate = kerchunk_config_get_int(cfg, "general", "sample_rate", 0);
    test_assert(rate == 8000, "sample_rate mismatch");
    test_end();

    test_begin("get float value");
    float alpha = kerchunk_config_get_float(cfg, "audio", "preemphasis_alpha", 0.0f);
    test_assert(alpha > 0.94f && alpha < 0.96f, "alpha mismatch");
    test_end();

    test_begin("missing key returns default");
    int missing = kerchunk_config_get_int(cfg, "general", "nonexistent", 42);
    test_assert(missing == 42, "default not returned");
    test_end();

    test_begin("missing section returns NULL");
    const char *v = kerchunk_config_get(cfg, "nosection", "nokey");
    test_assert(v == NULL, "should be NULL");
    test_end();

    test_begin("comments are skipped");
    test_assert(kerchunk_config_get(cfg, "general", "# Comment line") == NULL, "comment leaked");
    test_end();

    test_begin("user.1 section parsed");
    const char *name = kerchunk_config_get(cfg, "user.1", "name");
    test_assert(name && strcmp(name, "Brian") == 0, "user.1 name wrong");
    test_end();

    test_begin("user.2 section parsed");
    name = kerchunk_config_get(cfg, "user.2", "name");
    test_assert(name && strcmp(name, "Alice") == 0, "user.2 name wrong");
    test_end();

    test_begin("set and get value");
    kerchunk_config_set(cfg, "test", "key", "value123");
    const char *tv = kerchunk_config_get(cfg, "test", "key");
    test_assert(tv && strcmp(tv, "value123") == 0, "set/get mismatch");
    test_end();

    test_begin("overwrite existing value");
    kerchunk_config_set(cfg, "test", "key", "newvalue");
    tv = kerchunk_config_get(cfg, "test", "key");
    test_assert(tv && strcmp(tv, "newvalue") == 0, "overwrite failed");
    test_end();

    test_begin("iterate sections");
    int iter = 0;
    int section_count = 0;
    while (kerchunk_config_next_section(cfg, &iter))
        section_count++;
    test_assert(section_count >= 4, "too few sections");
    test_end();

    test_begin("reload config");
    test_assert(kerchunk_config_reload(cfg) == 0, "reload failed");
    cs = kerchunk_config_get(cfg, "general", "callsign");
    test_assert(cs && strcmp(cs, "WRZZ123") == 0, "reload lost data");
    test_end();

    test_begin("create empty config");
    kerchunk_config_t *empty = kerchunk_config_create();
    test_assert(empty != NULL, "create returned NULL");
    test_assert(kerchunk_config_get(empty, "x", "y") == NULL, "empty not empty");
    kerchunk_config_destroy(empty);
    test_end();

    /* ── duration_ms parser (bare digits = ms) ── */

    test_begin("duration_ms: bare digits = milliseconds");
    test_assert(kerchunk_parse_duration_ms("500", 0) == 500, "500 != 500ms");
    test_assert(kerchunk_parse_duration_ms("100", 0) == 100, "100 != 100ms");
    test_assert(kerchunk_parse_duration_ms("0", 0) == 0, "0 != 0ms");
    test_end();

    test_begin("duration_ms: suffix s");
    test_assert(kerchunk_parse_duration_ms("10s", 0) == 10000, "10s != 10000ms");
    test_assert(kerchunk_parse_duration_ms("1s", 0) == 1000, "1s != 1000ms");
    test_end();

    test_begin("duration_ms: suffix m");
    test_assert(kerchunk_parse_duration_ms("5m", 0) == 300000, "5m != 300000ms");
    test_assert(kerchunk_parse_duration_ms("1m", 0) == 60000, "1m != 60000ms");
    test_end();

    test_begin("duration_ms: suffix h");
    test_assert(kerchunk_parse_duration_ms("1h", 0) == 3600000, "1h != 3600000ms");
    test_end();

    test_begin("duration_ms: suffix ms");
    test_assert(kerchunk_parse_duration_ms("500ms", 0) == 500, "500ms != 500ms");
    test_end();

    test_begin("duration_ms: compound 1h30m");
    test_assert(kerchunk_parse_duration_ms("1h30m", 0) == 5400000, "1h30m wrong");
    test_end();

    test_begin("duration_ms: compound 1m30s");
    test_assert(kerchunk_parse_duration_ms("1m30s", 0) == 90000, "1m30s wrong");
    test_end();

    test_begin("duration_ms: decimal 0.5s");
    test_assert(kerchunk_parse_duration_ms("0.5s", 0) == 500, "0.5s != 500ms");
    test_end();

    test_begin("duration_ms: decimal 0.2m");
    test_assert(kerchunk_parse_duration_ms("0.2m", 0) == 12000, "0.2m != 12000ms");
    test_end();

    test_begin("duration_ms: decimal 1.5s");
    test_assert(kerchunk_parse_duration_ms("1.5s", 0) == 1500, "1.5s != 1500ms");
    test_end();

    test_begin("duration_ms: empty/null returns default");
    test_assert(kerchunk_parse_duration_ms(NULL, 42) == 42, "NULL != default");
    test_assert(kerchunk_parse_duration_ms("", 42) == 42, "empty != default");
    test_end();

    test_begin("duration_ms: garbage returns default");
    test_assert(kerchunk_parse_duration_ms("abc", 99) == 99, "garbage != default");
    test_end();

    /* ── duration_s parser (bare digits = seconds) ── */

    test_begin("duration_s: bare digits = seconds");
    test_assert(kerchunk_parse_duration_s("10", 0) == 10, "10 != 10s");
    test_assert(kerchunk_parse_duration_s("300", 0) == 300, "300 != 300s");
    test_end();

    test_begin("duration_s: suffix s");
    test_assert(kerchunk_parse_duration_s("30s", 0) == 30, "30s != 30");
    test_end();

    test_begin("duration_s: suffix m");
    test_assert(kerchunk_parse_duration_s("5m", 0) == 300, "5m != 300");
    test_end();

    test_begin("duration_s: decimal 0.2m");
    test_assert(kerchunk_parse_duration_s("0.2m", 0) == 12, "0.2m != 12s");
    test_end();

    test_begin("duration_s: decimal 0.5m");
    test_assert(kerchunk_parse_duration_s("0.5m", 0) == 30, "0.5m != 30s");
    test_end();

    test_begin("duration_s: empty returns default");
    test_assert(kerchunk_parse_duration_s(NULL, 10) == 10, "NULL != default");
    test_end();

    /* ── config_get_duration_s via config ── */

    test_begin("config_get_duration_s with suffix");
    kerchunk_config_set(cfg, "test", "dur", "10s");
    test_assert(kerchunk_config_get_duration_s(cfg, "test", "dur", 0) == 10, "10s via config");
    test_end();

    test_begin("config_get_duration_s bare digits = seconds");
    kerchunk_config_set(cfg, "test", "dur", "30");
    test_assert(kerchunk_config_get_duration_s(cfg, "test", "dur", 0) == 30, "30 via config");
    test_end();

    test_begin("config_get_duration_s missing key = default");
    test_assert(kerchunk_config_get_duration_s(cfg, "test", "nope", 99) == 99, "missing = default");
    test_end();

    /* ── [txactivity] + [dtmf] new-architecture config plumbing ── */

    test_begin("config: [txactivity] end_silence_ms parses");
    kerchunk_config_set(cfg, "txactivity", "end_silence_ms", "300");
    test_assert(
        kerchunk_config_get_duration_ms(cfg, "txactivity", "end_silence_ms", -1) == 300,
        "end_silence_ms");
    test_end();

    test_begin("config: [txactivity] end_silence_dtmf_ms parses");
    kerchunk_config_set(cfg, "txactivity", "end_silence_dtmf_ms", "1s");
    test_assert(
        kerchunk_config_get_duration_ms(cfg, "txactivity", "end_silence_dtmf_ms", -1) == 1000,
        "end_silence_dtmf_ms suffix s");
    test_end();

    test_begin("config: [txactivity] dtmf_grace_ms parses");
    kerchunk_config_set(cfg, "txactivity", "dtmf_grace_ms", "3s");
    test_assert(
        kerchunk_config_get_duration_ms(cfg, "txactivity", "dtmf_grace_ms", -1) == 3000,
        "dtmf_grace_ms suffix s");
    test_end();

    test_begin("config: [txactivity] trust_cos_bit reads as int");
    kerchunk_config_set(cfg, "txactivity", "trust_cos_bit", "1");
    {
        const char *v = kerchunk_config_get(cfg, "txactivity", "trust_cos_bit");
        test_assert(v != NULL, "trust_cos_bit present");
        test_assert(atoi(v) == 1, "trust_cos_bit = 1");
    }
    kerchunk_config_set(cfg, "txactivity", "trust_cos_bit", "0");
    {
        const char *v = kerchunk_config_get(cfg, "txactivity", "trust_cos_bit");
        test_assert(atoi(v) == 0, "trust_cos_bit = 0");
    }
    test_end();

    test_begin("config: back-compat [repeater] cor_drop_hold reads through");
    {
        kerchunk_config_t *bc = kerchunk_config_create();
        kerchunk_config_set(bc, "repeater", "cor_drop_hold", "1s");
        /* main.c reads end_silence_ms first then falls back to cor_drop_hold;
         * verify the legacy key still parses to the same numeric value. */
        test_assert(
            kerchunk_config_get_duration_ms(bc, "repeater", "cor_drop_hold", -1) == 1000,
            "cor_drop_hold = 1s -> 1000ms");
        test_assert(
            kerchunk_config_get_duration_ms(bc, "txactivity", "end_silence_ms", -1) == -1,
            "no [txactivity] section -> default sentinel");
        kerchunk_config_destroy(bc);
    }
    test_end();

    test_begin("config: [dtmf] decoder threshold knobs read as int");
    {
        kerchunk_config_t *d = kerchunk_config_create();
        kerchunk_config_set(d, "dtmf", "hits_to_begin",  "1");
        kerchunk_config_set(d, "dtmf", "misses_to_end",  "3");
        kerchunk_config_set(d, "dtmf", "min_off_frames", "1");
        const char *v;
        v = kerchunk_config_get(d, "dtmf", "hits_to_begin");
        test_assert(v != NULL && atoi(v) == 1, "hits_to_begin = 1");
        v = kerchunk_config_get(d, "dtmf", "misses_to_end");
        test_assert(v != NULL && atoi(v) == 3, "misses_to_end = 3");
        v = kerchunk_config_get(d, "dtmf", "min_off_frames");
        test_assert(v != NULL && atoi(v) == 1, "min_off_frames = 1");
        /* Defaults: missing key → NULL, caller picks its own default */
        test_assert(kerchunk_config_get(d, "dtmf", "missing_knob") == NULL,
            "absent knob returns NULL");
        kerchunk_config_destroy(d);
    }
    test_end();

    kerchunk_config_destroy(cfg);
    unlink(path);
}
