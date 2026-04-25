/*
 * config.h — typed reflectd configuration loaded from INI.
 *
 * Wraps the project-wide kerchunk_config (key/value over [section])
 * with a domain-specific roster of nodes and talkgroups. Loaded once
 * at startup; reload on SIGHUP is a phase-2+ feature.
 */

#ifndef KERCHUNK_REFLECTD_CONFIG_H
#define KERCHUNK_REFLECTD_CONFIG_H

#include <stdint.h>
#include "../../include/kerchunk_link_proto.h"

#define RCFG_MAX_NODES        256
#define RCFG_MAX_TGS          64
#define RCFG_MAX_TGS_PER_NODE 32
#define RCFG_MAX_NODE_ID      64
#define RCFG_MAX_TG_NAME      64

typedef struct {
    char     id[RCFG_MAX_NODE_ID];
    uint8_t  psk[KERCHUNK_LINK_PSK_BYTES];
    uint16_t allowed_tgs[RCFG_MAX_TGS_PER_NODE];
    int      n_allowed_tgs;
    uint16_t default_tg;
    int      banned;
} rcfg_node_t;

typedef struct {
    uint16_t number;
    char     name[RCFG_MAX_TG_NAME];
    int      member_node_idxs[RCFG_MAX_NODES];
    int      n_members;
} rcfg_tg_t;

typedef struct {
    /* Listener: ws://host:port or wss://host:port. */
    char     listen_url[128];
    char     tls_cert[256];
    char     tls_key[256];
    char     admin_user[64];
    char     admin_password[128];
    char     log_file[256];
    char     dashboard_dir[256]; /* GET /admin/* served from here */

    int      rtp_port;     /* UDP port for SRTP audio plane */
    char     rtp_advertise_host[64]; /* what we tell clients in login_ok */

    int      keepalive_s;
    int      hangtime_ms;
    char     min_client_version[32]; /* "kerchunk 1.0.0" — lex compare */
    int      mute_threshold_pct;
    int      mute_window_s;
    int      auth_fail_kick;
    int      max_reconnects_per_node_per_min;

    int      recording_enabled;
    char     recording_dir[256];
    int      recording_max_age_days;

    rcfg_node_t nodes[RCFG_MAX_NODES];
    int         n_nodes;
    rcfg_tg_t   tgs[RCFG_MAX_TGS];
    int         n_tgs;
} rcfg_t;

/* Load and validate. Returns 0 on success, -1 on parse / semantic error
 * (errors written to stderr). out is fully populated only on success. */
int rcfg_load(const char *path, rcfg_t *out);

/* Lookups. Return -1 if not found. */
int rcfg_node_idx(const rcfg_t *c, const char *id);
int rcfg_tg_idx(const rcfg_t *c, uint16_t number);

/* Authorization check: is this node permitted on this TG? */
int rcfg_node_allowed_on_tg(const rcfg_t *c, int node_idx, uint16_t tg);

#endif /* KERCHUNK_REFLECTD_CONFIG_H */
