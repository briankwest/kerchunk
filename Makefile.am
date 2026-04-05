# Makefile.am — kerchunk autotools build
# vim: set ts=8 noet:

ACLOCAL_AMFLAGS = -I m4

# ---------- common flags ----------

GIT_HASH := $(shell git -C $(srcdir) rev-parse --short HEAD 2>/dev/null || echo "unknown")

AM_CFLAGS = -Wall -Wextra -Wpedantic -O2 \
	    -Iinclude $(PLATFORM_CFLAGS) \
	    $(PLCODE_CFLAGS) \
	    -DKERCHUNK_GIT_HASH=\"$(GIT_HASH)\"

# ---------- convenience library: linenoise ----------
# Vendored linenoise needs relaxed warnings (no -Wpedantic)

noinst_LTLIBRARIES = liblinenoise.la
liblinenoise_la_SOURCES = cli/linenoise.c
liblinenoise_la_CFLAGS  = -std=c11 -Wall -O2 \
			   $(PLATFORM_CFLAGS) -D_DEFAULT_SOURCE

# ---------- programs ----------

bin_PROGRAMS = kerchunkd kerchunk kerchunk-diag

# --- kerchunkd (daemon) ---

kerchunkd_SOURCES = \
	src/main.c \
	src/kerchunk_core.c \
	src/kerchunk_events.c \
	src/kerchunk_modules.c \
	src/kerchunk_queue.c \
	src/kerchunk_audio.c \
	src/kerchunk_hid.c \
	src/kerchunk_socket.c \
	src/kerchunk_config.c \
	src/kerchunk_log.c \
	src/kerchunk_user.c \
	src/kerchunk_wav.c \
	src/kerchunk_timer.c \
	src/kerchunk_sched.c \
	src/kerchunk_threads.c \
	src/kerchunk_resp.c \
	src/kerchunk_evt_json.c \
	src/kerchunk_console.c \
	src/kerchunk_rrd.c

kerchunkd_CFLAGS = $(AM_CFLAGS) \
		   -Icli \
		   -DKERCHUNK_MODULE_DIR=\"$(kerchunkmoddir)\" \
		   $(PORTAUDIO_CFLAGS) \
		   $(ALSA_CFLAGS)

kerchunkd_LDFLAGS = $(EXPORT_DYNAMIC)
kerchunkd_LDADD = liblinenoise.la \
		  $(PLCODE_LIBS) $(PORTAUDIO_LIBS) $(ALSA_LIBS) \
		  -lm -ldl $(PTHREAD_LIBS)

# --- kerchunk (CLI client) ---

kerchunk_SOURCES = cli/kerchunk.c

kerchunk_CFLAGS = -std=c11 -Wall -O2 \
		  $(PLATFORM_CFLAGS) -D_DEFAULT_SOURCE

kerchunk_LDADD = liblinenoise.la $(PTHREAD_LIBS)

# --- kerchunk-diag (diagnostic tool) ---

kerchunk_diag_SOURCES = tools/kerchunk_diag.c
kerchunk_diag_CFLAGS  = -std=c11 -Wall -O2 \
			$(PLATFORM_CFLAGS) -D_DEFAULT_SOURCE \
			$(PORTAUDIO_CFLAGS)
kerchunk_diag_LDADD   = $(PORTAUDIO_LIBS) -lm

# ---------- modules (libtool modules) ----------

kerchunkmod_LTLIBRARIES = \
	modules/mod_repeater.la \
	modules/mod_cwid.la \
	modules/mod_courtesy.la \
	modules/mod_caller.la \
	modules/mod_dtmfcmd.la \
	modules/mod_voicemail.la \
	modules/mod_gpio.la \
	modules/mod_logger.la \
	modules/mod_weather.la \
	modules/mod_time.la \
	modules/mod_recorder.la \
	modules/mod_tones.la \
	modules/mod_emergency.la \
	modules/mod_parrot.la \
	modules/mod_cdr.la \
	modules/mod_tts.la \
	modules/mod_nws.la \
	modules/mod_stats.la \
	modules/mod_web.la \
	modules/mod_otp.la \
	modules/mod_webhook.la \
	modules/mod_scrambler.la \
	modules/mod_freeswitch.la

