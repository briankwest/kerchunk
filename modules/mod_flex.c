/*
 * mod_flex.c — FLEX paging transmitter with frame-synchronized timing
 *
 * FLEX pagers use battery-saving mode: they wake up only during their
 * assigned frame slot (capcode % 128).  This module schedules TX to
 * align with the correct frame boundary so pagers receive the message.
 *
 * Time structure:
 *   1 hour  = 15 cycles × 4 minutes
 *   1 cycle = 128 frames × 1.875 seconds
 *   Frame 0 of cycle 0 = top of the UTC hour
 *
 * CLI:
 *   flex send <capcode> [speed] <message>   Send alphanumeric page
 *   flex tone <capcode>                     Send tone-only page
 *   flex numeric <capcode> <digits>         Send numeric page
 *   flex status                             Show module status + pending
 *
 * Config [flex]:
 *   enabled = on
 *   default_speed = 1600
 *   modulation = baseband | fsk
 *   deemphasis = off
 *   tx_level = 0.5
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <libflex/flex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define LOG_MOD "flex"

/* ── configuration ── */

static kerchunk_core_t *g_core;
static int g_enabled = 1;
static int g_tx_count = 0;
static int g_deemph = 0;
static int g_use_fsk = 0;
static float g_tx_level = 0.5f;
static flex_speed_t g_default_speed = FLEX_SPEED_1600_2;

/* ── pending page queue (protected by mutex) ── */

#define MAX_PENDING 16

typedef struct {
	uint32_t        capcode;
	flex_msg_type_t type;
	flex_speed_t    speed;
	char            text[512];
	uint16_t        target_frame;
	struct timespec fire_time;   /* when to begin TX (preamble start) */
	int16_t        *pcm;        /* pre-encoded audio, NULL if not yet encoded */
	size_t          pcm_len;
	int             valid;
} flex_pending_t;

static flex_pending_t g_pending[MAX_PENDING];
static int g_pending_count;
static pthread_mutex_t g_pending_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_worker_tid = -1;

/* ── frame timing ── */

/* pre-roll: start TX this many ms before the frame boundary.
 * Accounts for PTT key-up (~100ms) + audio queue latency (~50ms)
 * + preamble needs to be in the air when pager wakes up. */
#define FLEX_PREROLL_MS 200

static void flex_frame_at_time(time_t utc, uint16_t *cycle, uint16_t *frame)
{
	struct tm t;
	gmtime_r(&utc, &t);
	int sec_in_hour = t.tm_min * 60 + t.tm_sec;
	*cycle = (uint16_t)((sec_in_hour / 240) % 15);
	int sec_in_cycle = sec_in_hour % 240;
	*frame = (uint16_t)(((sec_in_cycle * 1000) / 1875) % 128);
}

static void flex_next_frame_time(uint32_t capcode,
                                 const struct timespec *now,
                                 struct timespec *when,
                                 uint16_t *out_cycle,
                                 uint16_t *out_frame)
{
	uint16_t target_frame = (uint16_t)(capcode % 128);
	uint16_t cur_cycle, cur_frame;
	flex_frame_at_time(now->tv_sec, &cur_cycle, &cur_frame);

	int delta = (int)target_frame - (int)cur_frame;
	if (delta <= 0) delta += 128;  /* next cycle */

	int64_t wait_ms = (int64_t)delta * 1875 - FLEX_PREROLL_MS;
	if (wait_ms < 0) {
		/* target frame is very soon; push to next cycle */
		wait_ms += 128 * 1875;
		cur_cycle = (cur_cycle + 1) % 15;
	}

	when->tv_sec  = now->tv_sec + (time_t)(wait_ms / 1000);
	when->tv_nsec = now->tv_nsec + (long)(wait_ms % 1000) * 1000000L;
	if (when->tv_nsec >= 1000000000L) {
		when->tv_sec++;
		when->tv_nsec -= 1000000000L;
	}

	/* compute the actual cycle/frame for the FIW at fire time */
	if (out_cycle && out_frame) {
		time_t fire_utc = when->tv_sec + (FLEX_PREROLL_MS / 1000);
		flex_frame_at_time(fire_utc, out_cycle, out_frame);
	}
}

