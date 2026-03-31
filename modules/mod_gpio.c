/*
 * mod_gpio.c — GPIO relay control via DTMF commands
 *
 * Listens for GPIO on/off custom events from mod_dtmfcmd.
 * Controls Pi GPIO pins via sysfs for external relays.
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LOG_MOD "gpio"
#define MAX_PINS 8

#define DTMF_EVT_GPIO_ON   (KERCHEVT_CUSTOM + 5)
#define DTMF_EVT_GPIO_OFF  (KERCHEVT_CUSTOM + 6)

static kerchunk_core_t *g_core;
static int g_allowed_pins[MAX_PINS];
static int g_num_pins;
static int g_pin_state[MAX_PINS];

static int is_allowed(int pin)
{
    for (int i = 0; i < g_num_pins; i++) {
        if (g_allowed_pins[i] == pin)
            return 1;
    }
    return 0;
}

static int set_gpio(int pin, int value)
{
#ifdef __linux__
    /* Export pin */
    char path[128];
    FILE *fp;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    fp = fopen(path, "r");
    if (!fp) {
        /* Need to export first */
        fp = fopen("/sys/class/gpio/export", "w");
        if (!fp) return -1;
        fprintf(fp, "%d", pin);
        fclose(fp);

        /* Set direction */
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
        fp = fopen(path, "w");
        if (!fp) return -1;
        fprintf(fp, "out");
        fclose(fp);
    } else {
        fclose(fp);
    }

    /* Set value */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%d", value ? 1 : 0);
    fclose(fp);

    return 0;
#else
    (void)pin; (void)value;
    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "GPIO not available (stub mode)");
    return 0;
#endif
}

static void on_gpio_on(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!evt->custom.data) return;
    int pin = atoi((const char *)evt->custom.data);

    if (!is_allowed(pin)) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "pin %d not allowed", pin);
        g_core->queue_tone(400, 500, 4000, KERCHUNK_PRI_LOW);
        return;
    }

    if (set_gpio(pin, 1) == 0) {
        for (int i = 0; i < g_num_pins; i++) {
            if (g_allowed_pins[i] == pin)
                g_pin_state[i] = 1;
        }
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "GPIO %d ON", pin);
        g_core->queue_tone(800, 200, 4000, KERCHUNK_PRI_LOW);
    }
}

static void on_gpio_off(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!evt->custom.data) return;
    int pin = atoi((const char *)evt->custom.data);

    if (!is_allowed(pin)) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "pin %d not allowed", pin);
        g_core->queue_tone(400, 500, 4000, KERCHUNK_PRI_LOW);
        return;
    }

    if (set_gpio(pin, 0) == 0) {
        for (int i = 0; i < g_num_pins; i++) {
            if (g_allowed_pins[i] == pin)
                g_pin_state[i] = 0;
        }
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "GPIO %d OFF", pin);
        g_core->queue_tone(600, 200, 4000, KERCHUNK_PRI_LOW);
    }
}

static int gpio_load(kerchunk_core_t *core)
{
    g_core = core;
    g_num_pins = 0;
    memset(g_pin_state, 0, sizeof(g_pin_state));

    if (core->dtmf_register) {
        core->dtmf_register("41", 5, "GPIO on",  "gpio_on");
        core->dtmf_register("40", 6, "GPIO off", "gpio_off");
    }

    core->subscribe(DTMF_EVT_GPIO_ON,  on_gpio_on, NULL);
    core->subscribe(DTMF_EVT_GPIO_OFF, on_gpio_off, NULL);
    return 0;
}

static int gpio_configure(const kerchunk_config_t *cfg)
{
    const char *pins = kerchunk_config_get(cfg, "gpio", "allowed_pins");
    if (pins) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s", pins);
        char *tok = strtok(buf, ",");
        g_num_pins = 0;
        while (tok && g_num_pins < MAX_PINS) {
            g_allowed_pins[g_num_pins++] = atoi(tok);
            tok = strtok(NULL, ",");
        }
    }
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "%d GPIO pins configured", g_num_pins);
    return 0;
}

static void gpio_unload(void)
{
    if (g_core->dtmf_unregister) {
        g_core->dtmf_unregister("41");
        g_core->dtmf_unregister("40");
    }
    g_core->unsubscribe(DTMF_EVT_GPIO_ON,  on_gpio_on);
    g_core->unsubscribe(DTMF_EVT_GPIO_OFF, on_gpio_off);
}

/* CLI */
static int cli_gpio(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    resp_int(r, "pin_count", g_num_pins);
    /* JSON: array of pin objects */
    if (!r->jfirst) resp_json_raw(r, ",");
    resp_json_raw(r, "\"pins\":[");
    for (int i = 0; i < g_num_pins; i++) {
        if (i > 0) resp_json_raw(r, ",");
        char frag[64];
        snprintf(frag, sizeof(frag), "{\"pin\":%d,\"state\":\"%s\"}",
                 g_allowed_pins[i], g_pin_state[i] ? "ON" : "OFF");
        resp_json_raw(r, frag);
    }
    resp_json_raw(r, "]");
    r->jfirst = 0;
    /* Text */
    if (g_num_pins == 0) {
        resp_text_raw(r, "(no pins configured)\n");
    } else {
        resp_text_raw(r, "GPIO Pins:\n");
        for (int i = 0; i < g_num_pins; i++) {
            char line[64];
            snprintf(line, sizeof(line), "  Pin %d: %s\n",
                     g_allowed_pins[i], g_pin_state[i] ? "ON" : "OFF");
            resp_text_raw(r, line);
        }
    }
    return 0;

usage:
    resp_text_raw(r, "GPIO relay control via DTMF commands\n\n"
        "  gpio\n"
        "    Show all configured GPIO pins and their current ON/OFF state.\n\n"
        "    Each pin is controlled via DTMF:\n"
        "      *41<pin>#  Turn pin ON\n"
        "      *40<pin>#  Turn pin OFF\n\n"
        "    Only pins listed in the allowed_pins config are controllable.\n"
        "    Uses Linux sysfs GPIO interface (/sys/class/gpio/).\n\n"
        "Config: [gpio] allowed_pins\n"
        "DTMF:   *41<pin># on, *40<pin># off\n");
    resp_str(r, "error", "usage: gpio [help]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "gpio", "gpio", "Show GPIO pin states", cli_gpio },
};

static kerchunk_module_def_t mod_gpio = {
    .name         = "mod_gpio",
    .version      = "1.0.0",
    .description  = "GPIO relay control",
    .load         = gpio_load,
    .configure    = gpio_configure,
    .unload       = gpio_unload,
    .cli_commands = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_gpio);