if HAVE_RTLSDR
kerchunkmod_LTLIBRARIES += modules/mod_sdr.la
endif

# Common module flags
MOD_CFLAGS  = $(AM_CFLAGS)
MOD_LDFLAGS = -module -avoid-version -shared

# --- basic modules (just plcode) ---

modules_mod_repeater_la_SOURCES  = modules/mod_repeater.c
modules_mod_repeater_la_CFLAGS   = $(MOD_CFLAGS)
modules_mod_repeater_la_LDFLAGS  = $(MOD_LDFLAGS)
modules_mod_repeater_la_LIBADD   = $(PLCODE_LIBS)

modules_mod_courtesy_la_SOURCES  = modules/mod_courtesy.c
modules_mod_courtesy_la_CFLAGS   = $(MOD_CFLAGS)
modules_mod_courtesy_la_LDFLAGS  = $(MOD_LDFLAGS)
modules_mod_courtesy_la_LIBADD   = $(PLCODE_LIBS)

modules_mod_caller_la_SOURCES    = modules/mod_caller.c
modules_mod_caller_la_CFLAGS     = $(MOD_CFLAGS)
modules_mod_caller_la_LDFLAGS    = $(MOD_LDFLAGS)
modules_mod_caller_la_LIBADD     = $(PLCODE_LIBS)

modules_mod_dtmfcmd_la_SOURCES   = modules/mod_dtmfcmd.c
modules_mod_dtmfcmd_la_CFLAGS    = $(MOD_CFLAGS)
modules_mod_dtmfcmd_la_LDFLAGS   = $(MOD_LDFLAGS)
modules_mod_dtmfcmd_la_LIBADD    = $(PLCODE_LIBS)

modules_mod_voicemail_la_SOURCES = modules/mod_voicemail.c
modules_mod_voicemail_la_CFLAGS  = $(MOD_CFLAGS)
modules_mod_voicemail_la_LDFLAGS = $(MOD_LDFLAGS)
modules_mod_voicemail_la_LIBADD  = $(PLCODE_LIBS)

modules_mod_gpio_la_SOURCES      = modules/mod_gpio.c
modules_mod_gpio_la_CFLAGS       = $(MOD_CFLAGS)
modules_mod_gpio_la_LDFLAGS      = $(MOD_LDFLAGS)
modules_mod_gpio_la_LIBADD       = $(PLCODE_LIBS)

modules_mod_logger_la_SOURCES    = modules/mod_logger.c
modules_mod_logger_la_CFLAGS     = $(MOD_CFLAGS)
modules_mod_logger_la_LDFLAGS    = $(MOD_LDFLAGS)
modules_mod_logger_la_LIBADD     = $(PLCODE_LIBS)

modules_mod_time_la_SOURCES      = modules/mod_time.c
modules_mod_time_la_CFLAGS       = $(MOD_CFLAGS)
modules_mod_time_la_LDFLAGS      = $(MOD_LDFLAGS)
modules_mod_time_la_LIBADD       = $(PLCODE_LIBS)

modules_mod_recorder_la_SOURCES  = modules/mod_recorder.c
modules_mod_recorder_la_CFLAGS   = $(MOD_CFLAGS)
modules_mod_recorder_la_LDFLAGS  = $(MOD_LDFLAGS)
modules_mod_recorder_la_LIBADD   = $(PLCODE_LIBS)

modules_mod_tones_la_SOURCES     = modules/mod_tones.c
modules_mod_tones_la_CFLAGS      = $(MOD_CFLAGS)
modules_mod_tones_la_LDFLAGS     = $(MOD_LDFLAGS)
modules_mod_tones_la_LIBADD      = $(PLCODE_LIBS)

