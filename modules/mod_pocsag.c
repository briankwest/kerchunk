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

	/* baseband at 8 kHz (kerchunk internal rate) */
	float fbuf[256000];
	size_t ns = 0;
	err = pocsag_baseband(bitstream, bs_bits, KERCHUNK_SAMPLE_RATE, baud,
	                      fbuf, sizeof(fbuf) / sizeof(float), &ns);
	if (err != POCSAG_OK) {
		g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
		            "baseband failed: %s", pocsag_strerror(err));
		return -1;
	}

	/* convert float → int16 for the queue */
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

/* ── CLI handlers ── */

static int cmd_send(int argc, const char **argv, kerchunk_resp_t *resp)
{
	if (argc < 4) {
		resp_str(resp, "error", "usage: pocsag send <addr> <baud> <message>");
		resp_finish(resp);
		return -1;
	}
	uint32_t addr = (uint32_t)strtoul(argv[1], NULL, 10);
	uint32_t baud = (uint32_t)strtoul(argv[2], NULL, 10);

	/* join remaining args as message */
	char msg[512] = {0};
	for (int i = 3; i < argc; i++) {
		if (i > 3) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
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
	resp_finish(resp);
	return rc;
}

static int cmd_numeric(int argc, const char **argv, kerchunk_resp_t *resp)
{
	if (argc < 4) {
		resp_str(resp, "error", "usage: pocsag numeric <addr> <baud> <digits>");
		resp_finish(resp);
		return -1;
	}
	uint32_t addr = (uint32_t)strtoul(argv[1], NULL, 10);
	uint32_t baud = (uint32_t)strtoul(argv[2], NULL, 10);

	int rc = pocsag_tx(addr, baud, POCSAG_FUNC_NUMERIC,
	                   POCSAG_MSG_NUMERIC, argv[3]);
	resp_str(resp, "status", rc == 0 ? "queued" : "error");
	resp_finish(resp);
	return rc;
}

static int cmd_tone(int argc, const char **argv, kerchunk_resp_t *resp)
{
	if (argc < 3) {
		resp_str(resp, "error", "usage: pocsag tone <addr> <baud>");
		resp_finish(resp);
		return -1;
	}
	uint32_t addr = (uint32_t)strtoul(argv[1], NULL, 10);
	uint32_t baud = (uint32_t)strtoul(argv[2], NULL, 10);

	int rc = pocsag_tx(addr, baud, POCSAG_FUNC_TONE1,
	                   POCSAG_MSG_TONE_ONLY, NULL);
	resp_str(resp, "status", rc == 0 ? "queued" : "error");
	resp_finish(resp);
	return rc;
}

static int cmd_status(int argc, const char **argv, kerchunk_resp_t *resp)
{
	(void)argc; (void)argv;
	resp_str(resp, "module", "pocsag");
	resp_bool(resp, "enabled", g_enabled);
	resp_int(resp, "tx_count", g_tx_count);
	resp_finish(resp);
	return 0;
}

/* ── Module lifecycle ── */

static const kerchunk_cli_cmd_t cli_cmds[] = {
	{ "pocsag send",    "pocsag send <addr> <baud> <message>",
	  "Send POCSAG alphanumeric page",  cmd_send },
	{ "pocsag numeric", "pocsag numeric <addr> <baud> <digits>",
	  "Send POCSAG numeric page",       cmd_numeric },
	{ "pocsag tone",    "pocsag tone <addr> <baud>",
	  "Send POCSAG tone-only page",     cmd_tone },
	{ "pocsag status",  "pocsag status",
	  "Show POCSAG module status",      cmd_status },
};

static int mod_load(kerchunk_core_t *core)
{
	g_core = core;
	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "loaded");
	return 0;
}

static int mod_configure(const kerchunk_config_t *cfg)
{
	(void)cfg;
	const char *v = g_core->config_get("pocsag", "enabled");
	if (v && (strcmp(v, "off") == 0 || strcmp(v, "0") == 0))
		g_enabled = 0;
	return 0;
}

static void mod_unload(void)
{
	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "unloaded (tx_count=%d)", g_tx_count);
}

static const kerchunk_module_def_t mod_def = {
	.name         = "pocsag",
	.version      = "1.0.0",
	.description  = "POCSAG paging transmitter",
	.load         = mod_load,
	.configure    = mod_configure,
	.unload       = mod_unload,
	.cli_commands     = cli_cmds,
	.num_cli_commands = sizeof(cli_cmds) / sizeof(cli_cmds[0]),
};

KERCHUNK_MODULE_DEFINE(mod_def);
