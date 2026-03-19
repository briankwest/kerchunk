/*
 * kerchunk_user.c — User database (loaded from config)
 */

#include "kerchunk_user.h"
#include "kerchunk_config.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

#define LOG_MOD "user"

#define MAX_USER_ID 9999
#define INITIAL_USER_CAP 64

static kerchunk_user_t *g_users;
static int              g_user_count;
static int              g_user_cap;
static kerchunk_group_t g_groups[KERCHUNK_MAX_GROUPS];
static int              g_group_count;

/* Derive username from name: lowercase, spaces→underscores */
static void derive_username(char *out, size_t outsz, const char *name)
{
    size_t j = 0;
    for (size_t i = 0; name[i] && j < outsz - 1; i++) {
        if (name[i] == ' ')
            out[j++] = '_';
        else
            out[j++] = (char)tolower((unsigned char)name[i]);
    }
    out[j] = '\0';
}

int kerchunk_user_init(const kerchunk_config_t *cfg)
{
    g_user_count = 0;
    g_group_count = 0;

    /* Free any previous allocation */
    free(g_users);
    g_users = NULL;
    g_user_cap = 0;

    if (!cfg)
        return 0;

    /* Scan for [group.N] sections */
    for (int id = 1; id <= KERCHUNK_MAX_GROUPS; id++) {
        char section[32];
        snprintf(section, sizeof(section), "group.%d", id);

        const char *name = kerchunk_config_get(cfg, section, "name");
        if (!name)
            continue;

        if (g_group_count >= KERCHUNK_MAX_GROUPS) {
            KERCHUNK_LOG_W(LOG_MOD, "max groups reached (%d)", KERCHUNK_MAX_GROUPS);
            break;
        }

        kerchunk_group_t *g = &g_groups[g_group_count];
        g->id = id;
        snprintf(g->name, sizeof(g->name), "%s", name);
        g->tx_ctcss_freq_x10 = (uint16_t)kerchunk_config_get_int(cfg, section, "tx_ctcss", 0);
        g->tx_dcs_code       = (uint16_t)kerchunk_config_get_int(cfg, section, "tx_dcs", 0);

        KERCHUNK_LOG_I(LOG_MOD, "loaded group %d: %s (tx_ctcss=%u)",
                     g->id, g->name, g->tx_ctcss_freq_x10);
        g_group_count++;
    }

    /* Allocate initial user array */
    g_user_cap = INITIAL_USER_CAP;
    g_users = calloc((size_t)g_user_cap, sizeof(kerchunk_user_t));
    if (!g_users) {
        KERCHUNK_LOG_E(LOG_MOD, "failed to allocate user array");
        return -1;
    }

    /* Scan for [user.N] sections (up to 9999) */
    for (int id = 1; id <= MAX_USER_ID; id++) {
        char section[32];
        snprintf(section, sizeof(section), "user.%d", id);

        const char *name = kerchunk_config_get(cfg, section, "name");
        if (!name)
            continue;

        /* Grow array if needed */
        if (g_user_count >= g_user_cap) {
            int new_cap = g_user_cap * 2;
            kerchunk_user_t *tmp = realloc(g_users,
                (size_t)new_cap * sizeof(kerchunk_user_t));
            if (!tmp) {
                KERCHUNK_LOG_W(LOG_MOD, "realloc failed at %d users", g_user_count);
                break;
            }
            memset(tmp + g_user_cap, 0,
                   (size_t)(new_cap - g_user_cap) * sizeof(kerchunk_user_t));
            g_users = tmp;
            g_user_cap = new_cap;
        }

        kerchunk_user_t *u = &g_users[g_user_count];
        u->id = id;
        snprintf(u->name, sizeof(u->name), "%s", name);

        /* Load username — fall back to derived from name */
        const char *uname = kerchunk_config_get(cfg, section, "username");
        if (uname && uname[0])
            snprintf(u->username, sizeof(u->username), "%s", uname);
        else
            derive_username(u->username, sizeof(u->username), u->name);

        /* Load email */
        const char *email = kerchunk_config_get(cfg, section, "email");
        if (email)
            snprintf(u->email, sizeof(u->email), "%s", email);
        else
            u->email[0] = '\0';

        u->access             = kerchunk_config_get_int(cfg, section, "access", 1);
        u->voicemail          = kerchunk_config_get_int(cfg, section, "voicemail", 0);
        u->group              = kerchunk_config_get_int(cfg, section, "group", 0);

        const char *totp = kerchunk_config_get(cfg, section, "totp_secret");
        if (totp) snprintf(u->totp_secret, sizeof(u->totp_secret), "%s", totp);
        else u->totp_secret[0] = '\0';

        const char *login = kerchunk_config_get(cfg, section, "dtmf_login");
        if (login)
            snprintf(u->dtmf_login, sizeof(u->dtmf_login), "%s", login);
        else
            u->dtmf_login[0] = '\0';

        const char *ani = kerchunk_config_get(cfg, section, "ani");
        if (ani)
            snprintf(u->ani, sizeof(u->ani), "%s", ani);
        else
            u->ani[0] = '\0';

        KERCHUNK_LOG_I(LOG_MOD, "loaded user %d: %s [%s] (group=%d, access=%d)",
                     u->id, u->name, u->username, u->group, u->access);
        g_user_count++;
    }

    KERCHUNK_LOG_I(LOG_MOD, "%d users, %d groups loaded", g_user_count, g_group_count);
    return 0;
}

