/*
 * kerchunk_modules.c — Module loader (dlopen, lifecycle)
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LOG_MOD "modules"

/* Loaded module entry */
typedef struct {
    char                     name[64];
    void                    *handle;       /* dlopen handle */
    const kerchunk_module_def_t *def;        /* Module definition */
    int                      loaded;
} loaded_module_t;

static loaded_module_t g_modules[KERCHUNK_MAX_MODULES];
static int  g_module_count;
static char g_module_path[256] = "./modules";

int kerchunk_modules_init(const char *module_path)
{
    memset(g_modules, 0, sizeof(g_modules));
    g_module_count = 0;
    if (module_path)
        snprintf(g_module_path, sizeof(g_module_path), "%s", module_path);
    return 0;
}

/* Validate module name: alphanumeric, underscore, hyphen only. No path chars. */
static int valid_module_name(const char *name)
{
    if (!name || name[0] == '\0')
        return 0;
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

int kerchunk_module_load(const char *name, kerchunk_core_t *core)
{
    if (!name || !core)
        return -1;

    /* Reject names with path traversal characters */
    if (!valid_module_name(name)) {
        KERCHUNK_LOG_E(LOG_MOD, "invalid module name: %s", name);
        return -1;
    }

    /* Check if already loaded */
    for (int i = 0; i < g_module_count; i++) {
        if (strcmp(g_modules[i].name, name) == 0 && g_modules[i].loaded) {
            KERCHUNK_LOG_W(LOG_MOD, "module %s already loaded", name);
            return -1;
        }
    }

    /* Find a free slot (reuse unloaded slots before appending) */
    int slot = -1;
    for (int i = 0; i < g_module_count; i++) {
        if (!g_modules[i].loaded) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (g_module_count >= KERCHUNK_MAX_MODULES) {
            KERCHUNK_LOG_E(LOG_MOD, "max modules reached (%d)", KERCHUNK_MAX_MODULES);
            return -1;
        }
        slot = g_module_count++;
    }

    /* Build path */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.so", g_module_path, name);

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        KERCHUNK_LOG_E(LOG_MOD, "dlopen %s: %s", path, dlerror());
        return -1;
    }

    /* Find entry point */
    kerchunk_module_init_fn init_fn;
    *(void **)&init_fn = dlsym(handle, "kerchunk_module_init");
    if (!init_fn) {
        KERCHUNK_LOG_E(LOG_MOD, "%s: missing kerchunk_module_init symbol", name);
        dlclose(handle);
        return -1;
    }

    const kerchunk_module_def_t *def = init_fn();
    if (!def || !def->name) {
        KERCHUNK_LOG_E(LOG_MOD, "%s: init returned NULL", name);
        dlclose(handle);
        return -1;
    }

    /* Call load */
    if (def->load) {
        if (def->load(core) != 0) {
            KERCHUNK_LOG_E(LOG_MOD, "%s: load() failed", name);
            dlclose(handle);
            return -1;
        }
    }

    /* Store */
    loaded_module_t *m = &g_modules[slot];
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->handle = handle;
    m->def    = def;
    m->loaded = 1;

    KERCHUNK_LOG_I(LOG_MOD, "loaded module: %s v%s (%s)",
                 def->name, def->version ? def->version : "?",
                 def->description ? def->description : "");
    return 0;
}

int kerchunk_module_unload(const char *name)
{
    if (!name)
        return -1;

    for (int i = 0; i < g_module_count; i++) {
        if (strcmp(g_modules[i].name, name) == 0 && g_modules[i].loaded) {
            if (g_modules[i].def->unload)
                g_modules[i].def->unload();
            g_modules[i].def = NULL;
            dlclose(g_modules[i].handle);
            g_modules[i].handle = NULL;
            g_modules[i].loaded = 0;
            KERCHUNK_LOG_I(LOG_MOD, "unloaded module: %s", name);
            return 0;
        }
    }
    return -1;
}

int kerchunk_module_reload(const char *name, kerchunk_core_t *core)
{
    kerchunk_module_unload(name);
    return kerchunk_module_load(name, core);
}

void kerchunk_modules_shutdown(void)
{
    /* Unload in reverse order */
    for (int i = g_module_count - 1; i >= 0; i--) {
        if (g_modules[i].loaded) {
            if (g_modules[i].def->unload)
                g_modules[i].def->unload();
            dlclose(g_modules[i].handle);
            g_modules[i].loaded = 0;
            KERCHUNK_LOG_I(LOG_MOD, "unloaded module: %s", g_modules[i].name);
        }
    }
    g_module_count = 0;
}

int kerchunk_module_count(void)
{
    int n = 0;
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i].loaded)
            n++;
    }
    return n;
}

const kerchunk_module_def_t *kerchunk_module_get(int index)
{
    int n = 0;
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i].loaded) {
            if (n == index)
                return g_modules[i].def;
            n++;
        }
    }
    return NULL;
}

const kerchunk_module_def_t *kerchunk_module_find(const char *name)
{
    if (!name)
        return NULL;
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i].loaded && strcmp(g_modules[i].name, name) == 0)
            return g_modules[i].def;
    }
    return NULL;
}

int kerchunk_module_dispatch_cli(const char *cmd_name, int argc, const char **argv,
                                struct kerchunk_resp *resp)
{
    if (!cmd_name || !resp)
        return -1;

    for (int i = 0; i < g_module_count; i++) {
        if (!g_modules[i].loaded || !g_modules[i].def->cli_commands)
            continue;

        for (int j = 0; j < g_modules[i].def->num_cli_commands; j++) {
            const kerchunk_cli_cmd_t *cmd = &g_modules[i].def->cli_commands[j];
            if (strcmp(cmd->name, cmd_name) == 0 && cmd->handler)
                return cmd->handler(argc, argv, resp);
        }
    }

    resp_str(resp, "error", "Unknown command");
    return -1;
}

int kerchunk_module_iter_cli_commands(
    void (*cb)(const char *name, const char *usage, const char *desc, void *ud),
    void *ud)
{
    int count = 0;
    for (int i = 0; i < g_module_count; i++) {
        if (!g_modules[i].loaded || !g_modules[i].def->cli_commands)
            continue;
        for (int j = 0; j < g_modules[i].def->num_cli_commands; j++) {
            const kerchunk_cli_cmd_t *cmd = &g_modules[i].def->cli_commands[j];
            if (cb)
                cb(cmd->name, cmd->usage, cmd->description, ud);
            count++;
        }
    }
    return count;
}
