/*
 * config.c — load reflectd's typed roster from an INI file.
 *
 * Section layout (see PLAN-LINK.md § 5):
 *
 *   [reflector]
 *     listen_url = ws://0.0.0.0:8443
 *     tls_cert   = /etc/.../cert.pem
 *     tls_key    = /etc/.../key.pem
 *     admin_token = <opaque>
 *     keepalive_s = 15
 *     hangtime_ms = 1500
 *
 *   [node.WK7ABC-1]
 *     preshared_key_hex = <64 hex>
 *     allowed_tgs = 4123, 4124
 *     default_tg = 4123
 *
 *   [talkgroup.4123]
 *     name  = "Pacific Northwest"
 *     nodes = WK7ABC-1, WK7DEF-1
 */

#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/kerchunk_config.h"

/* ── small helpers ────────────────────────────────────────────────── */

static int hex_to_bytes(const char *hex, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char hi = hex[2*i], lo = hex[2*i + 1];
        if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo))
            return -1;
        unsigned v;
        if (sscanf(hex + 2*i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* Strip leading/trailing whitespace + matching surrounding quotes from
 * an in-place string. */
static char *trim_unquote(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1])) s[--l] = '\0';
    if (l >= 2 && ((s[0] == '"'  && s[l - 1] == '"') ||
                   (s[0] == '\'' && s[l - 1] == '\''))) {
        s[l - 1] = '\0';
        s++;
    }
    return s;
}

/* Split CSV: yields up to max trimmed tokens via the callback.
 * Modifies src in place. Returns token count. */
static int split_csv(char *src, char **out, int max)
{
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(src, ",", &save); t && n < max;
         t = strtok_r(NULL, ",", &save)) {
        out[n++] = trim_unquote(t);
    }
    return n;
}

/* ── config loaders ──────────────────────────────────────────────── */

static int load_node(rcfg_t *out, const kerchunk_config_t *cfg,
                     const char *section)
{
    if (out->n_nodes >= RCFG_MAX_NODES) {
        fprintf(stderr, "rcfg: too many nodes (max %d)\n", RCFG_MAX_NODES);
        return -1;
    }
    rcfg_node_t *n = &out->nodes[out->n_nodes];

    /* "node.<id>" -> n->id */
    snprintf(n->id, sizeof(n->id), "%s", section + strlen("node."));

    const char *psk_hex = kerchunk_config_get(cfg, section, "preshared_key_hex");
    if (!psk_hex) {
        fprintf(stderr, "rcfg: [%s] missing preshared_key_hex\n", section);
        return -1;
    }
    if (strlen(psk_hex) != 2 * KERCHUNK_LINK_PSK_BYTES ||
        hex_to_bytes(psk_hex, n->psk, KERCHUNK_LINK_PSK_BYTES) != 0) {
        fprintf(stderr, "rcfg: [%s] preshared_key_hex must be %d hex chars\n",
                section, KERCHUNK_LINK_PSK_BYTES * 2);
        return -1;
    }

    const char *allowed = kerchunk_config_get(cfg, section, "allowed_tgs");
    if (allowed) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", allowed);
        char *toks[RCFG_MAX_TGS_PER_NODE];
        int   nt = split_csv(buf, toks, RCFG_MAX_TGS_PER_NODE);
        for (int i = 0; i < nt; i++) {
            int v = atoi(toks[i]);
            if (v <= 0 || v > 65535) {
                fprintf(stderr, "rcfg: [%s] bad TG number '%s'\n",
                        section, toks[i]);
                return -1;
            }
            n->allowed_tgs[n->n_allowed_tgs++] = (uint16_t)v;
        }
    }

    n->default_tg = (uint16_t)kerchunk_config_get_int(
        cfg, section, "default_tg", 0);
    /* If default_tg specified but not in allowed_tgs, add it for sanity. */
    if (n->default_tg) {
        int found = 0;
        for (int i = 0; i < n->n_allowed_tgs; i++)
            if (n->allowed_tgs[i] == n->default_tg) { found = 1; break; }
        if (!found && n->n_allowed_tgs < RCFG_MAX_TGS_PER_NODE)
            n->allowed_tgs[n->n_allowed_tgs++] = n->default_tg;
    }

    n->banned = kerchunk_config_get_int(cfg, section, "banned", 0) ? 1 : 0;

    out->n_nodes++;
    return 0;
}

