/*
 * mod_aprs.c — APRS beacon and message transmitter
 *
 * CLI:
 *   aprs beacon                             Transmit position beacon
 *   aprs send <callsign> <message>          Send APRS message
 *   aprs status                             Show module status
 *
 * Config [aprs]:
 *   enabled = on
 *   callsign = N0CALL-9
 *   lat = 35.2100
 *   lon = -97.5000
 *   symbol = >
 *   comment = Kerchunk APRS gateway
 *   path = WIDE1-1,WIDE2-1
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <libaprs/aprs.h>
#include <libaprs/ax25.h>
#include <libaprs/modem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MOD "aprs"
#define MAX_PATH_HOPS 8

static kerchunk_core_t *g_core;
static int g_enabled = 1;
static int g_tx_count = 0;

static char g_callsign[16] = "N0CALL-9";
static double g_lat = 35.21;
static double g_lon = -97.50;
static char g_symbol = '>';
static char g_symbol_table = '/';
static char g_comment[128] = "Kerchunk APRS gateway";
static char g_path_str[64] = "WIDE1-1,WIDE2-1";

static const char *g_path[MAX_PATH_HOPS];
static size_t g_path_len = 0;

static void parse_path(void)
{
	static char buf[64];
	strncpy(buf, g_path_str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	g_path_len = 0;
	char *tok = strtok(buf, ",");
	while (tok && g_path_len < MAX_PATH_HOPS) {
		g_path[g_path_len++] = tok;
		tok = strtok(NULL, ",");
	}
}

static int aprs_tx_frame(const uint8_t *frame, size_t frame_len)
{
	afsk_mod_t *mod = afsk_mod_create(g_core->sample_rate);
	if (!mod) {
		g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to create modulator");
		return -1;
	}

	int16_t audio[128000];
	size_t ns = 0;
	aprs_err_t err = afsk_mod_frame(mod, frame, frame_len,
	                                audio, sizeof(audio) / sizeof(int16_t),
	                                &ns);
	afsk_mod_destroy(mod);

	if (err != APRS_OK) {
		g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
		            "AFSK modulate failed: %d", (int)err);
		return -1;
	}

	g_core->queue_audio_buffer(audio, ns, KERCHUNK_PRI_NORMAL);

	g_tx_count++;
	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
	            "queued APRS frame: %zu samples (%.1f ms)",
	            ns, (double)ns / g_core->sample_rate * 1000.0);
	return 0;
}

static int build_and_tx(aprs_packet_t *pkt)
{
	ax25_ui_frame_t ax;
	aprs_err_t err = ax25_from_aprs(pkt, &ax);
	if (err != APRS_OK) return -1;

	uint8_t frame[512];
	size_t frame_len = 0;
	err = ax25_encode_ui_frame(&ax, frame, sizeof(frame), &frame_len);
	if (err != APRS_OK) return -1;

	return aprs_tx_frame(frame, frame_len);
}

static int do_beacon(void)
{
	aprs_packet_t pkt;
	aprs_packet_init(&pkt);
	aprs_err_t err = aprs_build_position(&pkt, g_callsign, "APRS",
	                                     g_path, g_path_len,
	                                     g_lat, g_lon,
	                                     g_symbol_table, g_symbol,
	                                     g_comment);
	if (err != APRS_OK) return -1;
	return build_and_tx(&pkt);
}

static int do_message(const char *to_call, const char *text)
{
	aprs_packet_t pkt;
	aprs_packet_init(&pkt);
	aprs_err_t err = aprs_build_message(&pkt, g_callsign, "APRS",
	                                    g_path, g_path_len,
	                                    to_call, text, NULL);
	if (err != APRS_OK) return -1;
	return build_and_tx(&pkt);
}

/* ── CLI handler ── */

static int cli_aprs(int argc, const char **argv, kerchunk_resp_t *resp)
{
	if (argc < 2) goto usage;

	const char *sub = argv[1];

	if (strcmp(sub, "beacon") == 0) {
		int rc = do_beacon();
		resp_str(resp, "status", rc == 0 ? "queued" : "error");
		if (rc == 0) {
			resp_str(resp, "callsign", g_callsign);
			resp_float(resp, "lat", g_lat);
			resp_float(resp, "lon", g_lon);
		}
	} else if (strcmp(sub, "send") == 0) {
		if (argc < 4) {
			resp_str(resp, "error", "usage: aprs send <callsign> <message>");
			resp_finish(resp);
			return -1;
		}
		const char *to_call = argv[2];
		char msg[256] = {0};
		for (int i = 3; i < argc; i++) {
			if (i > 3) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
			strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
		}
		int rc = do_message(to_call, msg);
		resp_str(resp, "status", rc == 0 ? "queued" : "error");
		if (rc == 0) {
			resp_str(resp, "to", to_call);
			resp_str(resp, "message", msg);
		}
	} else if (strcmp(sub, "status") == 0) {
		resp_str(resp, "module", "mod_aprs");
		resp_bool(resp, "enabled", g_enabled);
		resp_str(resp, "callsign", g_callsign);
		resp_float(resp, "lat", g_lat);
		resp_float(resp, "lon", g_lon);
		resp_int(resp, "tx_count", g_tx_count);
	} else {
		goto usage;
	}

	resp_finish(resp);
	return 0;

usage:
	resp_str(resp, "error", "usage: aprs <beacon|send|status> ...");
	resp_finish(resp);
	return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
	{ "aprs", "aprs <beacon|send|status> ...",
	  "APRS beacon and message transmitter", cli_aprs },
};

static int mod_load(kerchunk_core_t *core)
{
	g_core = core;
	return 0;
}

static int mod_configure(const kerchunk_config_t *cfg)
{
	(void)cfg;
	const char *v;
	v = g_core->config_get("aprs", "enabled");
	if (v && (strcmp(v, "off") == 0 || strcmp(v, "0") == 0))
		g_enabled = 0;
	v = g_core->config_get("aprs", "callsign");
	if (v) strncpy(g_callsign, v, sizeof(g_callsign) - 1);
	v = g_core->config_get("aprs", "lat");
	if (v) g_lat = atof(v);
	v = g_core->config_get("aprs", "lon");
	if (v) g_lon = atof(v);
	v = g_core->config_get("aprs", "symbol");
	if (v && v[0]) g_symbol = v[0];
	v = g_core->config_get("aprs", "comment");
	if (v) strncpy(g_comment, v, sizeof(g_comment) - 1);
	v = g_core->config_get("aprs", "path");
	if (v) strncpy(g_path_str, v, sizeof(g_path_str) - 1);
	parse_path();
	return 0;
}

static void mod_unload(void)
{
	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "unloaded (tx_count=%d)", g_tx_count);
}

static const kerchunk_module_def_t mod_def = {
	.name         = "mod_aprs",
	.version      = "1.0.0",
	.description  = "APRS beacon and message transmitter",
	.load         = mod_load,
	.configure    = mod_configure,
	.unload       = mod_unload,
	.cli_commands     = cli_cmds,
	.num_cli_commands = sizeof(cli_cmds) / sizeof(cli_cmds[0]),
};

KERCHUNK_MODULE_DEFINE(mod_def);
