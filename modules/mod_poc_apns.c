/*
 * mod_poc_apns.c — APNs HTTP/2 sender with ES256 JWT auth
 *
 * One worker thread owns a persistent CURL easy handle so successive
 * pushes reuse the same HTTP/2 connection to Apple (Apple actively
 * penalizes connection churn). Producers (the kerchunk timer thread)
 * enqueue jobs and signal the worker.
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
#include <unistd.h>

#define LOG_MOD             "poc_apns"
#define MAX_TOKENS          128
#define APNS_QUEUE_SIZE     64

extern void poc_apns_log(int level, const char *fmt, ...);

typedef struct {
    uint32_t uid;
    uint8_t  token[64];
    int      token_len;
    char     bundle_id[128];
    int64_t  registered_ms;
    int64_t  last_seen_ms;       /* updated on register/disconnect/push */
    int      connected;          /* 1 = TCP up, 0 = dormant (APNs-only) */
} token_entry_t;

typedef struct {
    uint32_t uid;
    uint8_t  token[64];
    int      token_len;
    char     bundle_id[128];
    char     talker[64];
    uint32_t group_id;
    int      retried;        /* JWT-refresh retry budget (0 or 1) */
} push_job_t;

struct apns_ctx {
    apns_config_t   cfg;

    EVP_PKEY       *pkey;

    pthread_mutex_t tokens_mu;
    token_entry_t   tokens[MAX_TOKENS];
    int             token_count;

    pthread_mutex_t jwt_mu;
    char            jwt[2048];
    int64_t         jwt_expires_ms;

    /* Worker thread + queue */
    pthread_t       worker;
    int             worker_started;
    atomic_int      stop;
    pthread_mutex_t q_mu;
    pthread_cond_t  q_cv;
    push_job_t      q[APNS_QUEUE_SIZE];
    int             q_head, q_tail, q_size;

    atomic_int      sent;
    atomic_int      failed;
    atomic_int      dropped;   /* tokens evicted on 400/410 */
};

/* ── helpers ────────────────────────────────────────────────────── */

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

static void hex_encode(const uint8_t *in, int in_len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < in_len; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[in_len * 2] = '\0';
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
    if (!out) { BIO_free_all(b64); return NULL; }
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

/* DER-encoded ECDSA signature → raw r||s (64 bytes for P-256). */
static int ecdsa_der_to_raw(const uint8_t *der, size_t der_len, uint8_t *raw64)
{
    const uint8_t *p = der;
    ECDSA_SIG *sig = d2i_ECDSA_SIG(NULL, &p, (long)der_len);
    if (!sig) return -1;
    const BIGNUM *r, *s;
    ECDSA_SIG_get0(sig, &r, &s);
    memset(raw64, 0, 64);
    int rl = BN_num_bytes(r);
    int sl = BN_num_bytes(s);
    if (rl > 32 || sl > 32) { ECDSA_SIG_free(sig); return -1; }
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
    if (!h64 || !c64) { free(h64); free(c64); return -1; }

    char signing_input[1024];
    int slen = snprintf(signing_input, sizeof(signing_input), "%s.%s", h64, c64);
    free(h64); free(c64);
    if (slen < 0 || (size_t)slen >= sizeof(signing_input)) return -1;

    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (!md) return -1;
    if (EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, ctx->pkey) != 1 ||
        EVP_DigestSignUpdate(md, signing_input, (size_t)slen) != 1) {
        EVP_MD_CTX_free(md); return -1;
    }
    size_t der_len = 0;
    if (EVP_DigestSignFinal(md, NULL, &der_len) != 1) {
        EVP_MD_CTX_free(md); return -1;
    }
    uint8_t der[128];
    if (der_len > sizeof(der) ||
        EVP_DigestSignFinal(md, der, &der_len) != 1) {
        EVP_MD_CTX_free(md); return -1;
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

/* Force a refresh on next ensure_jwt() call (e.g. after 403). */
static void invalidate_jwt(apns_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->jwt_mu);
    ctx->jwt[0] = '\0';
    ctx->jwt_expires_ms = 0;
    pthread_mutex_unlock(&ctx->jwt_mu);
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

    int64_t now = mono_ms();
    pthread_mutex_lock(&ctx->tokens_mu);
    for (int i = 0; i < ctx->token_count; i++) {
        if (ctx->tokens[i].uid == user_id) {
            int was_dormant = !ctx->tokens[i].connected;
            memcpy(ctx->tokens[i].token, token, (size_t)token_len);
            ctx->tokens[i].token_len = token_len;
            snprintf(ctx->tokens[i].bundle_id, sizeof(ctx->tokens[i].bundle_id),
                     "%s", bundle_id ? bundle_id : "");
            ctx->tokens[i].registered_ms = now;
            ctx->tokens[i].last_seen_ms  = now;
            ctx->tokens[i].connected     = 1;
            pthread_mutex_unlock(&ctx->tokens_mu);
            poc_apns_log(KERCHUNK_LOG_INFO,
                         was_dormant
                         ? "uid %u rebound from dormant — token refreshed (bundle=%s, %d B)"
                         : "uid %u token refreshed (bundle=%s, %d B)",
                         user_id, bundle_id ? bundle_id : "?", token_len);
            return;
        }
    }
    if (ctx->token_count >= MAX_TOKENS) {
        pthread_mutex_unlock(&ctx->tokens_mu);
        poc_apns_log(KERCHUNK_LOG_WARN,
                     "token store full, dropping uid %u", user_id);
        return;
    }
    token_entry_t *e = &ctx->tokens[ctx->token_count++];
    e->uid = user_id;
    memcpy(e->token, token, (size_t)token_len);
    e->token_len = token_len;
    snprintf(e->bundle_id, sizeof(e->bundle_id), "%s",
             bundle_id ? bundle_id : "");
    e->registered_ms = now;
    e->last_seen_ms  = now;
    e->connected     = 1;
    pthread_mutex_unlock(&ctx->tokens_mu);
    poc_apns_log(KERCHUNK_LOG_INFO,
                 "uid %u token registered (bundle=%s, %d B)",
                 user_id, bundle_id ? bundle_id : "?", token_len);
}

void apns_mark_dormant(apns_ctx_t *ctx, uint32_t user_id)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->tokens_mu);
    for (int i = 0; i < ctx->token_count; i++) {
        if (ctx->tokens[i].uid != user_id) continue;
        if (!ctx->tokens[i].connected) {
            pthread_mutex_unlock(&ctx->tokens_mu);
            return;  /* already dormant */
        }
        ctx->tokens[i].connected    = 0;
        ctx->tokens[i].last_seen_ms = mono_ms();
        pthread_mutex_unlock(&ctx->tokens_mu);
        poc_apns_log(KERCHUNK_LOG_INFO,
                     "uid %u TCP gone, marking dormant "
                     "(push token retained for APNs wake)",
                     user_id);
        return;
    }
    pthread_mutex_unlock(&ctx->tokens_mu);
    /* No entry — client never registered a token; nothing to do. */
}

