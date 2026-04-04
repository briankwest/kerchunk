/*
 * kerchunk_config.h — INI configuration parser
 */

#ifndef KERCHUNK_CONFIG_H
#define KERCHUNK_CONFIG_H

#include <stddef.h>

/* Opaque config type */
typedef struct kerchunk_config kerchunk_config_t;

/* Max lengths */
#define KERCHUNK_CONFIG_MAX_SECTION  64
#define KERCHUNK_CONFIG_MAX_KEY      64
#define KERCHUNK_CONFIG_MAX_VALUE    1024
#define KERCHUNK_CONFIG_MAX_ENTRIES  1024

/* Create config from an INI file. Returns NULL on error. */
kerchunk_config_t *kerchunk_config_load(const char *path);

/* Create empty config. */
kerchunk_config_t *kerchunk_config_create(void);

/* Reload config from file (same path used in load). Returns 0 on success. */
int  kerchunk_config_reload(kerchunk_config_t *cfg);

/* Free config. */
void kerchunk_config_destroy(kerchunk_config_t *cfg);

/* Get a value. Returns NULL if not found. */
const char *kerchunk_config_get(const kerchunk_config_t *cfg,
                               const char *section, const char *key);

/* Get an integer value with default. */
int  kerchunk_config_get_int(const kerchunk_config_t *cfg,
                            const char *section, const char *key, int def);

/* Get a float value with default. */
float kerchunk_config_get_float(const kerchunk_config_t *cfg,
                               const char *section, const char *key, float def);

/* Parse a human-readable duration string to milliseconds.
 * Accepts: "10m", "1h30m", "500ms", "2s", or raw digits (ms).
 * Returns parsed ms, or default_ms on parse error. */
int  kerchunk_parse_duration_ms(const char *str, int default_ms);

/* Get a duration value with default (human-readable or raw ms). */
int  kerchunk_config_get_duration_ms(const kerchunk_config_t *cfg,
                                     const char *section, const char *key,
                                     int default_ms);

/* Set a value (copies strings). Returns 0 on success. */
int  kerchunk_config_set(kerchunk_config_t *cfg,
                        const char *section, const char *key, const char *value);

/* Iterate sections. Returns NULL when done. */
const char *kerchunk_config_next_section(const kerchunk_config_t *cfg, int *iter);

/* Save config to its file (atomic write via tmp+rename). Returns 0 on success. */
int kerchunk_config_save(kerchunk_config_t *cfg);

/* Remove all entries in a section. Returns 0 on success. */
int kerchunk_config_remove_section(kerchunk_config_t *cfg, const char *section);

/* Iterate keys within a section. Returns key name, NULL when done.
 * Sets *value_out to the value string. */
const char *kerchunk_config_next_key(const kerchunk_config_t *cfg,
                                     const char *section, int *iter,
                                     const char **value_out);

#endif /* KERCHUNK_CONFIG_H */
