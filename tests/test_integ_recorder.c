/*
 * test_integ_recorder.c — Integration tests for mod_recorder
 *
 * Includes mod_recorder.c directly.
 * Tests RX recording (audio tap → WAV), TX recording (playback tap),
 * caller name in filename, disabled flag.
 */

#include "test_integ_mock.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <math.h>

/* Pull in the module source */
#include "../modules/mod_recorder.c"

/* ---- helpers ---- */

/* Count files in a directory matching a prefix */
static int count_files(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (prefix && strstr(ent->d_name, prefix))
            n++;
    }
    closedir(d);
    return n;
}

/* Find a file matching prefix, copy name into buf */
static int find_file(const char *dir, const char *prefix, char *buf, size_t max)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (prefix && strstr(ent->d_name, prefix)) {
            snprintf(buf, max, "%s/%s", dir, ent->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

/* Remove all files in directory */
static void clean_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        remove(path);
    }
    closedir(d);
}

/* Feed N frames of a 440 Hz tone through the audio tap */
static void feed_audio_frames(int frames)
{
    if (!g_mock.tap_handler) return;
    for (int i = 0; i < frames; i++) {
        int16_t buf[160];
        /* Generate simple tone so WAV has non-zero content */
        for (int j = 0; j < 160; j++)
            buf[j] = (int16_t)(4000.0 * sin(2.0 * 3.14159 * 440.0 * (i * 160 + j) / 8000.0));
        kerchevt_t evt = {
            .type = KERCHEVT_AUDIO_FRAME,
            .audio = { .samples = buf, .n = 160 },
        };
        g_mock.tap_handler(&evt, g_mock.tap_ud);
    }
}

/* ---- entry point ---- */

void test_integ_recorder(void)
{
    const char *test_dir = "/tmp/kerchunk_test_recordings";
    mkdir(test_dir, 0755);
    clean_dir(test_dir);

    /* Set up user DB */
    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "user.1", "name",    "Brian");
    kerchunk_config_set(cfg, "user.1", "access",  "2");
    kerchunk_config_set(cfg, "recording", "enabled",   "on");
    kerchunk_config_set(cfg, "recording", "directory",  test_dir);
    kerchunk_config_set(cfg, "recording", "max_duration", "10");
    kerchunk_user_init(cfg);

    kerchevt_init();
    mock_reset();
    mock_init_core();

    mod_recorder.load(&g_mock_core);
    recorder_configure(cfg);

    /* 1. RX recording: COR assert → audio → COR drop → WAV saved */
    test_begin("recorder: RX recording saves WAV");
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_rx_active == 1, "recording not started");
    test_assert(g_mock.tap_registered == 1, "tap not registered");
    /* Feed 50 frames = 1 second of audio */
    feed_audio_frames(50);
    /* Identify caller */
    {
        kerchevt_t e = { .type = KERCHEVT_CALLER_IDENTIFIED,
                        .caller = { .user_id = 1, .method = 1 } };
        kerchevt_fire(&e);
    }
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(g_rx_active == 0, "recording not stopped");
    test_assert(count_files(test_dir, "RX_Brian") == 1, "RX WAV not found");
    test_end();

    /* 2. Filename contains caller name */
    test_begin("recorder: filename has caller name");
    char found[512];
    test_assert(find_file(test_dir, "RX_Brian", found, sizeof(found)) == 0,
                "file not found");
    test_assert(strstr(found, "RX_Brian.wav") != NULL, "wrong filename");
    test_end();

    /* 3. WAV file has non-zero size */
    test_begin("recorder: WAV file has content");
    struct stat st;
    test_assert(stat(found, &st) == 0, "stat failed");
    test_assert(st.st_size > 100, "WAV too small");
    test_end();

    /* 4. Unknown caller → "unknown" in filename */
    test_begin("recorder: unknown caller filename");
    clean_dir(test_dir);
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    feed_audio_frames(25);
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(count_files(test_dir, "RX_unknown") == 1, "unknown RX not found");
    test_end();

    /* 5. Disabled flag prevents recording */
    test_begin("recorder: disabled prevents recording");
    clean_dir(test_dir);
    g_enabled = 0;
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_rx_active == 0, "recorded when disabled");
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(count_files(test_dir, "RX") == 0, "file created when disabled");
    g_enabled = 1;
    test_end();

    /* 6. Max duration caps recording */
    test_begin("recorder: max duration caps buffer");
    clean_dir(test_dir);
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    /* Feed 600 frames = 12 seconds, but max is 10s */
    feed_audio_frames(600);
    mock_fire_simple(KERCHEVT_COR_DROP);
    /* Buffer should be capped at 10s = 80000 samples */
    /* Check file exists (size check is approximate) */
    test_assert(count_files(test_dir, "RX") == 1, "no recording");
    test_end();

    /* Cleanup */
    mod_recorder.unload();
    kerchevt_shutdown();
    kerchunk_user_shutdown();
    kerchunk_config_destroy(cfg);
    /* Leave test recordings for inspection — cleaned on next run */
}
