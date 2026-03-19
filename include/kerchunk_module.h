/*
 * kerchunk_module.h — Module interface and loader API
 */

#ifndef KERCHUNK_MODULE_H
#define KERCHUNK_MODULE_H

#include "kerchunk_events.h"

/* Forward declarations */
typedef struct kerchunk_core kerchunk_core_t;
typedef struct kerchunk_config kerchunk_config_t;

/* CLI command handler — populates a response object (struct kerchunk_resp from kerchunk.h) */
typedef struct {
    const char *name;
    const char *usage;
    const char *description;
    int (*handler)(int argc, const char **argv, struct kerchunk_resp *resp);
} kerchunk_cli_cmd_t;

/* Module descriptor */
typedef struct {
    const char *name;
    const char *version;
    const char *description;

    /* Lifecycle */
    int  (*load)(kerchunk_core_t *core);
    int  (*configure)(const kerchunk_config_t *cfg);
    void (*unload)(void);

    /* Optional CLI commands */
    const kerchunk_cli_cmd_t *cli_commands;
    int num_cli_commands;
} kerchunk_module_def_t;

/* Module entry point — every .so exports this symbol */
typedef const kerchunk_module_def_t *(*kerchunk_module_init_fn)(void);

#define KERCHUNK_MODULE_DEFINE(mod) \
    const kerchunk_module_def_t *kerchunk_module_init(void) { return &(mod); } \
    extern const kerchunk_module_def_t *kerchunk_module_init(void)

/* Maximum loadable modules */
#define KERCHUNK_MAX_MODULES 32

/* Module loader API */
int  kerchunk_modules_init(const char *module_path);
int  kerchunk_module_load(const char *name, kerchunk_core_t *core);
int  kerchunk_module_unload(const char *name);
int  kerchunk_module_reload(const char *name, kerchunk_core_t *core);
void kerchunk_modules_shutdown(void);
int  kerchunk_module_count(void);

/* Access loaded module info */
const kerchunk_module_def_t *kerchunk_module_get(int index);
const kerchunk_module_def_t *kerchunk_module_find(const char *name);

/* CLI command dispatch across all modules */
int  kerchunk_module_dispatch_cli(const char *cmd_name, int argc, const char **argv,
                                struct kerchunk_resp *resp);

/* Iterate all CLI commands from loaded modules */
int  kerchunk_module_iter_cli_commands(
    void (*cb)(const char *name, const char *usage, const char *desc, void *ud),
    void *ud);

#endif /* KERCHUNK_MODULE_H */
