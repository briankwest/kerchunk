/*
 * kerchunk_config.c — INI configuration parser
 */

#include "kerchunk_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* Config entry */
typedef struct {
    char section[KERCHUNK_CONFIG_MAX_SECTION];
    char key[KERCHUNK_CONFIG_MAX_KEY];
    char value[KERCHUNK_CONFIG_MAX_VALUE];
} config_entry_t;

/* Config structure */
struct kerchunk_config {
    config_entry_t entries[KERCHUNK_CONFIG_MAX_ENTRIES];
    int            count;
    char           path[256];
};

/* Trim leading/trailing whitespace in place, return pointer into buf */
static char *trim(char *buf)
{
    while (*buf && isspace((unsigned char)*buf))
        buf++;
    if (*buf == '\0')
        return buf;
    char *end = buf + strlen(buf) - 1;
    while (end > buf && isspace((unsigned char)*end))
        *end-- = '\0';
    return buf;
}

/* Parse one file into config */
static int parse_file(kerchunk_config_t *cfg, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char line[2048];
    char current_section[KERCHUNK_CONFIG_MAX_SECTION] = "";

    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);

        /* Skip empty lines and comments */
        if (*p == '\0' || *p == '#' || *p == ';')
            continue;

        /* Section header */
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (!end)
                continue;
            *end = '\0';
            snprintf(current_section, sizeof(current_section), "%s", p + 1);
            continue;
        }

        /* Key = value */
        char *eq = strchr(p, '=');
        if (!eq)
            continue;

        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        /* Strip inline comments (; or #) — but not inside quoted values */
        if (*val != '"' && *val != '\'') {
            char *ic = strpbrk(val, ";#");
            if (ic) {
                *ic = '\0';
                val = trim(val);
            }
        }

        if (cfg->count >= KERCHUNK_CONFIG_MAX_ENTRIES) {
            fclose(fp);
            return -1;
        }

        config_entry_t *e = &cfg->entries[cfg->count];
        snprintf(e->section, sizeof(e->section), "%s", current_section);
        snprintf(e->key, sizeof(e->key), "%s", key);
        snprintf(e->value, sizeof(e->value), "%s", val);
        cfg->count++;
    }

    fclose(fp);
    return 0;
}

kerchunk_config_t *kerchunk_config_load(const char *path)
{
    kerchunk_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg)
        return NULL;

    snprintf(cfg->path, sizeof(cfg->path), "%s", path);

    if (parse_file(cfg, path) != 0) {
        free(cfg);
        return NULL;
    }

    return cfg;
}

kerchunk_config_t *kerchunk_config_create(void)
{
    return calloc(1, sizeof(kerchunk_config_t));
}

int kerchunk_config_reload(kerchunk_config_t *cfg)
{
    if (!cfg || cfg->path[0] == '\0')
        return -1;

    char path[256];
    snprintf(path, sizeof(path), "%s", cfg->path);
    cfg->count = 0;
    return parse_file(cfg, path);
}

void kerchunk_config_destroy(kerchunk_config_t *cfg)
{
    free(cfg);
}

const char *kerchunk_config_get(const kerchunk_config_t *cfg,
                               const char *section, const char *key)
{
    if (!cfg || !section || !key)
        return NULL;

    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].section, section) == 0 &&
            strcmp(cfg->entries[i].key, key) == 0)
            return cfg->entries[i].value;
    }
    return NULL;
}

int kerchunk_config_get_int(const kerchunk_config_t *cfg,
                           const char *section, const char *key, int def)
{
    const char *v = kerchunk_config_get(cfg, section, key);
    if (!v)
        return def;
    return atoi(v);
}

float kerchunk_config_get_float(const kerchunk_config_t *cfg,
                               const char *section, const char *key, float def)
{
    const char *v = kerchunk_config_get(cfg, section, key);
    if (!v)
        return def;
    return (float)atof(v);
}

int kerchunk_config_set(kerchunk_config_t *cfg,
                       const char *section, const char *key, const char *value)
{
    if (!cfg || !section || !key || !value)
        return -1;

    /* Update existing entry */
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].section, section) == 0 &&
            strcmp(cfg->entries[i].key, key) == 0) {
            snprintf(cfg->entries[i].value, sizeof(cfg->entries[i].value),
                     "%s", value);
            return 0;
        }
    }

    /* Add new entry */
    if (cfg->count >= KERCHUNK_CONFIG_MAX_ENTRIES)
        return -1;

    config_entry_t *e = &cfg->entries[cfg->count];
    snprintf(e->section, sizeof(e->section), "%s", section);
    snprintf(e->key, sizeof(e->key), "%s", key);
    snprintf(e->value, sizeof(e->value), "%s", value);
    cfg->count++;
    return 0;
}

const char *kerchunk_config_next_section(const kerchunk_config_t *cfg, int *iter)
{
    if (!cfg || !iter)
        return NULL;

    const char *last = NULL;
    int skip = *iter;
    int found = 0;

    for (int i = 0; i < cfg->count; i++) {
        /* Check if this section was already returned */
        int dup = 0;
        for (int j = 0; j < i; j++) {
            if (strcmp(cfg->entries[j].section, cfg->entries[i].section) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;

        if (found == skip) {
            last = cfg->entries[i].section;
            *iter = skip + 1;
            return last;
        }
        found++;
    }
    return NULL;
}

int kerchunk_config_save(kerchunk_config_t *cfg)
{
    if (!cfg || cfg->path[0] == '\0')
        return -1;

    char tmp[260];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg->path);

    FILE *fp = fopen(tmp, "w");
    if (!fp)
        return -1;

    int iter = 0;
    const char *section;
    int first_section = 1;

    while ((section = kerchunk_config_next_section(cfg, &iter)) != NULL) {
        if (!first_section)
            fprintf(fp, "\n");
        first_section = 0;

        fprintf(fp, "[%s]\n", section);

        for (int i = 0; i < cfg->count; i++) {
            if (strcmp(cfg->entries[i].section, section) == 0)
                fprintf(fp, "%s = %s\n", cfg->entries[i].key,
                        cfg->entries[i].value);
        }
    }

    fclose(fp);
    if (rename(tmp, cfg->path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

const char *kerchunk_config_next_key(const kerchunk_config_t *cfg,
                                     const char *section, int *iter,
                                     const char **value_out)
{
    if (!cfg || !section || !iter)
        return NULL;

    int skip = *iter;
    int found = 0;

    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].section, section) != 0)
            continue;
        if (found == skip) {
            *iter = skip + 1;
            if (value_out)
                *value_out = cfg->entries[i].value;
            return cfg->entries[i].key;
        }
        found++;
    }
    return NULL;
}

int kerchunk_config_remove_section(kerchunk_config_t *cfg, const char *section)
{
    if (!cfg || !section)
        return -1;

    int dst = 0;
    for (int src = 0; src < cfg->count; src++) {
        if (strcmp(cfg->entries[src].section, section) != 0) {
            if (dst != src)
                cfg->entries[dst] = cfg->entries[src];
            dst++;
        }
    }
    cfg->count = dst;
    return 0;
}

