/*
 * test_cwid.c — CW ID tests using libplcode
 */

#include "plcode.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

extern void test_begin(const char *name);
extern void test_end(void);
extern void test_assert(int cond, const char *msg);

void test_cwid(void)
{
    test_begin("plcode_cwid_morse covers A-Z, 0-9, /");
    int ok = 1;
    for (char c = 'A'; c <= 'Z'; c++) {
        if (!plcode_cwid_morse(c)) { ok = 0; break; }
    }
    for (char c = '0'; c <= '9'; c++) {
        if (!plcode_cwid_morse(c)) { ok = 0; break; }
    }
    if (!plcode_cwid_morse('/')) ok = 0;
    test_assert(ok, "missing character");
    test_end();

    test_begin("SOS = ...---...");
    const char *s = plcode_cwid_morse('S');
    const char *o = plcode_cwid_morse('O');
    test_assert(s && strcmp(s, "...") == 0, "S wrong");
    test_assert(o && strcmp(o, "---") == 0, "O wrong");
    test_end();

    test_begin("case insensitive lookup");
    test_assert(plcode_cwid_morse('a') != NULL, "lowercase failed");
    test_assert(strcmp(plcode_cwid_morse('a'), plcode_cwid_morse('A')) == 0, "case mismatch");
    test_end();

    test_begin("invalid char returns NULL");
    test_assert(plcode_cwid_morse('!') == NULL, "! should be NULL");
    test_assert(plcode_cwid_morse(' ') == NULL, "space should be NULL");
    test_end();

    test_begin("cwid encoder create/destroy");
    plcode_cwid_enc_t *enc = NULL;
    int rc = plcode_cwid_enc_create(&enc, 8000, "CQ", 800, 20, 4000);
    test_assert(rc == PLCODE_OK && enc != NULL, "create failed");
    plcode_cwid_enc_destroy(enc);
    test_end();

    test_begin("cwid encoder generates non-silent audio");
    enc = NULL;
    plcode_cwid_enc_create(&enc, 8000, "TEST", 800, 20, 4000);
    int16_t buf[8000];  /* 1 second */
    memset(buf, 0, sizeof(buf));
    plcode_cwid_enc_process(enc, buf, 8000);
    int nonzero = 0;
    for (int i = 0; i < 8000; i++) {
        if (buf[i] != 0) nonzero++;
    }
    test_assert(nonzero > 1000, "mostly silent");
    plcode_cwid_enc_destroy(enc);
    test_end();

    test_begin("cwid encoder completes");
    enc = NULL;
    plcode_cwid_enc_create(&enc, 8000, "E", 800, 20, 4000);  /* Short message */
    int16_t small[16000];
    memset(small, 0, sizeof(small));
    plcode_cwid_enc_process(enc, small, 16000);
    test_assert(plcode_cwid_enc_complete(enc), "should be complete");
    plcode_cwid_enc_destroy(enc);
    test_end();

    test_begin("tone encoder create/destroy");
    plcode_tone_enc_t *tone = NULL;
    rc = plcode_tone_enc_create(&tone, 8000, 800, 4000);
    test_assert(rc == PLCODE_OK && tone != NULL, "create failed");
    plcode_tone_enc_destroy(tone);
    test_end();

    test_begin("tone encoder generates correct-frequency audio");
    tone = NULL;
    plcode_tone_enc_create(&tone, 8000, 1000, 8000);
    int16_t tbuf[800];  /* 100ms */
    memset(tbuf, 0, sizeof(tbuf));
    plcode_tone_enc_process(tone, tbuf, 800);
    /* 1000 Hz at 8000 Hz = 8 samples/cycle. Check zero crossings. */
    int crossings = 0;
    for (int i = 1; i < 800; i++) {
        if ((tbuf[i-1] >= 0 && tbuf[i] < 0) || (tbuf[i-1] < 0 && tbuf[i] >= 0))
            crossings++;
    }
    /* ~200 crossings for 100 cycles (1000 Hz * 0.1s), allow +-5 */
    test_assert(crossings > 190 && crossings < 210, "frequency off");
    plcode_tone_enc_destroy(tone);
    test_end();
}
