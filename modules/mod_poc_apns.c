/*
 * mod_poc_apns.c — APNs HTTP/2 sender with ES256 JWT auth
 */

#include "mod_poc_apns.h"
#include "kerchunk_log.h"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/bn.h>
#include <openssl/err.h>

#include <curl/curl.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_MOD "poc_apns"
#define MAX_TOKENS         128
#define MAX_INFLIGHT       8

/* Set by mod_poc.c via core->log; we receive it indirectly through
 * a logger pointer. To keep things simple, mod_poc.c installs a
 * trampoline before apns_create(). */
extern void poc_apns_log(int level, const char *fmt, ...);

typedef struct {
    uint32_t uid;
    uint8_t  token[64];
    int      token_len;
    char     bundle_id[128];
    int64_t  registered_ms;
} token_entry_t;

struct apns_ctx {
    apns_config_t   cfg;

    EVP_PKEY       *pkey;       /* loaded from .p8 */

    pthread_mutex_t tokens_mu;
    token_entry_t   tokens[MAX_TOKENS];
    int             token_count;

    pthread_mutex_t jwt_mu;
    char            jwt[2048];
    int64_t         jwt_expires_ms;

    atomic_int      inflight;
    atomic_int      sent;
    atomic_int      failed;
};

/* ── time helpers ───────────────────────────────────────────────── */

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t unix_seconds(void)
{
    return (int64_t)time(NULL);
}

/* ── base64url (no padding) ─────────────────────────────────────── */

static char *base64url_encode(const uint8_t *in, size_t in_len, size_t *out_len)
{
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64, mem);
    BIO_write(b64, in, (int)in_len);
    BIO_flush(b64);

    char *bptr;
    long blen = BIO_get_mem_data(mem, &bptr);

    char *out = malloc((size_t)blen + 1);
    if (!out) {
        BIO_free_all(b64);
        return NULL;
    }
    size_t n = 0;
    for (long i = 0; i < blen; i++) {
        char c = bptr[i];
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
        else if (c == '=') continue;
        out[n++] = c;
    }
    out[n] = '\0';
    if (out_len) *out_len = n;

    BIO_free_all(b64);
    return out;
}

/* Convert a DER-encoded ECDSA signature to raw r||s (64 bytes for P-256). */
static int ecdsa_der_to_raw(const uint8_t *der, size_t der_len,
                            uint8_t *raw64)
{
    const uint8_t *p = der;
    ECDSA_SIG *sig = d2i_ECDSA_SIG(NULL, &p, (long)der_len);
    if (!sig) return -1;

    const BIGNUM *r, *s;
    ECDSA_SIG_get0(sig, &r, &s);

    memset(raw64, 0, 64);
    int rl = BN_num_bytes(r);
    int sl = BN_num_bytes(s);
    if (rl > 32 || sl > 32) {
        ECDSA_SIG_free(sig);
        return -1;
    }
    BN_bn2bin(r, raw64 + 32 - rl);
    BN_bn2bin(s, raw64 + 64 - sl);

    ECDSA_SIG_free(sig);
    return 0;
}

/* ── JWT (ES256) ─────────────────────────────────────────────────── */