modules_mod_emergency_la_SOURCES = modules/mod_emergency.c
modules_mod_emergency_la_CFLAGS  = $(MOD_CFLAGS)
modules_mod_emergency_la_LDFLAGS = $(MOD_LDFLAGS)
modules_mod_emergency_la_LIBADD  = $(PLCODE_LIBS)

modules_mod_parrot_la_SOURCES    = modules/mod_parrot.c
modules_mod_parrot_la_CFLAGS     = $(MOD_CFLAGS)
modules_mod_parrot_la_LDFLAGS    = $(MOD_LDFLAGS)
modules_mod_parrot_la_LIBADD     = $(PLCODE_LIBS)

modules_mod_cdr_la_SOURCES       = modules/mod_cdr.c
modules_mod_cdr_la_CFLAGS        = $(MOD_CFLAGS)
modules_mod_cdr_la_LDFLAGS       = $(MOD_LDFLAGS)
modules_mod_cdr_la_LIBADD        = $(PLCODE_LIBS)

modules_mod_stats_la_SOURCES     = modules/mod_stats.c
modules_mod_stats_la_CFLAGS      = $(MOD_CFLAGS)
modules_mod_stats_la_LDFLAGS     = $(MOD_LDFLAGS)
modules_mod_stats_la_LIBADD      = $(PLCODE_LIBS)

modules_mod_otp_la_SOURCES       = modules/mod_otp.c
modules_mod_otp_la_CFLAGS        = $(MOD_CFLAGS)
modules_mod_otp_la_LDFLAGS       = $(MOD_LDFLAGS)
modules_mod_otp_la_LIBADD        = $(PLCODE_LIBS)

modules_mod_scrambler_la_SOURCES = modules/mod_scrambler.c
modules_mod_scrambler_la_CFLAGS  = $(MOD_CFLAGS)
modules_mod_scrambler_la_LDFLAGS = $(MOD_LDFLAGS)
modules_mod_scrambler_la_LIBADD  = $(PLCODE_LIBS)

modules_mod_freeswitch_la_SOURCES = modules/mod_freeswitch.c
modules_mod_freeswitch_la_CFLAGS  = $(MOD_CFLAGS)
modules_mod_freeswitch_la_LDFLAGS = $(MOD_LDFLAGS)
modules_mod_freeswitch_la_LIBADD  = $(PLCODE_LIBS)

# --- curl modules ---

modules_mod_cwid_la_SOURCES      = modules/mod_cwid.c
modules_mod_cwid_la_CFLAGS       = $(MOD_CFLAGS) $(CURL_CFLAGS)
modules_mod_cwid_la_LDFLAGS      = $(MOD_LDFLAGS)
modules_mod_cwid_la_LIBADD       = $(PLCODE_LIBS) $(CURL_LIBS)

modules_mod_weather_la_SOURCES   = modules/mod_weather.c
modules_mod_weather_la_CFLAGS    = $(MOD_CFLAGS) $(CURL_CFLAGS)
modules_mod_weather_la_LDFLAGS   = $(MOD_LDFLAGS)
modules_mod_weather_la_LIBADD    = $(PLCODE_LIBS) $(CURL_LIBS)

modules_mod_nws_la_SOURCES       = modules/mod_nws.c
modules_mod_nws_la_CFLAGS        = $(MOD_CFLAGS) $(CURL_CFLAGS)
modules_mod_nws_la_LDFLAGS       = $(MOD_LDFLAGS)
modules_mod_nws_la_LIBADD        = $(PLCODE_LIBS) $(CURL_LIBS)

modules_mod_webhook_la_SOURCES   = modules/mod_webhook.c
modules_mod_webhook_la_CFLAGS    = $(MOD_CFLAGS) $(CURL_CFLAGS)
modules_mod_webhook_la_LDFLAGS   = $(MOD_LDFLAGS)
modules_mod_webhook_la_LIBADD    = $(PLCODE_LIBS) $(CURL_LIBS)

# --- TTS module (curl + optional nemo) ---

