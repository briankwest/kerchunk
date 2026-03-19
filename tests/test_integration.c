/*
 * test_integration.c — Coordinator for per-module integration tests
 */

extern void test_integ_timer(void);
extern void test_integ_userdb(void);
extern void test_integ_repeater(void);
extern void test_integ_dtmfcmd(void);
extern void test_integ_caller(void);
extern void test_integ_voicemail(void);
extern void test_integ_time(void);
extern void test_integ_decode(void);
extern void test_integ_recorder(void);
extern void test_integ_txcode(void);
extern void test_integ_emergency(void);
extern void test_integ_parrot(void);
extern void test_integ_cdr(void);
extern void test_integ_cwid_module(void);
extern void test_integ_stats(void);
extern void test_integ_otp(void);
extern void test_integ_scrambler(void);

void test_integration(void)
{
    test_integ_timer();
    test_integ_userdb();
    test_integ_repeater();
    test_integ_dtmfcmd();
    test_integ_caller();
    test_integ_voicemail();
    test_integ_time();
    test_integ_decode();
    test_integ_recorder();
    test_integ_txcode();
    test_integ_emergency();
    test_integ_parrot();
    test_integ_cdr();
    test_integ_cwid_module();
    test_integ_stats();
    test_integ_otp();
    test_integ_scrambler();
}
