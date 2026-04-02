/*
 * mod_txcode.c — TX tone encoder and burst tone CLI
 *
 * Continuous path: sets the active TX CTCSS/DCS encoder based on
 * the identified caller's group config.  The audio thread mixes
 * the encoder output into all outbound audio (when tx_encode=on).
 *
 * Burst CLI commands (queued one-shot):
 *   txcode                         Show active encoder status
 *   txcode dtmf <digits>           Transmit DTMF sequence
 *   txcode twotone <f1> <f2> <ms>  Two-tone sequential page
 *   txcode selcall <digits> [std]  5-tone selective call
 *   txcode burst <freq> <ms>       Single tone burst
 *   txcode mdc <op> <arg> <unit>   MDC-1200 data burst
 *   txcode cwid [callsign]         Manual CW ID
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "plcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MOD "txcode"
#define AMP     16000   /* default burst amplitude */

static kerchunk_core_t *g_core;

/* Config: repeater-wide default TX tone */
static uint16_t g_default_tx_ctcss;
static uint16_t g_default_tx_dcs;
static int16_t  g_ctcss_amplitude = 800;

/* Current continuous encoder */
static void *g_enc;
static int   g_enc_type;

/* ── Continuous CTCSS/DCS encoder ── */

static void destroy_encoder(void)
{
	if (!g_enc) return;
	if (g_enc_type == KERCHUNK_TX_ENC_CTCSS)
		plcode_ctcss_enc_destroy(g_enc);
	else if (g_enc_type == KERCHUNK_TX_ENC_DCS)
		plcode_dcs_enc_destroy(g_enc);
	g_enc      = NULL;
	g_enc_type = KERCHUNK_TX_ENC_NONE;
	kerchunk_core_set_tx_encoder(NULL, KERCHUNK_TX_ENC_NONE);
}

static void create_encoder(uint16_t ctcss_freq_x10, uint16_t dcs_code)
{
	destroy_encoder();
	if (ctcss_freq_x10 > 0) {
		plcode_ctcss_enc_t *enc = NULL;
		if (plcode_ctcss_enc_create(&enc, g_core->sample_rate,
		    ctcss_freq_x10, g_ctcss_amplitude) == PLCODE_OK) {
			g_enc = enc; g_enc_type = KERCHUNK_TX_ENC_CTCSS;
			kerchunk_core_set_tx_encoder(enc, KERCHUNK_TX_ENC_CTCSS);
			g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
			            "TX CTCSS: %.1f Hz", (float)ctcss_freq_x10 / 10.0f);
		}
	} else if (dcs_code > 0) {
		plcode_dcs_enc_t *enc = NULL;
		if (plcode_dcs_enc_create(&enc, g_core->sample_rate,
		    dcs_code, 0, 1600) == PLCODE_OK) {
			g_enc = enc; g_enc_type = KERCHUNK_TX_ENC_DCS;
			kerchunk_core_set_tx_encoder(enc, KERCHUNK_TX_ENC_DCS);
			g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "TX DCS: %u", dcs_code);
		}
	}
}

/* ── Helper: render a burst encoder into a buffer and queue it ── */

static int queue_burst(int16_t *buf, size_t n)
{
	g_core->queue_audio_buffer(buf, n, KERCHUNK_PRI_NORMAL, 0);
	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
	            "queued burst: %zu samples (%.1f ms)",
	            n, (double)n / g_core->sample_rate * 1000.0);
	return 0;
}

/* ── Event handlers ── */

static void on_caller_identified(const kerchevt_t *evt, void *ud)
{
	(void)ud;
	uint16_t ctcss = 0, dcs = 0;
	if (kerchunk_user_lookup_group_tx(evt->caller.user_id, &ctcss, &dcs) == 0 &&
	    (ctcss > 0 || dcs > 0)) {
		create_encoder(ctcss, dcs);
	} else if (g_default_tx_ctcss > 0 || g_default_tx_dcs > 0) {
		create_encoder(g_default_tx_ctcss, g_default_tx_dcs);
	} else {
		destroy_encoder();
	}
}

static void on_caller_cleared(const kerchevt_t *evt, void *ud)
{
	(void)evt; (void)ud;
	if (g_default_tx_ctcss > 0 || g_default_tx_dcs > 0)
		create_encoder(g_default_tx_ctcss, g_default_tx_dcs);
	else
		destroy_encoder();
}

