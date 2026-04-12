/*
 * kerchunk_module.h — Module interface and loader API
 */

#ifndef KERCHUNK_MODULE_H
#define KERCHUNK_MODULE_H

#include "kerchunk_events.h"

/* Forward declarations */
typedef struct kerchunk_core kerchunk_core_t;
typedef struct kerchunk_config kerchunk_config_t;

/* UI widget types for dashboard auto-generation */
#define CLI_UI_NONE    0   /* don't show in dashboard */
#define CLI_UI_BUTTON  1   /* simple button, fires command directly */
#define CLI_UI_FORM    2   /* button that opens input form with fields */
#define CLI_UI_TOGGLE  3   /* on/off toggle switch */

/* Input field descriptor for CLI_UI_FORM commands */
typedef struct {
    const char *name;       /* field name (used in command string) */
    const char *label;      /* display label */
    const char *type;       /* "text", "number", "select" */
    const char *options;    /* comma-separated for select, NULL otherwise */
    const char *placeholder;/* hint text, NULL for none */
} kerchunk_ui_field_t;

/* CLI command handler — populates a response object (struct kerchunk_resp from kerchunk.h) */
typedef struct {
    const char *name;
    const char *usage;
    const char *description;
    int (*handler)(int argc, const char **argv, struct kerchunk_resp *resp);

    /* UI hints (all optional — NULL/0 = not shown in dashboard) */
    const char *category;                  /* grouping label */
    const char *ui_label;                  /* short button label */
    int         ui_type;                   /* CLI_UI_BUTTON / FORM / TOGGLE */
    const char *ui_command;                /* command string to fire */
    const kerchunk_ui_field_t *ui_fields;  /* input fields for FORM */
    int         num_ui_fields;

    /* Tab completion: comma-separated list of subcommand names. Used by
     * __COMPLETIONS__ so the CLI can offer subcommand suggestions even
     * when no UI form is wired up. NULL = no subcommands. */
    const char *subcommands;
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
    void (*cb)(const kerchunk_cli_cmd_t *cmd, void *ud),
    void *ud);

#endif /* KERCHUNK_MODULE_H */