static int build_jwt(apns_ctx_t *ctx, char *out, size_t out_max)
{
    char header[256];
    snprintf(header, sizeof(header),
             "{\"alg\":\"ES256\",\"kid\":\"%s\",\"typ\":\"JWT\"}",
             ctx->cfg.key_id);

    char claims[256];
    snprintf(claims, sizeof(claims),
             "{\"iss\":\"%s\",\"iat\":%lld}",
             ctx->cfg.team_id, (long long)unix_seconds());

    size_t hlen, clen;
    char *h64 = base64url_encode((const uint8_t *)header, strlen(header), &hlen);
    char *c64 = base64url_encode((const uint8_t *)claims, strlen(claims), &clen);
    if (!h64 || !c64) {
        free(h64); free(c64);
        return -1;
    }

    char signing_input[1024];
    int slen = snprintf(signing_input, sizeof(signing_input), "%s.%s", h64, c64);
    free(h64); free(c64);
    if (slen < 0 || (size_t)slen >= sizeof(signing_input)) return -1;

    /* Sign with ES256 (SHA256 + ECDSA-P256) */
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (!md) return -1;
    if (EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, ctx->pkey) != 1) {
        EVP_MD_CTX_free(md);
        return -1;
    }
    if (EVP_DigestSignUpdate(md, signing_input, (size_t)slen) != 1) {
        EVP_MD_CTX_free(md);
        return -1;
    }
    size_t der_len = 0;
    if (EVP_DigestSignFinal(md, NULL, &der_len) != 1) {
        EVP_MD_CTX_free(md);
        return -1;
    }
    uint8_t der[128];
    if (der_len > sizeof(der) ||
        EVP_DigestSignFinal(md, der, &der_len) != 1) {
        EVP_MD_CTX_free(md);
        return -1;
    }
    EVP_MD_CTX_free(md);

    uint8_t raw[64];
    if (ecdsa_der_to_raw(der, der_len, raw) != 0) return -1;

    size_t sig_len;
    char *sig64 = base64url_encode(raw, 64, &sig_len);
    if (!sig64) return -1;

    int n = snprintf(out, out_max, "%s.%s", signing_input, sig64);
    free(sig64);
    if (n < 0 || (size_t)n >= out_max) return -1;
    return n;
}

static int ensure_jwt(apns_ctx_t *ctx, char *out, size_t out_max)
{
    pthread_mutex_lock(&ctx->jwt_mu);
    int64_t now = mono_ms();
    if (ctx->jwt[0] && now < ctx->jwt_expires_ms) {
        snprintf(out, out_max, "%s", ctx->jwt);
        pthread_mutex_unlock(&ctx->jwt_mu);
        return 0;
    }

    int n = build_jwt(ctx, ctx->jwt, sizeof(ctx->jwt));
    if (n < 0) {
        ctx->jwt[0] = '\0';
        pthread_mutex_unlock(&ctx->jwt_mu);
        poc_apns_log(KERCHUNK_LOG_ERROR, "jwt build failed");
        return -1;
    }
    ctx->jwt_expires_ms = now + (int64_t)ctx->cfg.jwt_lifetime_s * 1000;
    snprintf(out, out_max, "%s", ctx->jwt);
    pthread_mutex_unlock(&ctx->jwt_mu);
    poc_apns_log(KERCHUNK_LOG_DEBUG, "jwt refreshed (expires in %ds)",
                 ctx->cfg.jwt_lifetime_s);
    return 0;
}

/* ── token store ─────────────────────────────────────────────────── */

void apns_register_token(apns_ctx_t *ctx, uint32_t user_id,
                         const uint8_t *token, int token_len,
                         const char *bundle_id)
{
    if (!ctx || !token || token_len <= 0 || token_len > 64) return;

    pthread_mutex_lock(&ctx->tokens_mu);
    /* Replace if uid already present */
    for (int i = 0; i < ctx->token_count; i++) {
        if (ctx->tokens[i].uid == user_id) {
            memcpy(ctx->tokens[i].token, token, (size_t)token_len);
            ctx->tokens[i].token_len = token_len;
            snprintf(ctx->tokens[i].bundle_id, sizeof(ctx->tokens[i].bundle_id),
                     "%s", bundle_id ? bundle_id : "");
            ctx->tokens[i].registered_ms = mono_ms();
            pthread_mutex_unlock(&ctx->tokens_mu);
            poc_apns_log(KERCHUNK_LOG_INFO,
                         "token refreshed for user %u (bundle=%s, %d B)",
                         user_id, bundle_id ? bundle_id : "?", token_len);
            return;
        }
    }
    if (ctx->token_count >= MAX_TOKENS) {
        pthread_mutex_unlock(&ctx->tokens_mu);
        poc_apns_log(KERCHUNK_LOG_WARN, "token store full, dropping uid %u",
                     user_id);
        return;
    }
    token_entry_t *e = &ctx->tokens[ctx->token_count++];
    e->uid = user_id;
    memcpy(e->token, token, (size_t)token_len);
    e->token_len = token_len;
    snprintf(e->bundle_id, sizeof(e->bundle_id), "%s",
             bundle_id ? bundle_id : "");
    e->registered_ms = mono_ms();
    pthread_mutex_unlock(&ctx->tokens_mu);
    poc_apns_log(KERCHUNK_LOG_INFO,
                 "token registered for user %u (bundle=%s, %d B)",
                 user_id, bundle_id ? bundle_id : "?", token_len);
}

