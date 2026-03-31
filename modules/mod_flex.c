/*
 * mod_flex.c — FLEX paging transmitter
 *
 * CLI:
 *   flex send <capcode> [speed] <message>   Send alphanumeric page
 *   flex tone <capcode>                     Send tone-only page
 *   flex numeric <capcode> <digits>         Send numeric page
 *   flex status                             Show module status
 *
 * Config [flex]:
 *   enabled = on
 *   default_speed = 1600
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <libflex/flex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MOD "flex"

static kerchunk_core_t *g_core;
static int g_enabled = 1;
static int g_tx_count = 0;
static int g_deemph = 0;
static flex_speed_t g_default_speed = FLEX_SPEED_1600_2;

static int flex_tx(uint32_t capcode, flex_msg_type_t type,
                   flex_speed_t speed, const char *text)
{
	uint8_t bitbuf[FLEX_BITSTREAM_MAX];
	size_t len = 0, bits = 0;

	flex_err_t err = flex_encode_single(capcode, FLEX_ADDR_SHORT, type,
	                                    speed, text, NULL, 0,
	                                    bitbuf, sizeof(bitbuf),
	                                    &len, &bits);
	if (err != FLEX_OK) {
		g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
		            "encode failed: %s", flex_strerror(err));
		return -1;
	}

	float fbuf[512000];
	size_t ns = 0;
	int flags = g_deemph ? FLEX_BASEBAND_DEEMPH : 0;
	err = flex_baseband_ex(bitbuf, bits, speed,
	                       (float)g_core->sample_rate, flags,
	                       fbuf, sizeof(fbuf) / sizeof(float), &ns);
	if (err != FLEX_OK) {
		g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
		            "baseband failed: %s", flex_strerror(err));
		return -1;
	}

	int16_t *pcm = malloc(ns * sizeof(int16_t));
	if (!pcm) return -1;
	for (size_t i = 0; i < ns; i++)
		pcm[i] = (int16_t)(fbuf[i] * 32767.0f);

	g_core->queue_audio_buffer(pcm, ns, KERCHUNK_PRI_NORMAL);
	free(pcm);

	g_tx_count++;
	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
	            "queued FLEX page: capcode=%u speed=%d type=%d",
	            capcode, flex_speed_bps(speed), (int)type);
	return 0;
}

static flex_speed_t parse_speed(const char *s)
{
	if (!s) return g_default_speed;
	int v = atoi(s);
	switch (v) {
	case 1600: return FLEX_SPEED_1600_2;
	case 3200: return FLEX_SPEED_3200_2;
	default:   return g_default_speed;
	}
}

static int cli_flex(int argc, const char **argv, kerchunk_resp_t *resp)
{
	if (argc < 2 || strcmp(argv[1], "help") == 0) goto usage;

	const char *sub = argv[1];

	if (strcmp(sub, "send") == 0) {
		if (argc < 4) {
			resp_str(resp, "error", "usage: flex send <capcode> [speed] <message>");
			resp_finish(resp);
			return -1;
		}
		uint32_t capcode = (uint32_t)strtoul(argv[2], NULL, 10);
		flex_speed_t speed = g_default_speed;
		int msg_start = 3;

		if (argc >= 5) {
			int maybe = atoi(argv[3]);
			if (maybe == 1600 || maybe == 3200) {
				speed = parse_speed(argv[3]);
				msg_start = 4;
			}
		}

		char msg[512] = {0};
		for (int i = msg_start; i < argc; i++) {
			if (i > msg_start) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
			strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
		}

		int rc = flex_tx(capcode, FLEX_MSG_ALPHA, speed, msg);
		resp_str(resp, "status", rc == 0 ? "queued" : "error");
		if (rc == 0) {
			resp_int(resp, "capcode", (int)capcode);
			resp_int(resp, "speed", flex_speed_bps(speed));
			resp_str(resp, "message", msg);
		}
	} else if (strcmp(sub, "numeric") == 0) {
		if (argc < 4) {
			resp_str(resp, "error", "usage: flex numeric <capcode> <digits>");
			resp_finish(resp);
			return -1;
		}
		uint32_t capcode = (uint32_t)strtoul(argv[2], NULL, 10);
		int rc = flex_tx(capcode, FLEX_MSG_NUMERIC, g_default_speed, argv[3]);
		resp_str(resp, "status", rc == 0 ? "queued" : "error");
	} else if (strcmp(sub, "tone") == 0) {
		if (argc < 3) {
			resp_str(resp, "error", "usage: flex tone <capcode>");
			resp_finish(resp);
			return -1;
		}
		uint32_t capcode = (uint32_t)strtoul(argv[2], NULL, 10);
		int rc = flex_tx(capcode, FLEX_MSG_TONE_ONLY, g_default_speed, NULL);
		resp_str(resp, "status", rc == 0 ? "queued" : "error");
	} else if (strcmp(sub, "status") == 0) {
		resp_str(resp, "module", "mod_flex");
		resp_bool(resp, "enabled", g_enabled);
		resp_bool(resp, "deemphasis", g_deemph);
		resp_int(resp, "tx_count", g_tx_count);
		resp_int(resp, "default_speed", flex_speed_bps(g_default_speed));
	} else {
		goto usage;
	}

	resp_finish(resp);
	return 0;

usage:
	resp_text_raw(resp, "FLEX paging transmitter\n\n"
		"  flex send <capcode> [speed] <message>\n"
		"    Send alphanumeric page.\n"
		"    capcode: pager address (decimal)\n"
		"    speed:   1600 (default), 3200\n"
		"    message: text string\n\n"
		"  flex numeric <capcode> <digits>\n"
		"    Send numeric page.\n\n"
		"  flex tone <capcode>\n"
		"    Send tone-only page.\n\n"
		"  flex status\n"
		"    Show module status, TX count, speed, deemphasis.\n\n"
		"Config: [flex] enabled, default_speed, deemphasis\n");
	resp_str(resp, "error", "usage: flex <send|numeric|tone|status>");
	resp_finish(resp);
	return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
	{ "flex", "flex <send|numeric|tone|status> ...",
	  "FLEX paging transmitter", cli_flex },
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
	v = g_core->config_get("flex", "enabled");
	if (v && (strcmp(v, "off") == 0 || strcmp(v, "0") == 0))
		g_enabled = 0;
	v = g_core->config_get("flex", "default_speed");
	if (v) g_default_speed = parse_speed(v);
	v = g_core->config_get("flex", "deemphasis");
	g_deemph = (v && strcmp(v, "on") == 0);
	return 0;
}

static void mod_unload(void)
{
	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "unloaded (tx_count=%d)", g_tx_count);
}

static const kerchunk_module_def_t mod_def = {
	.name         = "mod_flex",
	.version      = "1.0.0",
	.description  = "FLEX paging transmitter",
	.load         = mod_load,
	.configure    = mod_configure,
	.unload       = mod_unload,
	.cli_commands     = cli_cmds,
	.num_cli_commands = sizeof(cli_cmds) / sizeof(cli_cmds[0]),
};

KERCHUNK_MODULE_DEFINE(mod_def);