/* Drop the cached token for `uid` if its hex form matches `tok_hex`.
 * (We match on the token bytes the worker just used so concurrent
 * re-registration for the same uid doesn't get evicted.) */
static void evict_token_if_match(apns_ctx_t *ctx, uint32_t uid,
                                 const uint8_t *tok, int tok_len)
{
    pthread_mutex_lock(&ctx->tokens_mu);
    for (int i = 0; i < ctx->token_count; i++) {
        if (ctx->tokens[i].uid != uid) continue;
        if (ctx->tokens[i].token_len != tok_len) break;
        if (memcmp(ctx->tokens[i].token, tok, (size_t)tok_len) != 0) break;
        /* swap-with-last */
        ctx->tokens[i] = ctx->tokens[ctx->token_count - 1];
        ctx->token_count--;
        atomic_fetch_add(&ctx->dropped, 1);
        pthread_mutex_unlock(&ctx->tokens_mu);
        poc_apns_log(KERCHUNK_LOG_INFO,
                     "evicted dead token for user %u", uid);
        return;
    }
    pthread_mutex_unlock(&ctx->tokens_mu);
}

/* ── HTTP/2 worker ──────────────────────────────────────────────── */

static size_t curl_discard(void *ptr, size_t s, size_t n, void *ud)
{
    (void)ptr; (void)ud;
    return s * n;
}

static const char *apns_host(apns_ctx_t *ctx)
{
    return (strcmp(ctx->cfg.env, "production") == 0)
        ? "api.push.apple.com" : "api.sandbox.push.apple.com";
}

/* Build the apns-topic for this push. Returns 0 on success, -1 if no
 * topic can be derived (no override, no bundle_id from registration). */
static int build_topic(apns_ctx_t *ctx, const push_job_t *job,
                       char *out, size_t out_max)
{
    if (ctx->cfg.topic[0]) {
        snprintf(out, out_max, "%s", ctx->cfg.topic);
        return 0;
    }
    if (job->bundle_id[0]) {
        snprintf(out, out_max, "%s.voip-ptt", job->bundle_id);
        return 0;
    }
    return -1;
}

/* Single attempt. Returns HTTP status (0 on transport error). On 200
 * increments `sent`; on other statuses the caller decides what to do. */