/* ── pre-encode a page to PCM audio ── */

static int flex_pre_encode(flex_pending_t *p, uint16_t cycle, uint16_t frame)
{
	flex_encoder_t enc;
	flex_encoder_init(&enc, p->speed);
	flex_encoder_set_frame(&enc, cycle, frame);

	flex_err_t err = flex_encoder_add(&enc, p->capcode, FLEX_ADDR_SHORT,
	                                  p->type, p->text[0] ? p->text : NULL,
	                                  NULL, 0);
	if (err != FLEX_OK) return -1;

	uint8_t bitbuf[FLEX_BITSTREAM_MAX];
	size_t len = 0, bits = 0;
	err = flex_encode(&enc, bitbuf, sizeof(bitbuf), &len, &bits);
	if (err != FLEX_OK) return -1;

	float fbuf[512000];
	size_t ns = 0;

	if (g_use_fsk) {
		uint8_t *unpacked = (uint8_t *)malloc(bits);
		if (!unpacked) return -1;
		for (size_t i = 0; i < bits; i++)
			unpacked[i] = (bitbuf[i / 8] >> (7 - (i % 8))) & 1;
		flex_mod_t mod;
		flex_mod_init(&mod, p->speed, (float)g_core->sample_rate);
		err = flex_mod_bits(&mod, unpacked, bits,
		                    fbuf, sizeof(fbuf) / sizeof(float), &ns);
		free(unpacked);
	} else {
		int flags = g_deemph ? FLEX_BASEBAND_DEEMPH : 0;
		err = flex_baseband_ex(bitbuf, bits, p->speed,
		                       (float)g_core->sample_rate, flags,
		                       fbuf, sizeof(fbuf) / sizeof(float), &ns);
	}
	if (err != FLEX_OK) return -1;

	p->pcm = (int16_t *)malloc(ns * sizeof(int16_t));
	if (!p->pcm) return -1;
	for (size_t i = 0; i < ns; i++)
		p->pcm[i] = (int16_t)(fbuf[i] * 32767.0f * g_tx_level);
	p->pcm_len = ns;

	return 0;
}

/* ── worker thread: encodes and fires pages at frame boundaries ── */

static void *flex_worker(void *arg)
{
	(void)arg;

	while (!g_core->thread_should_stop(g_worker_tid)) {
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);

		flex_pending_t *ready = NULL;

		pthread_mutex_lock(&g_pending_mutex);
		for (int i = 0; i < g_pending_count; i++) {
			flex_pending_t *p = &g_pending[i];
			if (!p->valid) continue;

			/* pre-encode if not yet done */
			if (!p->pcm) {
				uint16_t cycle, frame;
				flex_frame_at_time(p->fire_time.tv_sec + (FLEX_PREROLL_MS / 1000),
				                   &cycle, &frame);
				if (flex_pre_encode(p, cycle, frame) < 0) {
					g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
					            "encode failed for capcode=%u", p->capcode);
					p->valid = 0;
					continue;
				}
			}

			/* check if fire time has arrived */
			if (now.tv_sec > p->fire_time.tv_sec ||
			    (now.tv_sec == p->fire_time.tv_sec &&
			     now.tv_nsec >= p->fire_time.tv_nsec)) {
				ready = p;
				break;
			}
		}
		pthread_mutex_unlock(&g_pending_mutex);

		if (ready) {
			g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
			            "TX: capcode=%u frame=%u speed=%d (%zu samples)",
			            ready->capcode, ready->target_frame,
			            flex_speed_bps(ready->speed), ready->pcm_len);

			g_core->queue_audio_buffer(ready->pcm, ready->pcm_len,
			                           KERCHUNK_PRI_NORMAL,
			                           QUEUE_FLAG_NO_TAIL);

			free(ready->pcm);
			ready->pcm = NULL;
			ready->valid = 0;
			g_tx_count++;

			/* compact the queue */
			pthread_mutex_lock(&g_pending_mutex);
			int w = 0;
			for (int r = 0; r < g_pending_count; r++) {
				if (g_pending[r].valid) {
					if (w != r)
						g_pending[w] = g_pending[r];
					w++;
				}
			}
			g_pending_count = w;
			pthread_mutex_unlock(&g_pending_mutex);
		}

		/* sleep 50ms between checks */
		struct timespec sl = { 0, 50000000L };
		nanosleep(&sl, NULL);
	}

	return NULL;
}