/* ── push worker ─────────────────────────────────────────────────── */

typedef struct {
    apns_ctx_t *ctx;
    uint32_t    uid;
    uint8_t     token[64];
    int         token_len;
    char        bundle_id[128];
    char        talker[64];
    uint32_t    group_id;
} push_job_t;

static size_t curl_discard(void *ptr, size_t s, size_t n, void *ud)
{
    (void)ptr; (void)ud;
    return s * n;
}

static void hex_encode(const uint8_t *in, int in_len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < in_len; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[in_len * 2] = '\0';
}

static void *push_worker(void *arg)
{
    push_job_t *job = (push_job_t *)arg;
    apns_ctx_t *ctx = job->ctx;

    char jwt[2048];
    if (ensure_jwt(ctx, jwt, sizeof(jwt)) != 0) {
        atomic_fetch_add(&ctx->failed, 1);
        atomic_fetch_sub(&ctx->inflight, 1);
        free(job);
        return NULL;
    }

    char tok_hex[129];
    hex_encode(job->token, job->token_len, tok_hex);

    const char *host = (strcmp(ctx->cfg.env, "production") == 0)
        ? "api.push.apple.com" : "api.sandbox.push.apple.com";
    char url[256];
    snprintf(url, sizeof(url), "https://%s/3/device/%s", host, tok_hex);

    char topic[160];
    if (ctx->cfg.topic[0]) {
        snprintf(topic, sizeof(topic), "%s", ctx->cfg.topic);
    } else if (job->bundle_id[0]) {
        snprintf(topic, sizeof(topic), "%s.voip-ptt", job->bundle_id);
    } else {
        poc_apns_log(KERCHUNK_LOG_WARN,
                     "push: no topic for uid %u (bundle empty, no override)",
                     job->uid);
        atomic_fetch_add(&ctx->failed, 1);
        atomic_fetch_sub(&ctx->inflight, 1);
        free(job);
        return NULL;
    }

    char body[256];
    snprintf(body, sizeof(body), "{\"aps\":{\"alert\":\"%s\"}}",
             job->talker[0] ? job->talker : "Incoming PTT");

    CURL *c = curl_easy_init();
    if (!c) {
        atomic_fetch_add(&ctx->failed, 1);
        atomic_fetch_sub(&ctx->inflight, 1);
        free(job);
        return NULL;
    }

    struct curl_slist *headers = NULL;
    char auth_hdr[2200];
    snprintf(auth_hdr, sizeof(auth_hdr), "authorization: bearer %s", jwt);
    char topic_hdr[200];
    snprintf(topic_hdr, sizeof(topic_hdr), "apns-topic: %s", topic);

    headers = curl_slist_append(headers, auth_hdr);
    headers = curl_slist_append(headers, topic_hdr);
    headers = curl_slist_append(headers, "apns-push-type: pushtotalk");
    headers = curl_slist_append(headers, "apns-priority: 10");
    headers = curl_slist_append(headers, "apns-expiration: 0");
    headers = curl_slist_append(headers, "content-type: application/json");

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2_0);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)ctx->cfg.timeout_ms);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_discard);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);

    if (rc != CURLE_OK) {
        atomic_fetch_add(&ctx->failed, 1);
        poc_apns_log(KERCHUNK_LOG_WARN,
                     "push uid=%u failed: %s", job->uid, curl_easy_strerror(rc));
    } else if (http >= 200 && http < 300) {
        atomic_fetch_add(&ctx->sent, 1);
        poc_apns_log(KERCHUNK_LOG_DEBUG,
                     "push uid=%u ok (HTTP %ld, env=%s)",
                     job->uid, http, ctx->cfg.env);
    } else {
        atomic_fetch_add(&ctx->failed, 1);
        poc_apns_log(KERCHUNK_LOG_WARN,
                     "push uid=%u HTTP %ld (topic=%s)",
                     job->uid, http, topic);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    atomic_fetch_sub(&ctx->inflight, 1);
    free(job);
    return NULL;
}