static long do_push_once(apns_ctx_t *ctx, CURL *curl, const push_job_t *job)
{
    char jwt[2048];
    if (ensure_jwt(ctx, jwt, sizeof(jwt)) != 0) return 0;

    char tok_hex[129];
    hex_encode(job->token, job->token_len, tok_hex);

    char url[256];
    snprintf(url, sizeof(url), "https://%s/3/device/%s",
             apns_host(ctx), tok_hex);

    char topic[160];
    if (build_topic(ctx, job, topic, sizeof(topic)) != 0) {
        poc_apns_log(KERCHUNK_LOG_WARN,
                     "push uid=%u: no topic (bundle empty, no override)",
                     job->uid);
        return 0;
    }

    /* iOS PT framework reads activeSpeakerName from the payload to
     * surface in incomingPushResult; aps must be present (can be {}). */
    char body[384];
    int body_len = snprintf(body, sizeof(body),
        "{\"aps\":{},\"activeSpeakerName\":\"%s\"}",
        job->talker[0] ? job->talker : "Repeater");

    char auth_hdr[2200];
    snprintf(auth_hdr, sizeof(auth_hdr), "authorization: bearer %s", jwt);
    char topic_hdr[200];
    snprintf(topic_hdr, sizeof(topic_hdr), "apns-topic: %s", topic);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_hdr);
    headers = curl_slist_append(headers, topic_hdr);
    headers = curl_slist_append(headers, "apns-push-type: pushtotalk");
    headers = curl_slist_append(headers, "apns-priority: 10");
    headers = curl_slist_append(headers, "apns-expiration: 0");
    headers = curl_slist_append(headers, "content-type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        poc_apns_log(KERCHUNK_LOG_WARN,
                     "push uid=%u transport error: %s",
                     job->uid, curl_easy_strerror(rc));
        return 0;
    }
    return http;
}

/* Drive one job through retry policy:
 *   200          → sent
 *   400 / 410    → drop token, count failed
 *   403          → JWT expired, refresh + retry once
 *   429, 5xx     → backoff + retry once
 *   other        → count failed
 */
static void process_job(apns_ctx_t *ctx, CURL *curl, push_job_t *job)
{
    long http = do_push_once(ctx, curl, job);

    if (http == 200) {
        atomic_fetch_add(&ctx->sent, 1);
        poc_apns_log(KERCHUNK_LOG_DEBUG,
                     "push uid=%u ok (HTTP 200, env=%s)",
                     job->uid, ctx->cfg.env);
        return;
    }

    if ((http == 400 || http == 410) && !job->retried) {
        evict_token_if_match(ctx, job->uid, job->token, job->token_len);
        atomic_fetch_add(&ctx->failed, 1);
        poc_apns_log(KERCHUNK_LOG_INFO,
                     "push uid=%u HTTP %ld — token dropped",
                     job->uid, http);
        return;
    }

    if (http == 403 && !job->retried) {
        poc_apns_log(KERCHUNK_LOG_INFO,
                     "push uid=%u 403 — refreshing JWT and retrying",
                     job->uid);
        invalidate_jwt(ctx);
        job->retried = 1;
        process_job(ctx, curl, job);
        return;
    }

    if ((http == 429 || (http >= 500 && http < 600)) && !job->retried) {
        struct timespec ts = { 0, 250 * 1000 * 1000 };  /* 250 ms */
        nanosleep(&ts, NULL);
        job->retried = 1;
        poc_apns_log(KERCHUNK_LOG_INFO,
                     "push uid=%u HTTP %ld — backoff retry",
                     job->uid, http);
        process_job(ctx, curl, job);
        return;
    }

    atomic_fetch_add(&ctx->failed, 1);
    poc_apns_log(KERCHUNK_LOG_WARN,
                 "push uid=%u failed (HTTP %ld%s)",
                 job->uid, http, job->retried ? ", after retry" : "");
}

static void *worker_main(void *arg)
{
    apns_ctx_t *ctx = (apns_ctx_t *)arg;

    CURL *curl = curl_easy_init();
    if (!curl) {
        poc_apns_log(KERCHUNK_LOG_ERROR, "curl_easy_init failed");
        return NULL;
    }
    /* Persistent options. URL/headers/body are reset per request so
     * libcurl's connection cache reuses the HTTP/2 connection across
     * calls (Apple penalizes connection churn). */
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2_0);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)ctx->cfg.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_discard);

    while (!atomic_load(&ctx->stop)) {
        push_job_t job;
        pthread_mutex_lock(&ctx->q_mu);
        while (ctx->q_size == 0 && !atomic_load(&ctx->stop))
            pthread_cond_wait(&ctx->q_cv, &ctx->q_mu);
        if (atomic_load(&ctx->stop)) {
            pthread_mutex_unlock(&ctx->q_mu);
            break;
        }
        job = ctx->q[ctx->q_tail];
        ctx->q_tail = (ctx->q_tail + 1) % APNS_QUEUE_SIZE;
        ctx->q_size--;
        pthread_mutex_unlock(&ctx->q_mu);

        process_job(ctx, curl, &job);
    }

    curl_easy_cleanup(curl);
    return NULL;
}