/* ── CLI ── */

static int cli_txcode(int argc, const char **argv, kerchunk_resp_t *r)
{
	if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

	int rate = g_core->sample_rate;

	/* No subcommand: show status */
	if (argc < 2) {
		const char *type_str = "none";
		if (g_enc_type == KERCHUNK_TX_ENC_CTCSS) type_str = "CTCSS";
		else if (g_enc_type == KERCHUNK_TX_ENC_DCS) type_str = "DCS";
		resp_str(r, "tx_encoder", type_str);
		resp_int(r, "default_ctcss", g_default_tx_ctcss);
		resp_int(r, "default_dcs", g_default_tx_dcs);
		resp_int(r, "ctcss_amplitude", g_ctcss_amplitude);
		return 0;
	}

	const char *sub = argv[1];

	/* ── txcode dtmf <digits> ── */
	if (strcmp(sub, "dtmf") == 0) {
		if (argc < 3) {
			resp_str(r, "error", "usage: txcode dtmf <digits>");
			return -1;
		}
		const char *digits = argv[2];
		int tone_ms = 100, gap_ms = 50;
		size_t ndig = strlen(digits);
		size_t per_digit = (size_t)(rate * (tone_ms + gap_ms) / 1000);
		size_t total = per_digit * ndig;
		int16_t *buf = calloc(total, sizeof(int16_t));
		if (!buf) { resp_str(r, "error", "out of memory"); return -1; }

		for (size_t d = 0; d < ndig; d++) {
			plcode_dtmf_enc_t *enc = NULL;
			if (plcode_dtmf_enc_create(&enc, rate, digits[d], AMP) == PLCODE_OK) {
				size_t off = d * per_digit;
				size_t tone_n = (size_t)(rate * tone_ms / 1000);
				plcode_dtmf_enc_process(enc, buf + off, tone_n);
				plcode_dtmf_enc_destroy(enc);
			}
		}
		queue_burst(buf, total);
		free(buf);
		resp_bool(r, "ok", 1);
		resp_str(r, "type", "dtmf");
		resp_str(r, "digits", digits);
		return 0;
	}

	/* ── txcode twotone <freq1> <freq2> <duration_ms> ── */
	if (strcmp(sub, "twotone") == 0) {
		if (argc < 5) {
			resp_str(r, "error", "usage: txcode twotone <freq1> <freq2> <duration_ms>");
			return -1;
		}
		int f1 = atoi(argv[2]), f2 = atoi(argv[3]), ms = atoi(argv[4]);
		if (f1 < 100 || f2 < 100 || ms < 10) {
			resp_str(r, "error", "invalid parameters");
			return -1;
		}
		plcode_twotone_enc_t *enc = NULL;
		if (plcode_twotone_enc_create(&enc, rate, (uint16_t)f1, (uint16_t)f2,
		                              ms, ms, AMP) != PLCODE_OK) {
			resp_str(r, "error", "encoder create failed");
			return -1;
		}
		size_t n = (size_t)(rate * ms * 2 / 1000) + (size_t)rate; /* extra buffer */
		int16_t *buf = calloc(n, sizeof(int16_t));
		if (!buf) { plcode_twotone_enc_destroy(enc); resp_str(r, "error", "out of memory"); return -1; }
		size_t pos = 0;
		while (!plcode_twotone_enc_complete(enc) && pos < n) {
			size_t chunk = (n - pos > 960) ? 960 : (n - pos);
			plcode_twotone_enc_process(enc, buf + pos, chunk);
			pos += chunk;
		}
		plcode_twotone_enc_destroy(enc);
		queue_burst(buf, pos);
		free(buf);
		resp_bool(r, "ok", 1);
		resp_str(r, "type", "twotone");
		resp_int(r, "freq1", f1);
		resp_int(r, "freq2", f2);
		return 0;
	}

	/* ── txcode selcall <digits> [zvei1|ccir|eia] ── */
	if (strcmp(sub, "selcall") == 0) {
		if (argc < 3) {
			resp_str(r, "error", "usage: txcode selcall <digits> [zvei1|ccir|eia]");
			return -1;
		}
		const char *addr = argv[2];
		plcode_selcall_std_t std = PLCODE_SELCALL_ZVEI1;
		if (argc >= 4) {
			if (strcmp(argv[3], "ccir") == 0) std = PLCODE_SELCALL_CCIR;
			else if (strcmp(argv[3], "eia") == 0) std = PLCODE_SELCALL_EIA;
		}
		plcode_selcall_enc_t *enc = NULL;
		if (plcode_selcall_enc_create(&enc, rate, std, addr, AMP) != PLCODE_OK) {
			resp_str(r, "error", "encoder create failed (check digits)");
			return -1;
		}
		size_t n = (size_t)(rate * 2); /* 2 seconds max */
		int16_t *buf = calloc(n, sizeof(int16_t));
		if (!buf) { plcode_selcall_enc_destroy(enc); resp_str(r, "error", "out of memory"); return -1; }
		size_t pos = 0;
		while (!plcode_selcall_enc_complete(enc) && pos < n) {
			size_t chunk = (n - pos > 960) ? 960 : (n - pos);
			plcode_selcall_enc_process(enc, buf + pos, chunk);
			pos += chunk;
		}
		plcode_selcall_enc_destroy(enc);
		queue_burst(buf, pos);
		free(buf);
		resp_bool(r, "ok", 1);
		resp_str(r, "type", "selcall");
		resp_str(r, "address", addr);
		return 0;
	}

	/* ── txcode burst <freq> <duration_ms> ── */
	if (strcmp(sub, "burst") == 0) {
		if (argc < 4) {
			resp_str(r, "error", "usage: txcode burst <freq_hz> <duration_ms>");
			return -1;
		}
		int freq = atoi(argv[2]), ms = atoi(argv[3]);
		if (freq < 100 || freq > 10000 || ms < 10 || ms > 30000) {
			resp_str(r, "error", "freq: 100-10000, duration: 10-30000");
			return -1;
		}
		plcode_toneburst_enc_t *enc = NULL;
		if (plcode_toneburst_enc_create(&enc, rate, freq, ms, AMP) != PLCODE_OK) {
			resp_str(r, "error", "encoder create failed");
			return -1;
		}
		size_t n = (size_t)(rate * ms / 1000) + (size_t)(rate / 10);
		int16_t *buf = calloc(n, sizeof(int16_t));
		if (!buf) { plcode_toneburst_enc_destroy(enc); resp_str(r, "error", "out of memory"); return -1; }
		size_t pos = 0;
		while (!plcode_toneburst_enc_complete(enc) && pos < n) {
			size_t chunk = (n - pos > 960) ? 960 : (n - pos);
			plcode_toneburst_enc_process(enc, buf + pos, chunk);
			pos += chunk;
		}
		plcode_toneburst_enc_destroy(enc);
		queue_burst(buf, pos);
		free(buf);
		resp_bool(r, "ok", 1);
		resp_str(r, "type", "burst");
		resp_int(r, "freq", freq);
		resp_int(r, "duration_ms", ms);
		return 0;
	}

	/* ── txcode mdc <op> <arg> <unit_id> ── */
	if (strcmp(sub, "mdc") == 0) {
		if (argc < 5) {
			resp_str(r, "error", "usage: txcode mdc <op> <arg> <unit_id>");
			return -1;
		}
		int op = (int)strtol(argv[2], NULL, 16);
		int arg = (int)strtol(argv[3], NULL, 16);
		int unit = (int)strtol(argv[4], NULL, 16);
		plcode_mdc1200_enc_t *enc = NULL;
		if (plcode_mdc1200_enc_create(&enc, rate, (uint8_t)op, (uint8_t)arg,
		                              (uint16_t)unit, AMP) != PLCODE_OK) {
			resp_str(r, "error", "encoder create failed");
			return -1;
		}
		size_t n = (size_t)(rate); /* ~1 second max for MDC burst */
		int16_t *buf = calloc(n, sizeof(int16_t));
		if (!buf) { plcode_mdc1200_enc_destroy(enc); resp_str(r, "error", "out of memory"); return -1; }
		size_t pos = 0;
		while (!plcode_mdc1200_enc_complete(enc) && pos < n) {
			size_t chunk = (n - pos > 960) ? 960 : (n - pos);
			plcode_mdc1200_enc_process(enc, buf + pos, chunk);
			pos += chunk;
		}
		plcode_mdc1200_enc_destroy(enc);
		queue_burst(buf, pos);
		free(buf);
		resp_bool(r, "ok", 1);
		resp_str(r, "type", "mdc1200");
		return 0;
	}

	/* ── txcode cwid [callsign] ── */
	if (strcmp(sub, "cwid") == 0) {
		const char *call = NULL;
		if (argc >= 3)
			call = argv[2];
		else
			call = g_core->config_get("general", "callsign");
		if (!call || call[0] == '\0') {
			resp_str(r, "error", "no callsign (provide one or set [general] callsign)");
			return -1;
		}
		plcode_cwid_enc_t *enc = NULL;
		if (plcode_cwid_enc_create(&enc, rate, call, 800, 20, AMP) != PLCODE_OK) {
			resp_str(r, "error", "CW encoder create failed");
			return -1;
		}
		size_t n = (size_t)(rate * 30); /* 30 seconds max */
		int16_t *buf = calloc(n, sizeof(int16_t));
		if (!buf) { plcode_cwid_enc_destroy(enc); resp_str(r, "error", "out of memory"); return -1; }
		size_t pos = 0;
		while (!plcode_cwid_enc_complete(enc) && pos < n) {
			size_t chunk = (n - pos > 960) ? 960 : (n - pos);
			plcode_cwid_enc_process(enc, buf + pos, chunk);
			pos += chunk;
		}
		plcode_cwid_enc_destroy(enc);
		queue_burst(buf, pos);
		free(buf);
		resp_bool(r, "ok", 1);
		resp_str(r, "type", "cwid");
		resp_str(r, "callsign", call);
		return 0;
	}

	/* unknown subcommand */
	goto usage;

usage:
	resp_text_raw(r, "TX tone encoder and burst tone toolbox\n\n"
		"  txcode\n"
		"    Show active continuous TX encoder (CTCSS/DCS/none).\n\n"
		"  txcode dtmf <digits>\n"
		"    Transmit DTMF sequence (0-9, A-D, *, #).\n"
		"    100ms tone, 50ms gap per digit.\n\n"
		"  txcode twotone <freq1> <freq2> <duration_ms>\n"
		"    Two-tone sequential page (fire/EMS paging).\n"
		"    freq1/freq2: Hz, duration: ms per tone.\n\n"
		"  txcode selcall <digits> [zvei1|ccir|eia]\n"
		"    5-tone selective call. Default standard: ZVEI-1.\n"
		"    digits: 5-digit address (0-9).\n\n"
		"  txcode burst <freq_hz> <duration_ms>\n"
		"    Single tone burst (e.g. 1750 Hz repeater access).\n"
		"    freq: 100-10000 Hz, duration: 10-30000 ms.\n\n"
		"  txcode mdc <op> <arg> <unit_id>\n"
		"    MDC-1200 data burst. All values in hex.\n"
		"    op: opcode, arg: argument, unit_id: 4-digit hex.\n\n"
		"  txcode cwid [callsign]\n"
		"    Manual CW ID at 800 Hz, 20 WPM.\n"
		"    Uses [general] callsign if omitted.\n\n"
		"Config: [repeater] tx_ctcss, tx_dcs, ctcss_amplitude\n");
	resp_str(r, "error", "usage: txcode <dtmf|twotone|selcall|burst|mdc|cwid>");
	resp_finish(r);
	return -1;
}