modules_mod_tts_la_SOURCES       = modules/mod_tts.c
modules_mod_tts_la_CFLAGS        = $(MOD_CFLAGS) $(CURL_CFLAGS) $(NEMO_CFLAGS)
modules_mod_tts_la_LDFLAGS       = $(MOD_LDFLAGS)
modules_mod_tts_la_LIBADD        = $(PLCODE_LIBS) $(CURL_LIBS) $(NEMO_LIBS)

# --- web module (mongoose + openssl) ---

modules_mod_web_la_SOURCES       = modules/mod_web.c vendor/mongoose.c
modules_mod_web_la_CFLAGS        = $(MOD_CFLAGS) -Ivendor \
				   -DMG_TLS=MG_TLS_OPENSSL \
				   -DMG_ENABLE_LINES=0 \
				   -Wno-maybe-uninitialized \
				   -Wno-pedantic \
				   $(OPENSSL_CFLAGS)
modules_mod_web_la_LDFLAGS       = $(MOD_LDFLAGS)
modules_mod_web_la_LIBADD        = $(PLCODE_LIBS) $(OPENSSL_LIBS)

# --- SDR module (conditional on rtlsdr) ---

if HAVE_RTLSDR
modules_mod_sdr_la_SOURCES       = modules/mod_sdr.c
modules_mod_sdr_la_CFLAGS        = $(MOD_CFLAGS) $(RTLSDR_CFLAGS)
modules_mod_sdr_la_LDFLAGS       = $(MOD_LDFLAGS)
modules_mod_sdr_la_LIBADD        = $(PLCODE_LIBS) $(RTLSDR_LIBS)
endif

# --- Paging / APRS modules (conditional on libraries) ---

if HAVE_LIBPOCSAG
kerchunkmod_LTLIBRARIES += modules/mod_pocsag.la
modules_mod_pocsag_la_SOURCES    = modules/mod_pocsag.c
modules_mod_pocsag_la_CFLAGS     = $(MOD_CFLAGS) $(LIBPOCSAG_CFLAGS)
modules_mod_pocsag_la_LDFLAGS    = $(MOD_LDFLAGS)
modules_mod_pocsag_la_LIBADD     = $(LIBPOCSAG_LIBS)
endif

if HAVE_LIBFLEX
kerchunkmod_LTLIBRARIES += modules/mod_flex.la
modules_mod_flex_la_SOURCES      = modules/mod_flex.c
modules_mod_flex_la_CFLAGS       = $(MOD_CFLAGS) $(LIBFLEX_CFLAGS)
modules_mod_flex_la_LDFLAGS      = $(MOD_LDFLAGS)
modules_mod_flex_la_LIBADD       = $(LIBFLEX_LIBS)
endif

if HAVE_LIBAPRS
kerchunkmod_LTLIBRARIES += modules/mod_aprs.la
modules_mod_aprs_la_SOURCES      = modules/mod_aprs.c
modules_mod_aprs_la_CFLAGS       = $(MOD_CFLAGS) $(LIBAPRS_CFLAGS)
modules_mod_aprs_la_LDFLAGS      = $(MOD_LDFLAGS)
modules_mod_aprs_la_LIBADD       = $(LIBAPRS_LIBS) -lm
endif

if HAVE_LIBPOC
kerchunkmod_LTLIBRARIES += modules/mod_poc.la
modules_mod_poc_la_SOURCES       = modules/mod_poc.c
modules_mod_poc_la_CFLAGS        = $(MOD_CFLAGS) $(LIBPOC_CFLAGS)
modules_mod_poc_la_LDFLAGS       = $(MOD_LDFLAGS)
modules_mod_poc_la_LIBADD        = $(LIBPOC_LIBS)
endif

# ---------- tests ----------

check_PROGRAMS = test_kerchunk
TESTS = test_kerchunk

