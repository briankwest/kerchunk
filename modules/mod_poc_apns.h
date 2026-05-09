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

/* Mark a uid's cached token entry dormant (TCP gone, push token kept).
 * The entry is the wake path on the next PTT — a no-op if no entry
 * exists for this uid (client never registered a token). */
void apns_mark_dormant(apns_ctx_t *ctx, uint32_t user_id);

/* Returns count of pushes enqueued. Each is processed by the persistent
 * worker thread serially through one HTTP/2 connection. `talker` goes
 * in the alert payload; `exclude_uid` skips the speaker (0 = push all). */
int  apns_push_all(apns_ctx_t *ctx, const char *talker,
                   uint32_t group_id, uint32_t exclude_uid);

int  apns_token_count(apns_ctx_t *ctx);          /* total cached */
int  apns_active_token_count(apns_ctx_t *ctx);   /* TCP currently up  */
int  apns_dormant_token_count(apns_ctx_t *ctx);  /* TCP gone, will wake via APNs */
int  apns_pushes_sent(apns_ctx_t *ctx);
int  apns_pushes_failed(apns_ctx_t *ctx);

#endif