/* ── Module lifecycle ── */

static int txcode_load(kerchunk_core_t *core)
{
	g_core = core;
	g_enc      = NULL;
	g_enc_type = KERCHUNK_TX_ENC_NONE;
	core->subscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified, NULL);
	core->subscribe(KERCHEVT_CALLER_CLEARED,    on_caller_cleared, NULL);
	return 0;
}

static int txcode_configure(const kerchunk_config_t *cfg)
{
	g_default_tx_ctcss = (uint16_t)kerchunk_config_get_int(cfg, "repeater", "tx_ctcss", 0);
	g_default_tx_dcs   = (uint16_t)kerchunk_config_get_int(cfg, "repeater", "tx_dcs", 0);
	g_ctcss_amplitude  = (int16_t)kerchunk_config_get_int(cfg, "repeater", "ctcss_amplitude", 800);
	if (g_ctcss_amplitude < 100)   g_ctcss_amplitude = 100;
	if (g_ctcss_amplitude > 4000)  g_ctcss_amplitude = 4000;

	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
	            "default tx_ctcss=%u tx_dcs=%u ctcss_amplitude=%d",
	            g_default_tx_ctcss, g_default_tx_dcs, g_ctcss_amplitude);
	return 0;
}

static void txcode_unload(void)
{
	g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
	g_core->unsubscribe(KERCHEVT_CALLER_CLEARED,    on_caller_cleared);
	destroy_encoder();
}