void kerchunk_user_shutdown(void)
{
    g_user_count = 0;
    g_user_cap = 0;
    free(g_users);
    g_users = NULL;
}

const kerchunk_user_t *kerchunk_user_lookup_by_id(int user_id)
{
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].id == user_id)
            return &g_users[i];
    }
    return NULL;
}

const kerchunk_user_t *kerchunk_user_get(int index)
{
    if (index < 0 || index >= g_user_count) return NULL;
    return &g_users[index];
}

const kerchunk_user_t *kerchunk_user_lookup_by_ani(const char *ani)
{
    if (!ani || ani[0] == '\0')
        return NULL;
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].ani[0] != '\0' && strcmp(g_users[i].ani, ani) == 0)
            return &g_users[i];
    }
    return NULL;
}

const kerchunk_user_t *kerchunk_user_lookup_by_username(const char *username)
{
    if (!username || username[0] == '\0')
        return NULL;
    for (int i = 0; i < g_user_count; i++) {
        if (strcasecmp(g_users[i].username, username) == 0)
            return &g_users[i];
    }
    return NULL;
}

int kerchunk_user_count(void)
{
    return g_user_count;
}

int kerchunk_group_count(void)
{
    return g_group_count;
}

const kerchunk_group_t *kerchunk_group_get(int index)
{
    if (index < 0 || index >= g_group_count) return NULL;
    return &g_groups[index];
}

const kerchunk_group_t *kerchunk_group_lookup_by_id(int group_id)
{
    for (int i = 0; i < g_group_count; i++) {
        if (g_groups[i].id == group_id)
            return &g_groups[i];
    }
    return NULL;
}

int kerchunk_user_lookup_group_tx(int user_id, uint16_t *ctcss_out, uint16_t *dcs_out)
{
    const kerchunk_user_t *u = kerchunk_user_lookup_by_id(user_id);
    if (!u)
        return -1;

    /* TX tones come from group only */
    if (u->group > 0) {
        for (int i = 0; i < g_group_count; i++) {
            if (g_groups[i].id == u->group) {
                if (ctcss_out) *ctcss_out = g_groups[i].tx_ctcss_freq_x10;
                if (dcs_out)   *dcs_out   = g_groups[i].tx_dcs_code;
                return 0;
            }
        }
    }

    /* No TX tone configured */
    if (ctcss_out) *ctcss_out = 0;
    if (dcs_out)   *dcs_out   = 0;
    return 0;
}