/* ── queue a page for frame-synchronized TX ── */

static int flex_schedule_page(uint32_t capcode, flex_msg_type_t type,
                              flex_speed_t speed, const char *text,
                              kerchunk_resp_t *resp)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);

	uint16_t cycle, frame;
	struct timespec fire;
	flex_next_frame_time(capcode, &now, &fire, &cycle, &frame);

	int64_t wait_ms = (fire.tv_sec - now.tv_sec) * 1000
	                + (fire.tv_nsec - now.tv_nsec) / 1000000;

	pthread_mutex_lock(&g_pending_mutex);

	if (g_pending_count >= MAX_PENDING) {
		pthread_mutex_unlock(&g_pending_mutex);
		resp_str(resp, "error", "pending queue full");
		resp_finish(resp);
		return -1;
	}

	flex_pending_t *p = &g_pending[g_pending_count++];
	memset(p, 0, sizeof(*p));
	p->capcode = capcode;
	p->type = type;
	p->speed = speed;
	p->target_frame = (uint16_t)(capcode % 128);
	p->fire_time = fire;
	p->valid = 1;
	if (text)
		snprintf(p->text, sizeof(p->text), "%s", text);

	pthread_mutex_unlock(&g_pending_mutex);

	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
	            "scheduled: capcode=%u frame=%u cycle=%u fire_in=%ldms",
	            capcode, capcode % 128, (unsigned)cycle, (long)wait_ms);

	resp_str(resp, "status", "scheduled");
	resp_int(resp, "capcode", (int)capcode);
	resp_int(resp, "target_frame", (int)(capcode % 128));
	resp_int(resp, "target_cycle", (int)cycle);
	resp_int(resp, "fire_in_ms", (int)wait_ms);
	resp_int(resp, "speed", flex_speed_bps(speed));
	return 0;
}

/* ── immediate TX (bypass scheduler, for testing) ── */

static int flex_tx_now(uint32_t capcode, flex_msg_type_t type,
                       flex_speed_t speed, const char *text)
{
	uint16_t cycle = 0, frame = 0;
	{
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		flex_frame_at_time(now.tv_sec, &cycle, &frame);
	}

	flex_pending_t tmp;
	memset(&tmp, 0, sizeof(tmp));
	tmp.capcode = capcode;
	tmp.type = type;
	tmp.speed = speed;
	if (text) snprintf(tmp.text, sizeof(tmp.text), "%s", text);

	if (flex_pre_encode(&tmp, cycle, frame) < 0)
		return -1;

	g_core->queue_audio_buffer(tmp.pcm, tmp.pcm_len,
	                           KERCHUNK_PRI_NORMAL, QUEUE_FLAG_NO_TAIL);
	free(tmp.pcm);
	g_tx_count++;
	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
	            "immediate TX: capcode=%u speed=%d type=%d",
	            capcode, flex_speed_bps(speed), (int)type);
	return 0;
}

