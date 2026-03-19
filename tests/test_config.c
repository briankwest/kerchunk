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

    kerchunk_config_destroy(cfg);
    unlink(path);
}
