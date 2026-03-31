/*
 * mod_pocsag.c — POCSAG paging transmitter
 *
 * CLI:
 *   pocsag send <addr> <baud> <message>    Send alphanumeric page
 *   pocsag tone <addr> <baud>              Send tone-only page
 *   pocsag numeric <addr> <baud> <digits>  Send numeric page
 *   pocsag status                          Show module status
 *
 * Config [pocsag]:
 *   enabled = on
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <libpocsag/pocsag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MOD "pocsag"

static kerchunk_core_t *g_core;
static int g_enabled = 1;
static int g_tx_count = 0;
static int g_deemph = 0;

/* ── Transmit a POCSAG page ── */

static int pocsag_tx(uint32_t addr, uint32_t baud, pocsag_func_t func,
                     pocsag_msg_type_t type, const char *text)
{
	if (!pocsag_baud_valid(baud)) {
		g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
		            "invalid baud rate %u (512/1200/2400)", baud);
		return -1;
	}

	pocsag_encoder_t enc;
	pocsag_encoder_init(&enc);
	pocsag_encoder_add(&enc, addr, func, type, text);

	uint8_t bitstream[POCSAG_BITSTREAM_MAX];
	size_t bs_len = 0, bs_bits = 0;
	pocsag_err_t err = pocsag_encode(&enc, bitstream, sizeof(bitstream),
	                                 &bs_len, &bs_bits);
	if (err != POCSAG_OK) {
		g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
		            "encode failed: %s", pocsag_strerror(err));
		return -1;
	}

	float fbuf[256000];
	size_t ns = 0;
	int flags = g_deemph ? POCSAG_BASEBAND_DEEMPH : 0;
	err = pocsag_baseband_ex(bitstream, bs_bits, g_core->sample_rate, baud,
	                         flags, fbuf, sizeof(fbuf) / sizeof(float), &ns);
	if (err != POCSAG_OK) {
		g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
		            "baseband failed: %s", pocsag_strerror(err));
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
	            "queued POCSAG page: addr=%u baud=%u type=%d len=%zu bits",
	            addr, baud, (int)type, bs_bits);
	return 0;
}

/* ── CLI handler ── */

static int cli_pocsag(int argc, const char **argv, kerchunk_resp_t *resp)
{
	if (argc < 2 || strcmp(argv[1], "help") == 0) goto usage;

	const char *sub = argv[1];

	if (strcmp(sub, "send") == 0) {
		if (argc < 5) {
			resp_str(resp, "error", "usage: pocsag send <addr> <baud> <message>");
			resp_finish(resp);
			return -1;
		}
		uint32_t addr = (uint32_t)strtoul(argv[2], NULL, 10);
		uint32_t baud = (uint32_t)strtoul(argv[3], NULL, 10);
		char msg[512] = {0};
		for (int i = 4; i < argc; i++) {
			if (i > 4) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
			strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
		}
		int rc = pocsag_tx(addr, baud, POCSAG_FUNC_ALPHA,
		                   POCSAG_MSG_ALPHA, msg);
		resp_str(resp, "status", rc == 0 ? "queued" : "error");
		if (rc == 0) {
			resp_int(resp, "address", (int)addr);
			resp_int(resp, "baud", (int)baud);
			resp_str(resp, "message", msg);
		}
	} else if (strcmp(sub, "numeric") == 0) {
		if (argc < 5) {
			resp_str(resp, "error", "usage: pocsag numeric <addr> <baud> <digits>");
			resp_finish(resp);
			return -1;
		}
		uint32_t addr = (uint32_t)strtoul(argv[2], NULL, 10);
		uint32_t baud = (uint32_t)strtoul(argv[3], NULL, 10);
		int rc = pocsag_tx(addr, baud, POCSAG_FUNC_NUMERIC,
		                   POCSAG_MSG_NUMERIC, argv[4]);
		resp_str(resp, "status", rc == 0 ? "queued" : "error");
	} else if (strcmp(sub, "tone") == 0) {
		if (argc < 4) {
			resp_str(resp, "error", "usage: pocsag tone <addr> <baud>");
			resp_finish(resp);
			return -1;
		}
		uint32_t addr = (uint32_t)strtoul(argv[2], NULL, 10);
		uint32_t baud = (uint32_t)strtoul(argv[3], NULL, 10);
		int rc = pocsag_tx(addr, baud, POCSAG_FUNC_TONE1,
		                   POCSAG_MSG_TONE_ONLY, NULL);
		resp_str(resp, "status", rc == 0 ? "queued" : "error");
	} else if (strcmp(sub, "status") == 0) {
		resp_str(resp, "module", "mod_pocsag");
		resp_bool(resp, "enabled", g_enabled);
		resp_bool(resp, "deemphasis", g_deemph);
		resp_int(resp, "tx_count", g_tx_count);
	} else {
		goto usage;
	}

	resp_finish(resp);
	return 0;

usage:
	resp_text_raw(resp, "POCSAG paging transmitter\n\n"
		"  pocsag send <addr> <baud> <message>\n"
		"    Send alphanumeric page to address at baud rate.\n"
		"    addr:  0-2097151  Pager address (capcode)\n"
		"    baud:  512, 1200, 2400\n"
		"    message: text string\n\n"
		"  pocsag numeric <addr> <baud> <digits>\n"
		"    Send numeric page (digits 0-9, *, #, -, space).\n\n"
		"  pocsag tone <addr> <baud>\n"
		"    Send tone-only page (no message content).\n\n"
		"  pocsag status\n"
		"    Show module status, TX count, deemphasis setting.\n\n"
		"Config: [pocsag] enabled, deemphasis\n");
	resp_str(resp, "error", "usage: pocsag <send|numeric|tone|status>");
	resp_finish(resp);
	return -1;
}