static int load_tg(rcfg_t *out, const kerchunk_config_t *cfg,
                   const char *section)
{
    if (out->n_tgs >= RCFG_MAX_TGS) {
        fprintf(stderr, "rcfg: too many talkgroups (max %d)\n", RCFG_MAX_TGS);
        return -1;
    }
    rcfg_tg_t *t = &out->tgs[out->n_tgs];

    int v = atoi(section + strlen("talkgroup."));
    if (v <= 0 || v > 65535) {
        fprintf(stderr, "rcfg: [%s] bad TG number\n", section);
        return -1;
    }
    t->number = (uint16_t)v;

    const char *name = kerchunk_config_get(cfg, section, "name");
    if (name) {
        char buf[RCFG_MAX_TG_NAME];
        snprintf(buf, sizeof(buf), "%s", name);
        snprintf(t->name, sizeof(t->name), "%s", trim_unquote(buf));
    }

    const char *members = kerchunk_config_get(cfg, section, "nodes");
    if (members) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", members);
        char *toks[RCFG_MAX_NODES];
        int   nt = split_csv(buf, toks, RCFG_MAX_NODES);
        for (int i = 0; i < nt; i++) {
            int idx = rcfg_node_idx(out, toks[i]);
            if (idx < 0) {
                fprintf(stderr,
                        "rcfg: [%s] member '%s' has no [node.%s] section\n",
                        section, toks[i], toks[i]);
                return -1;
            }
            t->member_node_idxs[t->n_members++] = idx;
        }
    }

    out->n_tgs++;
    return 0;
}

static int load_globals(rcfg_t *out, const kerchunk_config_t *cfg)
{
    const char *url = kerchunk_config_get(cfg, "reflector", "listen_url");
    if (url) {
        snprintf(out->listen_url, sizeof(out->listen_url), "%s", url);
    } else {
        snprintf(out->listen_url, sizeof(out->listen_url),
                 "ws://0.0.0.0:%d", KERCHUNK_LINK_DEFAULT_WS_PORT);
    }

    const char *cert = kerchunk_config_get(cfg, "reflector", "tls_cert");
    const char *key  = kerchunk_config_get(cfg, "reflector", "tls_key");
    if (cert) snprintf(out->tls_cert, sizeof(out->tls_cert), "%s", cert);
    if (key)  snprintf(out->tls_key,  sizeof(out->tls_key),  "%s", key);

    const char *au = kerchunk_config_get(cfg, "reflector", "admin_user");
    const char *ap = kerchunk_config_get(cfg, "reflector", "admin_password");
    if (au) snprintf(out->admin_user,     sizeof(out->admin_user),     "%s", au);
    if (ap) snprintf(out->admin_password, sizeof(out->admin_password), "%s", ap);

    const char *log = kerchunk_config_get(cfg, "reflector", "log_file");
    if (log) snprintf(out->log_file, sizeof(out->log_file), "%s", log);

    const char *dd = kerchunk_config_get(cfg, "reflector", "dashboard_dir");
    snprintf(out->dashboard_dir, sizeof(out->dashboard_dir), "%s",
             dd ? dd : "/usr/share/kerchunk-reflectd/web");

    out->rtp_port           = kerchunk_config_get_int(
        cfg, "reflector", "rtp_port", KERCHUNK_LINK_DEFAULT_RTP_PORT);
    const char *adv = kerchunk_config_get(cfg, "reflector", "rtp_advertise_host");
    snprintf(out->rtp_advertise_host, sizeof(out->rtp_advertise_host),
             "%s", adv ? adv : "127.0.0.1");

    out->keepalive_s        = kerchunk_config_get_int(
        cfg, "reflector", "keepalive_s", KERCHUNK_LINK_DEFAULT_KEEPALIVE_S);

    const char *minver = kerchunk_config_get(cfg, "reflector", "min_client_version");
    snprintf(out->min_client_version, sizeof(out->min_client_version),
             "%s", minver ? minver : "");
    out->hangtime_ms        = kerchunk_config_get_int(
        cfg, "reflector", "hangtime_ms", KERCHUNK_LINK_DEFAULT_HANGTIME_MS);
    out->mute_threshold_pct = kerchunk_config_get_int(
        cfg, "reflector", "mute_threshold_pct", 8);
    out->mute_window_s      = kerchunk_config_get_int(
        cfg, "reflector", "mute_window_s", 30);
    out->auth_fail_kick     = kerchunk_config_get_int(
        cfg, "reflector", "auth_fail_kick", 200);
    out->max_reconnects_per_node_per_min = kerchunk_config_get_int(
        cfg, "reflector", "max_reconnects_per_node_per_min", 6);

    {
        const char *re = kerchunk_config_get(cfg, "reflector", "recording_enabled");
        out->recording_enabled = (re && (!strcmp(re, "on") || !strcmp(re, "true") ||
                                         !strcmp(re, "yes") || !strcmp(re, "1")));
    }
    const char *rd = kerchunk_config_get(cfg, "reflector", "recording_dir");
    snprintf(out->recording_dir, sizeof(out->recording_dir), "%s",
             rd ? rd : "/var/lib/kerchunk-reflectd/recordings");
    out->recording_max_age_days = kerchunk_config_get_int(
        cfg, "reflector", "recording_max_age_days", 30);

    if ((out->tls_cert[0] && !out->tls_key[0]) ||
        (!out->tls_cert[0] && out->tls_key[0])) {
        fprintf(stderr,
                "rcfg: tls_cert and tls_key must both be set or both empty\n");
        return -1;
    }
    if (strncmp(out->listen_url, "wss://", 6) == 0 && !out->tls_cert[0]) {
        fprintf(stderr, "rcfg: listen_url is wss:// but no tls_cert/tls_key\n");
        return -1;
    }
    return 0;
}