static const kerchunk_ui_field_t dtmf_fields[] = {
	{ "digits", "Digits", "text", NULL, "5551234" },
};
static const kerchunk_ui_field_t burst_fields[] = {
	{ "freq", "Frequency", "number", NULL, "1750" },
	{ "dur",  "Duration (ms)", "number", NULL, "500" },
};
static const kerchunk_ui_field_t selcall_fields[] = {
	{ "digits", "Address", "text", NULL, "12345" },
	{ "std",    "Standard", "select", "zvei1,ccir,eia", NULL },
};
static const kerchunk_ui_field_t twotone_fields[] = {
	{ "f1",  "Freq 1 (Hz)", "number", NULL, "1000" },
	{ "f2",  "Freq 2 (Hz)", "number", NULL, "1500" },
	{ "dur", "Duration (ms)", "number", NULL, "1000" },
};
static const kerchunk_ui_field_t mdc_fields[] = {
	{ "op",   "Opcode (hex)", "text", NULL, "01" },
	{ "arg",  "Arg (hex)", "text", NULL, "80" },
	{ "unit", "Unit ID (hex)", "text", NULL, "1234" },
};
static const kerchunk_ui_field_t cwid_fields[] = {
	{ "call", "Callsign", "text", NULL, "" },
};

static const kerchunk_cli_cmd_t cli_cmds[] = {
	{ .name = "txcode", .usage = "txcode dtmf <digits>",
	  .description = "DTMF sequence", .handler = cli_txcode,
	  .category = "Tones", .ui_label = "DTMF", .ui_type = CLI_UI_FORM,
	  .ui_command = "txcode dtmf", .ui_fields = dtmf_fields, .num_ui_fields = 1 },
	{ .name = "txcode", .usage = "txcode burst <freq> <dur_ms>",
	  .description = "Tone burst", .handler = cli_txcode,
	  .category = "Tones", .ui_label = "Tone Burst", .ui_type = CLI_UI_FORM,
	  .ui_command = "txcode burst", .ui_fields = burst_fields, .num_ui_fields = 2 },
	{ .name = "txcode", .usage = "txcode twotone <f1> <f2> <dur_ms>",
	  .description = "Two-tone page", .handler = cli_txcode,
	  .category = "Tones", .ui_label = "Two-Tone", .ui_type = CLI_UI_FORM,
	  .ui_command = "txcode twotone", .ui_fields = twotone_fields, .num_ui_fields = 3 },
	{ .name = "txcode", .usage = "txcode selcall <digits> [std]",
	  .description = "5-tone selective call", .handler = cli_txcode,
	  .category = "Tones", .ui_label = "Selcall", .ui_type = CLI_UI_FORM,
	  .ui_command = "txcode selcall", .ui_fields = selcall_fields, .num_ui_fields = 2 },
	{ .name = "txcode", .usage = "txcode mdc <op> <arg> <unit_id>",
	  .description = "MDC-1200 data burst", .handler = cli_txcode,
	  .category = "Tones", .ui_label = "MDC-1200", .ui_type = CLI_UI_FORM,
	  .ui_command = "txcode mdc", .ui_fields = mdc_fields, .num_ui_fields = 3 },
	{ .name = "txcode", .usage = "txcode cwid [callsign]",
	  .description = "Manual CW ID", .handler = cli_txcode,
	  .category = "Tones", .ui_label = "CW ID", .ui_type = CLI_UI_FORM,
	  .ui_command = "txcode cwid", .ui_fields = cwid_fields, .num_ui_fields = 1 },
};

static kerchunk_module_def_t mod_txcode = {
	.name             = "mod_txcode",
	.version          = "1.0.0",
	.description      = "TX tone encoder and burst tone toolbox",
	.load             = txcode_load,
	.configure        = txcode_configure,
	.unload           = txcode_unload,
	.cli_commands     = cli_cmds,
	.num_cli_commands = sizeof(cli_cmds) / sizeof(cli_cmds[0]),
};

KERCHUNK_MODULE_DEFINE(mod_txcode);
