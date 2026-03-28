/*
 * test_stubs.c — Stub implementations for core functions
 *                used directly by modules (not via vtable).
 */

#include "kerchunk.h"

static int g_emergency_flag;
static void *g_tx_enc;
static int   g_tx_enc_type;

void kerchunk_core_set_emergency(int active)
{
    g_emergency_flag = active;
}

int kerchunk_core_get_emergency(void)
{
    return g_emergency_flag;
}

void kerchunk_core_set_tx_encoder(void *enc, int type)
{
    g_tx_enc      = enc;
    g_tx_enc_type = type;
}

void *kerchunk_core_get_tx_encoder(int *type)
{
    if (type) *type = g_tx_enc_type;
    return g_tx_enc;
}

void kerchunk_socket_broadcast_log(int level, const char *formatted_line)
{
    (void)level;
    (void)formatted_line;
}

#define MAX_OTP_ELEVATED 64
static struct { int user_id; int elevated; } g_otp_sessions[MAX_OTP_ELEVATED];

void kerchunk_core_set_otp_elevated(int user_id, int elevated)
{
    if (user_id <= 0) return;
    for (int i = 0; i < MAX_OTP_ELEVATED; i++) {
        if (g_otp_sessions[i].user_id == user_id) {
            g_otp_sessions[i].elevated = elevated;
            return;
        }
    }
    if (!elevated) return;
    for (int i = 0; i < MAX_OTP_ELEVATED; i++) {
        if (g_otp_sessions[i].user_id == 0) {
            g_otp_sessions[i].user_id = user_id;
            g_otp_sessions[i].elevated = elevated;
            return;
        }
    }
}

int kerchunk_core_get_otp_elevated(int user_id)
{
    if (user_id <= 0) return 0;
    for (int i = 0; i < MAX_OTP_ELEVATED; i++) {
        if (g_otp_sessions[i].user_id == user_id)
            return g_otp_sessions[i].elevated;
    }
    return 0;
}

kerchunk_config_t *kerchunk_core_get_config(void)
{
    return NULL;
}

kerchunk_config_t *kerchunk_core_get_users_config(void)
{
    return NULL;
}

void kerchunk_core_set_users_config(kerchunk_config_t *cfg)
{
    (void)cfg;
}

static kerchunk_scrambler_fn g_rx_scrambler_fn;
static void *g_rx_scrambler_ctx;
static kerchunk_scrambler_fn g_tx_scrambler_fn;
static void *g_tx_scrambler_ctx;

void kerchunk_core_set_rx_scrambler(kerchunk_scrambler_fn fn, void *ctx)
{ g_rx_scrambler_fn = fn; g_rx_scrambler_ctx = ctx; }
kerchunk_scrambler_fn kerchunk_core_get_rx_scrambler(void **ctx)
{ if (ctx) *ctx = g_rx_scrambler_ctx; return g_rx_scrambler_fn; }
void kerchunk_core_set_tx_scrambler(kerchunk_scrambler_fn fn, void *ctx)
{ g_tx_scrambler_fn = fn; g_tx_scrambler_ctx = ctx; }
kerchunk_scrambler_fn kerchunk_core_get_tx_scrambler(void **ctx)
{ if (ctx) *ctx = g_tx_scrambler_ctx; return g_tx_scrambler_fn; }

void kerchunk_core_lock_config(void) {}
void kerchunk_core_unlock_config(void) {}

void kerchunk_console_log_line(const char *line)
{
    (void)line;
}