int rcfg_load(const char *path, rcfg_t *out)
{
    memset(out, 0, sizeof(*out));

    kerchunk_config_t *cfg = kerchunk_config_load(path);
    if (!cfg) {
        fprintf(stderr, "rcfg: cannot read %s\n", path);
        return -1;
    }

    /* Pass 1: globals + nodes (TGs reference nodes by index) */
    if (load_globals(out, cfg) < 0) goto fail;

    int iter = 0;
    const char *sec;
    while ((sec = kerchunk_config_next_section(cfg, &iter)) != NULL) {
        if (strncmp(sec, "node.", 5) == 0) {
            if (load_node(out, cfg, sec) < 0) goto fail;
        }
    }

    /* Pass 2: talkgroups now that node indices are stable */
    iter = 0;
    while ((sec = kerchunk_config_next_section(cfg, &iter)) != NULL) {
        if (strncmp(sec, "talkgroup.", 10) == 0) {
            if (load_tg(out, cfg, sec) < 0) goto fail;
        }
    }

    if (out->n_nodes == 0) {
        fprintf(stderr, "rcfg: no [node.*] sections defined\n");
        goto fail;
    }
    if (out->n_tgs == 0) {
        fprintf(stderr, "rcfg: no [talkgroup.*] sections defined\n");
        goto fail;
    }

    kerchunk_config_destroy(cfg);
    return 0;

fail:
    kerchunk_config_destroy(cfg);
    return -1;
}

int rcfg_node_idx(const rcfg_t *c, const char *id)
{
    for (int i = 0; i < c->n_nodes; i++)
        if (strcmp(c->nodes[i].id, id) == 0) return i;
    return -1;
}

int rcfg_tg_idx(const rcfg_t *c, uint16_t number)
{
    for (int i = 0; i < c->n_tgs; i++)
        if (c->tgs[i].number == number) return i;
    return -1;
}

int rcfg_node_allowed_on_tg(const rcfg_t *c, int node_idx, uint16_t tg)
{
    if (node_idx < 0 || node_idx >= c->n_nodes) return 0;
    /* Both must agree: node lists tg in allowed_tgs AND tg lists node
     * in members. Either alone is enough to deny. */
    int tg_idx = rcfg_tg_idx(c, tg);
    if (tg_idx < 0) return 0;
    int member_ok = 0;
    for (int i = 0; i < c->tgs[tg_idx].n_members; i++)
        if (c->tgs[tg_idx].member_node_idxs[i] == node_idx) {
            member_ok = 1; break;
        }
    if (!member_ok) return 0;
    int allowed_ok = 0;
    for (int i = 0; i < c->nodes[node_idx].n_allowed_tgs; i++)
        if (c->nodes[node_idx].allowed_tgs[i] == tg) {
            allowed_ok = 1; break;
        }
    return allowed_ok;
}
