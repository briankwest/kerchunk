/*
 * test_main.c — Test harness for kerchunkd
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_total;
static int g_passed;
static int g_failed;
static int g_in_test;  /* 1 between test_begin and result */

void test_begin(const char *name)
{
    /* Count UTF-8 continuation bytes — they occupy a byte but no
     * display column, so widen the field to keep PASS aligned. */
    int extra = 0;
    for (const char *p = name; *p; p++)
        if ((*p & 0xC0) == 0x80) extra++;
    printf("  %-*s ", 50 + extra, name);
    g_total++;
    g_in_test = 1;
}

void test_pass(void)
{
    if (!g_in_test) return;  /* Already recorded */
    printf("PASS\n");
    g_passed++;
    g_in_test = 0;
}

void test_fail(const char *msg)
{
    if (!g_in_test) return;
    printf("FAIL: %s\n", msg);
    g_failed++;
    g_in_test = 0;
}

/* Assert within a test — fails the test on first failure,
 * passes only if all asserts in the test succeed and test_pass is called
 * (or if this is the last assert). */
void test_assert(int cond, const char *msg)
{
    if (!cond)
        test_fail(msg);
}

/* Call after all asserts to mark passing if no assert failed */
void test_end(void)
{
    if (g_in_test)
        test_pass();
}

extern void test_events(void);
extern void test_config(void);
extern void test_queue(void);
extern void test_resample(void);
extern void test_multirate(void);
extern void test_repeater(void);
extern void test_cwid(void);
extern void test_resp(void);
extern void test_txactivity(void);
extern void test_integration(void);

int main(void)
{
    printf("kerchunkd test suite\n");
    printf("==================\n\n");

    printf("Event bus tests:\n");
    test_events();

    printf("\nConfig parser tests:\n");
    test_config();

    printf("\nQueue tests:\n");
    test_queue();

    printf("\nResample tests:\n");
    test_resample();

    printf("\nMulti-rate tests:\n");
    test_multirate();

    printf("\nRepeater state machine tests:\n");
    test_repeater();

    printf("\nCW ID tests:\n");
    test_cwid();

    printf("\nResponse system tests:\n");
    test_resp();

    printf("\nTX-activity detector tests:\n");
    test_txactivity();

    printf("\nIntegration tests:\n");
    test_integration();

    printf("\n==================\n");
    printf("Results: %d/%d passed", g_passed, g_total);
    if (g_failed > 0)
        printf(", %d FAILED", g_failed);
    printf("\n");

    return g_failed > 0 ? 1 : 0;
}
