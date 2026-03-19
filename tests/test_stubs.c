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

static int g_otp_elevated[KERCHUNK_MAX_USERS + 1];

void kerchunk_core_set_otp_elevated(int user_id, int elevated)
{
    if (user_id > 0 && user_id <= KERCHUNK_MAX_USERS)
        g_otp_elevated[user_id] = elevated;
}

int kerchunk_core_get_otp_elevated(int user_id)
{
    if (user_id > 0 && user_id <= KERCHUNK_MAX_USERS)
        return g_otp_elevated[user_id];
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

void kerchunk_core_lock_config(void) {}
void kerchunk_core_unlock_config(void) {}
