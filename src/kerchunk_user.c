/*
 * kerchunk_user.c — User database (loaded from config)
 */

#include "kerchunk_user.h"
#include "kerchunk_config.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MOD "user"

static kerchunk_user_t  g_users[KERCHUNK_MAX_USERS];
static int            g_user_count;
static kerchunk_group_t g_groups[KERCHUNK_MAX_GROUPS];
static int            g_group_count;

int kerchunk_user_init(const kerchunk_config_t *cfg)
{
    g_user_count = 0;
    g_group_count = 0;

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

    /* Scan for [user.N] sections */
    for (int id = 1; id <= KERCHUNK_MAX_USERS; id++) {
        char section[32];
        snprintf(section, sizeof(section), "user.%d", id);

        const char *name = kerchunk_config_get(cfg, section, "name");
        if (!name)
            continue;

        if (g_user_count >= KERCHUNK_MAX_USERS) {
            KERCHUNK_LOG_W(LOG_MOD, "max users reached (%d)", KERCHUNK_MAX_USERS);
            break;
        }

        kerchunk_user_t *u = &g_users[g_user_count];
        u->id = id;
        snprintf(u->name, sizeof(u->name), "%s", name);

        u->access             = kerchunk_config_get_int(cfg, section, "access", 1);
        u->voicemail          = kerchunk_config_get_int(cfg, section, "voicemail", 0);
        u->group              = kerchunk_config_get_int(cfg, section, "group", 0);
        u->tx_ctcss_freq_x10  = (uint16_t)kerchunk_config_get_int(cfg, section, "tx_ctcss", 0);
        u->tx_dcs_code        = (uint16_t)kerchunk_config_get_int(cfg, section, "tx_dcs", 0);

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

        KERCHUNK_LOG_I(LOG_MOD, "loaded user %d: %s (group=%d, access=%d)",
                     u->id, u->name, u->group, u->access);
        g_user_count++;
    }

    KERCHUNK_LOG_I(LOG_MOD, "%d users, %d groups loaded", g_user_count, g_group_count);
    return 0;
}

void kerchunk_user_shutdown(void)
{
    g_user_count = 0;
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

    /* User-level override takes precedence */
    if (u->tx_ctcss_freq_x10 != 0 || u->tx_dcs_code != 0) {
        if (ctcss_out) *ctcss_out = u->tx_ctcss_freq_x10;
        if (dcs_out)   *dcs_out   = u->tx_dcs_code;
        return 0;
    }

    /* Fall back to group */
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