/* Producer side. Returns count enqueued (drops silently if queue full). */
int apns_push_all(apns_ctx_t *ctx, const char *talker,
                  uint32_t group_id, uint32_t exclude_uid)
{
    if (!ctx || !ctx->cfg.enabled) return 0;

    int started = 0;
    pthread_mutex_lock(&ctx->tokens_mu);
    for (int i = 0; i < ctx->token_count; i++) {
        if (ctx->tokens[i].uid == exclude_uid) continue;

        push_job_t job = {0};
        job.uid = ctx->tokens[i].uid;
        memcpy(job.token, ctx->tokens[i].token,
               (size_t)ctx->tokens[i].token_len);
        job.token_len = ctx->tokens[i].token_len;
        snprintf(job.bundle_id, sizeof(job.bundle_id), "%s",
                 ctx->tokens[i].bundle_id);
        snprintf(job.talker, sizeof(job.talker), "%s",
                 talker ? talker : "");
        job.group_id = group_id;

        pthread_mutex_lock(&ctx->q_mu);
        if (ctx->q_size >= APNS_QUEUE_SIZE) {
            pthread_mutex_unlock(&ctx->q_mu);
            poc_apns_log(KERCHUNK_LOG_WARN,
                         "queue full, dropping push for uid=%u", job.uid);
            continue;
        }
        ctx->q[ctx->q_head] = job;
        ctx->q_head = (ctx->q_head + 1) % APNS_QUEUE_SIZE;
        ctx->q_size++;
        pthread_cond_signal(&ctx->q_cv);
        pthread_mutex_unlock(&ctx->q_mu);
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
    if (!ctx) { EVP_PKEY_free(pkey); return NULL; }
    ctx->cfg = *cfg;
    if (ctx->cfg.jwt_lifetime_s <= 0) ctx->cfg.jwt_lifetime_s = 3000;
    if (ctx->cfg.timeout_ms <= 0)     ctx->cfg.timeout_ms = 5000;
    ctx->pkey = pkey;
    pthread_mutex_init(&ctx->tokens_mu, NULL);
    pthread_mutex_init(&ctx->jwt_mu, NULL);
    pthread_mutex_init(&ctx->q_mu, NULL);
    pthread_cond_init(&ctx->q_cv, NULL);

    if (pthread_create(&ctx->worker, NULL, worker_main, ctx) != 0) {
        poc_apns_log(KERCHUNK_LOG_ERROR, "worker thread create failed");
        EVP_PKEY_free(pkey);
        pthread_mutex_destroy(&ctx->tokens_mu);
        pthread_mutex_destroy(&ctx->jwt_mu);
        pthread_mutex_destroy(&ctx->q_mu);
        pthread_cond_destroy(&ctx->q_cv);
        free(ctx);
        return NULL;
    }
    ctx->worker_started = 1;

    poc_apns_log(KERCHUNK_LOG_INFO,
                 "APNs ready (env=%s, kid=%s, team=%s, key=%s)",
                 ctx->cfg.env, ctx->cfg.key_id, ctx->cfg.team_id,
                 ctx->cfg.key_path);
    return ctx;
}

void apns_destroy(apns_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->worker_started) {
        atomic_store(&ctx->stop, 1);
        pthread_mutex_lock(&ctx->q_mu);
        pthread_cond_signal(&ctx->q_cv);
        pthread_mutex_unlock(&ctx->q_mu);
        pthread_join(ctx->worker, NULL);
    }
    if (ctx->pkey) EVP_PKEY_free(ctx->pkey);
    pthread_mutex_destroy(&ctx->tokens_mu);
    pthread_mutex_destroy(&ctx->jwt_mu);
    pthread_mutex_destroy(&ctx->q_mu);
    pthread_cond_destroy(&ctx->q_cv);
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

int apns_active_token_count(apns_ctx_t *ctx)
{
    if (!ctx) return 0;
    int n = 0;
    pthread_mutex_lock(&ctx->tokens_mu);
    for (int i = 0; i < ctx->token_count; i++)
        if (ctx->tokens[i].connected) n++;
    pthread_mutex_unlock(&ctx->tokens_mu);
    return n;
}

int apns_dormant_token_count(apns_ctx_t *ctx)
{
    if (!ctx) return 0;
    int n = 0;
    pthread_mutex_lock(&ctx->tokens_mu);
    for (int i = 0; i < ctx->token_count; i++)
        if (!ctx->tokens[i].connected) n++;
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