/* ── CLI handler ── */

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
			if (maybe == 1600 || maybe == 3200 || maybe == 6400) {
				speed = (maybe == 3200) ? FLEX_SPEED_3200_2 :
				        (maybe == 6400) ? FLEX_SPEED_6400_4 :
				        FLEX_SPEED_1600_2;
				msg_start = 4;
			}
		}

		char msg[512] = {0};
		for (int i = msg_start; i < argc; i++) {
			if (i > msg_start) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
			strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
		}

		int rc = flex_schedule_page(capcode, FLEX_MSG_ALPHA, speed, msg, resp);
		if (rc == 0) resp_str(resp, "message", msg);

	} else if (strcmp(sub, "now") == 0) {
		/* immediate TX — bypass frame scheduler */
		if (argc < 4) {
			resp_str(resp, "error", "usage: flex now <capcode> [speed] <message>");
			resp_finish(resp);
			return -1;
		}
		uint32_t capcode = (uint32_t)strtoul(argv[2], NULL, 10);
		flex_speed_t speed = g_default_speed;
		int msg_start = 3;
		if (argc >= 5) {
			int maybe = atoi(argv[3]);
			if (maybe == 1600 || maybe == 3200 || maybe == 6400) {
				speed = (maybe == 3200) ? FLEX_SPEED_3200_2 :
				        (maybe == 6400) ? FLEX_SPEED_6400_4 :
				        FLEX_SPEED_1600_2;
				msg_start = 4;
			}
		}
		char msg[512] = {0};
		for (int i = msg_start; i < argc; i++) {
			if (i > msg_start) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
			strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
		}
		int rc = flex_tx_now(capcode, FLEX_MSG_ALPHA, speed, msg);
		resp_str(resp, "status", rc == 0 ? "queued" : "error");
		if (rc == 0) {
			resp_int(resp, "capcode", (int)capcode);
			resp_str(resp, "message", msg);
		}

	} else if (strcmp(sub, "numeric") == 0) {
		if (argc < 4) {
			resp_str(resp, "error", "usage: flex numeric <capcode> <digits>");
			resp_finish(resp);
			return -1;
		}
		uint32_t capcode = (uint32_t)strtoul(argv[2], NULL, 10);
		flex_schedule_page(capcode, FLEX_MSG_NUMERIC, g_default_speed,
		                   argv[3], resp);

	} else if (strcmp(sub, "tone") == 0) {
		if (argc < 3) {
			resp_str(resp, "error", "usage: flex tone <capcode>");
			resp_finish(resp);
			return -1;
		}
		uint32_t capcode = (uint32_t)strtoul(argv[2], NULL, 10);
		flex_schedule_page(capcode, FLEX_MSG_TONE_ONLY, g_default_speed,
		                   NULL, resp);

	} else if (strcmp(sub, "status") == 0) {
		resp_str(resp, "module", "mod_flex");
		resp_bool(resp, "enabled", g_enabled);
		resp_str(resp, "modulation", g_use_fsk ? "fsk" : "baseband");
		resp_bool(resp, "deemphasis", g_deemph);
		resp_int(resp, "tx_level_pct", (int)(g_tx_level * 100.0f));
		resp_int(resp, "tx_count", g_tx_count);
		resp_int(resp, "default_speed", flex_speed_bps(g_default_speed));

		pthread_mutex_lock(&g_pending_mutex);
		resp_int(resp, "pending", g_pending_count);

		/* show current FLEX time */
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		uint16_t cycle, frame;
		flex_frame_at_time(now.tv_sec, &cycle, &frame);
		resp_int(resp, "current_cycle", (int)cycle);
		resp_int(resp, "current_frame", (int)frame);

		pthread_mutex_unlock(&g_pending_mutex);

	} else {
		goto usage;
	}

	resp_finish(resp);
	return 0;

usage:
	resp_text_raw(resp, "FLEX paging transmitter (frame-synchronized)\n\n"
		"  flex send <capcode> [speed] <message>\n"
		"    Schedule alphanumeric page for frame boundary.\n"
		"    capcode: pager address (decimal)\n"
		"    speed:   1600 (default), 3200, 6400\n"
		"    message: text string\n\n"
		"  flex now <capcode> [speed] <message>\n"
		"    Transmit immediately (bypass frame scheduler).\n"
		"    For testing — pagers in battery-save may miss this.\n\n"
		"  flex numeric <capcode> <digits>\n"
		"    Schedule numeric page.\n\n"
		"  flex tone <capcode>\n"
		"    Schedule tone-only page.\n\n"
		"  flex status\n"
		"    Show module status, pending queue, current FLEX time.\n\n"
		"Config: [flex] enabled, default_speed, modulation, deemphasis, tx_level\n");
	resp_str(resp, "error", "usage: flex <send|now|numeric|tone|status>");
	resp_finish(resp);
	return -1;
}

/* ── UI fields ── */

static const kerchunk_ui_field_t flex_send_fields[] = {
	{ "capcode", "Capcode", "number", NULL, "101000" },
	{ "speed",   "Speed",   "select", "1600,3200,6400", NULL },
	{ "msg",     "Message", "text", NULL, "Hello FLEX" },
};

