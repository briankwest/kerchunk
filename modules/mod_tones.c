/*
 * mod_tones.c — Burst tone CLI commands
 *
 * One-shot tone generation queued to the audio pipeline:
 *   tones                         Show help
 *   tones dtmf <digits>           Transmit DTMF sequence
 *   tones twotone <f1> <f2> <ms>  Two-tone sequential page
 *   tones selcall <digits> [std]  5-tone selective call
 *   tones burst <freq> <ms>       Single tone burst
 *   tones mdc <op> <arg> <unit>   MDC-1200 data burst
 *   tones cwid [callsign]         Manual CW ID
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "plcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MOD "tones"
#define AMP     16000   /* default burst amplitude */

static kerchunk_core_t *g_core;

/* ── Helper: render a burst encoder into a buffer and queue it ── */

static int queue_burst(int16_t *buf, size_t n)
{
	g_core->queue_audio_buffer(buf, n, KERCHUNK_PRI_NORMAL, 0);
	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
	            "queued burst: %zu samples (%.1f ms)",
	            n, (double)n / g_core->sample_rate * 1000.0);
	return 0;
}

/* ── CLI ── */

static int cli_tones(int argc, const char **argv, kerchunk_resp_t *r)
{
	if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

	int rate = g_core->sample_rate;

	/* No subcommand: show help */
	if (argc < 2) goto usage;

	const char *sub = argv[1];

	/* ── tones dtmf <digits> ── */
	if (strcmp(sub, "dtmf") == 0) {
		if (argc < 3) {
			resp_str(r, "error", "usage: tones dtmf <digits>");
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

	/* ── tones twotone <freq1> <freq2> <duration_ms> ── */
	if (strcmp(sub, "twotone") == 0) {
		if (argc < 5) {
			resp_str(r, "error", "usage: tones twotone <freq1> <freq2> <duration_ms>");
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

	/* ── tones selcall <digits> [zvei1|ccir|eia] ── */
	if (strcmp(sub, "selcall") == 0) {
		if (argc < 3) {
			resp_str(r, "error", "usage: tones selcall <digits> [zvei1|ccir|eia]");
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

	/* ── tones burst <freq> <duration_ms> ── */
	if (strcmp(sub, "burst") == 0) {
		if (argc < 4) {
			resp_str(r, "error", "usage: tones burst <freq_hz> <duration_ms>");
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

	/* ── tones mdc <op> <arg> <unit_id> ── */
	if (strcmp(sub, "mdc") == 0) {
		if (argc < 5) {
			resp_str(r, "error", "usage: tones mdc <op> <arg> <unit_id>");
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

	/* ── tones cwid [callsign] ── */
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
		int cw_freq = g_core->config_get_int("repeater", "cwid_freq", 800);
		int cw_wpm  = g_core->config_get_int("repeater", "cwid_wpm", 20);
		if (cw_wpm < 5) cw_wpm = 20;
		plcode_cwid_enc_t *enc = NULL;
		if (plcode_cwid_enc_create(&enc, rate, call, cw_freq, cw_wpm, AMP) != PLCODE_OK) {
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
	resp_text_raw(r, "Burst tone toolbox\n\n"
		"  tones dtmf <digits>\n"
		"    Transmit DTMF sequence (0-9, A-D, *, #).\n"
		"    100ms tone, 50ms gap per digit.\n\n"
		"  tones twotone <freq1> <freq2> <duration_ms>\n"
		"    Two-tone sequential page (fire/EMS paging).\n"
		"    freq1/freq2: Hz, duration: ms per tone.\n\n"
		"  tones selcall <digits> [zvei1|ccir|eia]\n"
		"    5-tone selective call. Default standard: ZVEI-1.\n"
		"    digits: 5-digit address (0-9).\n\n"
		"  tones burst <freq_hz> <duration_ms>\n"
		"    Single tone burst (e.g. 1750 Hz repeater access).\n"
		"    freq: 100-10000 Hz, duration: 10-30000 ms.\n\n"
		"  tones mdc <op> <arg> <unit_id>\n"
		"    MDC-1200 data burst. All values in hex.\n"
		"    op: opcode, arg: argument, unit_id: 4-digit hex.\n\n"
		"  tones cwid [callsign]\n"
		"    Manual CW ID at 800 Hz, 20 WPM.\n"
		"    Uses [general] callsign if omitted.\n");
	resp_str(r, "error", "usage: tones <dtmf|twotone|selcall|burst|mdc|cwid>");
	resp_finish(r);
	return -1;
}

/* ── Module lifecycle ── */

static int tones_load(kerchunk_core_t *core)
{
	g_core = core;
	return 0;
}

static void tones_unload(void)
{
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
	{ .name = "tones", .usage = "tones dtmf <digits>",
	  .description = "DTMF sequence", .handler = cli_tones,
	  .category = "Tones", .ui_label = "DTMF", .ui_type = CLI_UI_FORM,
	  .ui_command = "tones dtmf", .ui_fields = dtmf_fields, .num_ui_fields = 1,
	  .subcommands = "dtmf,twotone,selcall,burst,mdc,cwid" },
	{ .name = "tones", .usage = "tones burst <freq> <dur_ms>",
	  .description = "Tone burst", .handler = cli_tones,
	  .category = "Tones", .ui_label = "Tone Burst", .ui_type = CLI_UI_FORM,
	  .ui_command = "tones burst", .ui_fields = burst_fields, .num_ui_fields = 2 },
	{ .name = "tones", .usage = "tones twotone <f1> <f2> <dur_ms>",
	  .description = "Two-tone page", .handler = cli_tones,
	  .category = "Tones", .ui_label = "Two-Tone", .ui_type = CLI_UI_FORM,
	  .ui_command = "tones twotone", .ui_fields = twotone_fields, .num_ui_fields = 3 },
	{ .name = "tones", .usage = "tones selcall <digits> [std]",
	  .description = "5-tone selective call", .handler = cli_tones,
	  .category = "Tones", .ui_label = "Selcall", .ui_type = CLI_UI_FORM,
	  .ui_command = "tones selcall", .ui_fields = selcall_fields, .num_ui_fields = 2 },
	{ .name = "tones", .usage = "tones mdc <op> <arg> <unit_id>",
	  .description = "MDC-1200 data burst", .handler = cli_tones,
	  .category = "Tones", .ui_label = "MDC-1200", .ui_type = CLI_UI_FORM,
	  .ui_command = "tones mdc", .ui_fields = mdc_fields, .num_ui_fields = 3 },
	{ .name = "tones", .usage = "tones cwid [callsign]",
	  .description = "Manual CW ID", .handler = cli_tones,
	  .category = "Tones", .ui_label = "CW ID", .ui_type = CLI_UI_FORM,
	  .ui_command = "tones cwid", .ui_fields = cwid_fields, .num_ui_fields = 1 },
};

static kerchunk_module_def_t mod_tones = {
	.name             = "mod_tones",
	.version          = "1.0.0",
	.description      = "Burst tone toolbox",
	.load             = tones_load,
	.unload           = tones_unload,
	.cli_commands     = cli_cmds,
	.num_cli_commands = sizeof(cli_cmds) / sizeof(cli_cmds[0]),
};

KERCHUNK_MODULE_DEFINE(mod_tones);