/* ── Module lifecycle ── */

static const kerchunk_ui_field_t pocsag_send_fields[] = {
	{ "addr", "Address", "number", NULL, "1234" },
	{ "baud", "Baud", "select", "512,1200,2400", NULL },
	{ "msg",  "Message", "text", NULL, "Hello from kerchunk" },
};

static const kerchunk_ui_field_t pocsag_numeric_fields[] = {
	{ "addr", "Address", "number", NULL, "1234" },
	{ "baud", "Baud", "select", "512,1200,2400", NULL },
	{ "digits", "Digits", "text", NULL, "5551234" },
};

static const kerchunk_ui_field_t pocsag_tone_fields[] = {
	{ "addr", "Address", "number", NULL, "1234" },
	{ "baud", "Baud", "select", "512,1200,2400", NULL },
};

static const kerchunk_cli_cmd_t cli_cmds[] = {
	{ .name = "pocsag", .usage = "pocsag send <addr> <baud> <message>",
	  .description = "POCSAG alpha page", .handler = cli_pocsag,
	  .category = "Paging", .ui_label = "POCSAG Alpha", .ui_type = CLI_UI_FORM,
	  .ui_command = "pocsag send", .ui_fields = pocsag_send_fields, .num_ui_fields = 3 },
	{ .name = "pocsag", .usage = "pocsag numeric <addr> <baud> <digits>",
	  .description = "POCSAG numeric page", .handler = cli_pocsag,
	  .category = "Paging", .ui_label = "POCSAG Numeric", .ui_type = CLI_UI_FORM,
	  .ui_command = "pocsag numeric", .ui_fields = pocsag_numeric_fields, .num_ui_fields = 3 },
	{ .name = "pocsag", .usage = "pocsag tone <addr> <baud>",
	  .description = "POCSAG tone-only page", .handler = cli_pocsag,
	  .category = "Paging", .ui_label = "POCSAG Tone", .ui_type = CLI_UI_FORM,
	  .ui_command = "pocsag tone", .ui_fields = pocsag_tone_fields, .num_ui_fields = 2 },
};

static int mod_load(kerchunk_core_t *core)
{
	g_core = core;
	return 0;
}

static int mod_configure(const kerchunk_config_t *cfg)
{
	(void)cfg;
	const char *v = g_core->config_get("pocsag", "enabled");
	if (v && (strcmp(v, "off") == 0 || strcmp(v, "0") == 0))
		g_enabled = 0;
	v = g_core->config_get("pocsag", "deemphasis");
	g_deemph = (v && strcmp(v, "on") == 0);
	return 0;
}

static void mod_unload(void)
{
	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "unloaded (tx_count=%d)", g_tx_count);
}

static const kerchunk_module_def_t mod_def = {
	.name         = "mod_pocsag",
	.version      = "1.0.0",
	.description  = "POCSAG paging transmitter",
	.load         = mod_load,
	.configure    = mod_configure,
	.unload       = mod_unload,
	.cli_commands     = cli_cmds,
	.num_cli_commands = sizeof(cli_cmds) / sizeof(cli_cmds[0]),
};

KERCHUNK_MODULE_DEFINE(mod_def);