static const kerchunk_ui_field_t flex_numeric_fields[] = {
	{ "capcode", "Capcode", "number", NULL, "101000" },
	{ "digits",  "Digits",  "text", NULL, "5551234" },
};

static const kerchunk_ui_field_t flex_tone_fields[] = {
	{ "capcode", "Capcode", "number", NULL, "101000" },
};

static const kerchunk_cli_cmd_t cli_cmds[] = {
	{ .name = "flex", .usage = "flex send <capcode> [speed] <message>",
	  .description = "FLEX alpha page", .handler = cli_flex,
	  .category = "Paging", .ui_label = "FLEX Alpha", .ui_type = CLI_UI_FORM,
	  .ui_command = "flex send", .ui_fields = flex_send_fields, .num_ui_fields = 3,
	  .subcommands = "send,now,numeric,tone,status" },
	{ .name = "flex", .usage = "flex numeric <capcode> <digits>",
	  .description = "FLEX numeric page", .handler = cli_flex,
	  .category = "Paging", .ui_label = "FLEX Numeric", .ui_type = CLI_UI_FORM,
	  .ui_command = "flex numeric", .ui_fields = flex_numeric_fields, .num_ui_fields = 2 },
	{ .name = "flex", .usage = "flex tone <capcode>",
	  .description = "FLEX tone-only page", .handler = cli_flex,
	  .category = "Paging", .ui_label = "FLEX Tone", .ui_type = CLI_UI_FORM,
	  .ui_command = "flex tone", .ui_fields = flex_tone_fields, .num_ui_fields = 1 },
};

/* ── module lifecycle ── */

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
	if (v) {
		int sv = atoi(v);
		switch (sv) {
		case 1600: g_default_speed = FLEX_SPEED_1600_2; break;
		case 3200: g_default_speed = FLEX_SPEED_3200_2; break;
		case 6400: g_default_speed = FLEX_SPEED_6400_4; break;
		}
	}
	v = g_core->config_get("flex", "deemphasis");
	g_deemph = (v && strcmp(v, "on") == 0);
	v = g_core->config_get("flex", "modulation");
	g_use_fsk = (v && strcmp(v, "fsk") == 0);
	v = g_core->config_get("flex", "tx_level");
	if (v) {
		float lv = (float)atof(v);
		if (lv > 0.0f && lv <= 1.0f) g_tx_level = lv;
	}

	/* start worker thread for frame-synchronized TX */
	if (g_enabled) {
		g_worker_tid = g_core->thread_create("flex_tx", flex_worker, NULL);
		if (g_worker_tid < 0) {
			g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
			            "failed to create worker thread");
		} else {
			g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
			            "worker thread started (tid=%d)", g_worker_tid);
		}
	}

	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
	            "configured: speed=%d mod=%s deemph=%s level=%.0f%%",
	            flex_speed_bps(g_default_speed),
	            g_use_fsk ? "fsk" : "baseband",
	            g_deemph ? "on" : "off",
	            g_tx_level * 100.0f);
	return 0;
}

static void mod_unload(void)
{
	/* stop worker thread */
	if (g_worker_tid >= 0) {
		g_core->thread_stop(g_worker_tid);
		g_core->thread_join(g_worker_tid);
		g_worker_tid = -1;
	}

	/* free any pending pages */
	pthread_mutex_lock(&g_pending_mutex);
	for (int i = 0; i < g_pending_count; i++) {
		free(g_pending[i].pcm);
		g_pending[i].pcm = NULL;
	}
	g_pending_count = 0;
	pthread_mutex_unlock(&g_pending_mutex);

	g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
	            "unloaded (tx_count=%d)", g_tx_count);
}

static const kerchunk_module_def_t mod_def = {
	.name         = "mod_flex",
	.version      = "2.0.0",
	.description  = "FLEX paging transmitter (frame-synchronized)",
	.load         = mod_load,
	.configure    = mod_configure,
	.unload       = mod_unload,
	.cli_commands     = cli_cmds,
	.num_cli_commands = sizeof(cli_cmds) / sizeof(cli_cmds[0]),
};

KERCHUNK_MODULE_DEFINE(mod_def);
