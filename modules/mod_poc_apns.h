/*
 * mod_poc_apns.h — APNs push for iOS PushToTalk wake-up
 *
 * The iOS PT framework only reliably activates the audio session when
 * an APNs push of type "pushtotalk" arrives. mod_poc captures the APNs
 * device token via libpoc's on_push_token callback, then fires a push
 * to every cached token whenever someone starts talking.
 */

#ifndef MOD_POC_APNS_H
#define MOD_POC_APNS_H

#include <stdint.h>

typedef struct apns_ctx apns_ctx_t;

typedef struct {
    int      enabled;
    char     env[16];          /* "sandbox" | "production" */
    char     key_path[256];    /* .p8 file */
    char     key_id[16];       /* APNs key ID */
    char     team_id[16];      /* Apple developer team ID */
    char     topic[128];       /* if empty, derived from bundle_id */
    int      jwt_lifetime_s;   /* default 3000 (50 min) */
    int      timeout_ms;       /* HTTP timeout (default 5000) */
} apns_config_t;

apns_ctx_t *apns_create(const apns_config_t *cfg);
void        apns_destroy(apns_ctx_t *ctx);

void apns_register_token(apns_ctx_t *ctx, uint32_t user_id,
                         const uint8_t *token, int token_len,
                         const char *bundle_id);

/* Returns count of pushes initiated. Each runs on a detached worker
 * thread; failures log only. `talker` is included in the alert text;
 * `exclude_uid` skips the speaker (0 = push everyone). */
int  apns_push_all(apns_ctx_t *ctx, const char *talker,
                   uint32_t group_id, uint32_t exclude_uid);

int  apns_token_count(apns_ctx_t *ctx);
int  apns_pushes_sent(apns_ctx_t *ctx);
int  apns_pushes_failed(apns_ctx_t *ctx);

#endif