int apns_push_all(apns_ctx_t *ctx, const char *talker,
                  uint32_t group_id, uint32_t exclude_uid)
{
    if (!ctx || !ctx->cfg.enabled) return 0;

    int started = 0;
    pthread_mutex_lock(&ctx->tokens_mu);
    for (int i = 0; i < ctx->token_count; i++) {
        if (ctx->tokens[i].uid == exclude_uid) continue;
        if (atomic_load(&ctx->inflight) >= MAX_INFLIGHT) {
            poc_apns_log(KERCHUNK_LOG_WARN,
                         "push: %d in-flight cap reached, dropping rest",
                         MAX_INFLIGHT);
            break;
        }

        push_job_t *job = calloc(1, sizeof(*job));
        if (!job) break;
        job->ctx = ctx;
        job->uid = ctx->tokens[i].uid;
        memcpy(job->token, ctx->tokens[i].token,
               (size_t)ctx->tokens[i].token_len);
        job->token_len = ctx->tokens[i].token_len;
        snprintf(job->bundle_id, sizeof(job->bundle_id), "%s",
                 ctx->tokens[i].bundle_id);
        snprintf(job->talker, sizeof(job->talker), "%s",
                 talker ? talker : "");
        job->group_id = group_id;

        atomic_fetch_add(&ctx->inflight, 1);
        pthread_t t;
        if (pthread_create(&t, NULL, push_worker, job) != 0) {
            atomic_fetch_sub(&ctx->inflight, 1);
            free(job);
            continue;
        }
        pthread_detach(t);
        started++;
    }
    pthread_mutex_unlock(&ctx->tokens_mu);
    return started;
}

/* ── lifecycle ───────────────────────────────────────────────────── */

apns_ctx_t *apns_create(const apns_config_t *cfg)
{
    if (!cfg || !cfg->enabled) return NULL;
    if (!cfg->key_path[0] || !cfg->key_id[0] || !cfg->team_id[0]) {
        poc_apns_log(KERCHUNK_LOG_ERROR,
                     "missing required config: key_path/key_id/team_id");
        return NULL;
    }

    FILE *fp = fopen(cfg->key_path, "r");
    if (!fp) {
        poc_apns_log(KERCHUNK_LOG_ERROR,
                     "cannot open key file %s", cfg->key_path);
        return NULL;
    }
    EVP_PKEY *pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    fclose(fp);
    if (!pkey) {
        poc_apns_log(KERCHUNK_LOG_ERROR,
                     "PEM_read_PrivateKey failed for %s", cfg->key_path);
        return NULL;
    }

    apns_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return NULL;
    }
    ctx->cfg = *cfg;
    if (ctx->cfg.jwt_lifetime_s <= 0) ctx->cfg.jwt_lifetime_s = 3000;
    if (ctx->cfg.timeout_ms <= 0)     ctx->cfg.timeout_ms = 5000;
    ctx->pkey = pkey;
    pthread_mutex_init(&ctx->tokens_mu, NULL);
    pthread_mutex_init(&ctx->jwt_mu, NULL);

    poc_apns_log(KERCHUNK_LOG_INFO,
                 "APNs ready (env=%s, kid=%s, team=%s, key=%s)",
                 ctx->cfg.env, ctx->cfg.key_id, ctx->cfg.team_id,
                 ctx->cfg.key_path);
    return ctx;
}

void apns_destroy(apns_ctx_t *ctx)
{
    if (!ctx) return;
    /* Wait briefly for in-flight workers to finish (best effort). */
    for (int i = 0; i < 50 && atomic_load(&ctx->inflight) > 0; i++) {
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (ctx->pkey) EVP_PKEY_free(ctx->pkey);
    pthread_mutex_destroy(&ctx->tokens_mu);
    pthread_mutex_destroy(&ctx->jwt_mu);
    free(ctx);
}

int apns_token_count(apns_ctx_t *ctx)
{
    if (!ctx) return 0;
    pthread_mutex_lock(&ctx->tokens_mu);
    int n = ctx->token_count;
    pthread_mutex_unlock(&ctx->tokens_mu);
    return n;
}

int apns_pushes_sent(apns_ctx_t *ctx)
{
    return ctx ? atomic_load(&ctx->sent) : 0;
}

int apns_pushes_failed(apns_ctx_t *ctx)
{
    return ctx ? atomic_load(&ctx->failed) : 0;
}