test_kerchunk_SOURCES = \
	tests/test_main.c \
	tests/test_events.c \
	tests/test_config.c \
	tests/test_resp.c \
	tests/test_queue.c \
	tests/test_repeater.c \
	tests/test_cwid.c \
	tests/test_integration.c \
	tests/test_integ_repeater.c \
	tests/test_integ_dtmfcmd.c \
	tests/test_integ_caller.c \
	tests/test_integ_voicemail.c \
	tests/test_integ_timer.c \
	tests/test_integ_userdb.c \
	tests/test_integ_time.c \
	tests/test_integ_decode.c \
	tests/test_integ_recorder.c \
	tests/test_integ_emergency.c \
	tests/test_integ_parrot.c \
	tests/test_integ_cdr.c \
	tests/test_integ_cwid.c \
	tests/test_integ_stats.c \
	tests/test_integ_otp.c \
	tests/test_integ_scrambler.c \
	tests/test_integ_freeswitch.c \
	tests/test_stubs.c \
	src/kerchunk_events.c \
	src/kerchunk_config.c \
	src/kerchunk_log.c \
	src/kerchunk_queue.c \
	src/kerchunk_wav.c \
	src/kerchunk_timer.c \
	src/kerchunk_sched.c \
	src/kerchunk_threads.c \
	src/kerchunk_user.c \
	src/kerchunk_resp.c \
	src/kerchunk_evt_json.c \
	src/kerchunk_rrd.c

test_kerchunk_CFLAGS = $(AM_CFLAGS) \
		       $(PORTAUDIO_CFLAGS) \
		       $(CURL_CFLAGS) \
		       $(ALSA_CFLAGS)

test_kerchunk_LDADD = $(PLCODE_LIBS) $(PORTAUDIO_LIBS) $(CURL_LIBS) \
		      $(ALSA_LIBS) -lm -ldl $(PTHREAD_LIBS)

# Backward compat: 'make test' -> 'make check'
.PHONY: test
test: check

# ---------- data installs ----------

# Sounds — installed via install-data-local
soundsbasedir = $(datadir)/kerchunk/sounds

# Web dashboard — public files
kerchunkwebdir = $(datadir)/kerchunk/web
kerchunkweb_DATA = \
	web/index.html \
	web/register.html \
	web/stream-processor.js \
	web/capture-processor.js \
	web/sw.js \
	web/manifest.json \
	web/favicon.ico \
	web/logo.png

# Web dashboard — admin files
kerchunkwebadmindir = $(datadir)/kerchunk/web/admin
kerchunkwebadmin_DATA = \
	web/admin/index.html \
	web/admin/users.html \
	web/admin/config.html \
	web/admin/coverage.html \
	web/admin/ptt.html

# Config examples
kerchunkconfdir = $(sysconfdir)/kerchunk
kerchunkconf_DATA = kerchunk.conf.example users.conf.example

# Systemd service
systemddir = $(prefix)/lib/systemd/system
systemd_DATA = kerchunkd.service

# Documentation
docdir = $(datadir)/doc/kerchunk
doc_DATA = README.md ARCHITECTURE.md USAGE.md APRS.md

# ---------- install-data-local: sound files ----------

SOUND_SUBDIRS = caller numbers system time voicemail weather

install-data-local:
	@for subdir in $(SOUND_SUBDIRS); do \
	  $(MKDIR_P) "$(DESTDIR)$(soundsbasedir)/$$subdir"; \
	  for f in $(srcdir)/sounds/$$subdir/*.wav; do \
	    test -f "$$f" || continue; \
	    $(INSTALL_DATA) "$$f" "$(DESTDIR)$(soundsbasedir)/$$subdir/"; \
	  done; \
	done

uninstall-local:
	@for subdir in $(SOUND_SUBDIRS); do \
	  rm -rf "$(DESTDIR)$(soundsbasedir)/$$subdir"; \
	done
	-rmdir "$(DESTDIR)$(soundsbasedir)" 2>/dev/null || true

# ---------- extra dist ----------

EXTRA_DIST = \
	m4/ax_pthread.m4 \
	m4/ax_check_compile_flag.m4 \
	kerchunk.conf.example \
	users.conf.example \
	kerchunkd.service \
	README.md \
	ARCHITECTURE.md \
	USAGE.md \
	APRS.md \
	vendor/mongoose.c \
	vendor/mongoose.h \
	cli/linenoise.h \
	Makefile.old
