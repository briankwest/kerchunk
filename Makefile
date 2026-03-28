CC       ?= cc
AR       ?= ar
CFLAGS    = -std=c11 -Wall -Wextra -Wpedantic -O2 \
            -Iinclude -Ilibplcode/include -D_POSIX_C_SOURCE=200809L
LDFLAGS   = -lm -ldl -lpthread

# PortAudio (required)
PA_CFLAGS := $(shell pkg-config --cflags portaudio-2.0 2>/dev/null)
PA_LIBS   := $(shell pkg-config --libs portaudio-2.0 2>/dev/null)
ifeq ($(PA_LIBS),)
  $(error portaudio-2.0 not found. Install: brew install portaudio / apt install portaudio19-dev pkg-config)
endif
CFLAGS  += $(PA_CFLAGS)
LDFLAGS += $(PA_LIBS)

# Platform
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  CFLAGS += -D_GNU_SOURCE
endif

# libplcode
PLCODE_DIR = libplcode
PLCODE_LIB = $(PLCODE_DIR)/libplcode.a

# libnemo_normalize (optional — text normalization for TTS)
NEMO_DIR = libnemo_normalize
NEMO_LIB = $(NEMO_DIR)/libnemo_normalize.so
NEMO_AVAILABLE := $(wildcard $(NEMO_DIR)/nemo_normalize.h)
ifneq ($(NEMO_AVAILABLE),)
  # Check if OpenFst is available (required to build libnemo_normalize)
  OPENFST_CHECK := $(shell pkg-config --libs fst 2>/dev/null || echo "")
  CONDA_FST := $(wildcard /opt/conda/lib/libfst.so)
  SYSTEM_FST := $(wildcard /usr/lib/*/libfst.so*)
  ifneq ($(OPENFST_CHECK)$(CONDA_FST)$(SYSTEM_FST),)
    NEMO_CFLAGS = -I$(NEMO_DIR) -DHAVE_NEMO_NORMALIZE
    NEMO_LDFLAGS = -L$(NEMO_DIR) -lnemo_normalize -Wl,-rpath,'$$ORIGIN/../$(NEMO_DIR)'
    NEMO_DEP = $(NEMO_LIB)
  endif
endif

# Daemon
DAEMON_SRC = src/main.c src/kerchunk_core.c src/kerchunk_events.c src/kerchunk_modules.c \
             src/kerchunk_queue.c src/kerchunk_audio.c src/kerchunk_hid.c src/kerchunk_socket.c \
             src/kerchunk_config.c src/kerchunk_log.c src/kerchunk_user.c src/kerchunk_wav.c \
             src/kerchunk_timer.c src/kerchunk_resp.c src/kerchunk_evt_json.c
DAEMON_OBJ = $(DAEMON_SRC:.c=.o)
DAEMON_BIN = kerchunkd

# CLI
CLI_SRC = cli/kerchunk.c cli/linenoise.c
CLI_OBJ = $(CLI_SRC:.c=.o)
CLI_BIN = kerchunk

# Modules
MOD_SRC = modules/mod_repeater.c modules/mod_cwid.c modules/mod_courtesy.c \
          modules/mod_caller.c modules/mod_dtmfcmd.c modules/mod_voicemail.c \
          modules/mod_gpio.c modules/mod_logger.c modules/mod_weather.c \
          modules/mod_time.c modules/mod_recorder.c modules/mod_txcode.c \
          modules/mod_emergency.c modules/mod_parrot.c modules/mod_cdr.c \
          modules/mod_tts.c modules/mod_nws.c modules/mod_stats.c \
          modules/mod_web.c modules/mod_otp.c modules/mod_webhook.c \
          modules/mod_scrambler.c modules/mod_sdr.c \
          modules/mod_freeswitch.c
MOD_SO  = $(MOD_SRC:.c=.so)

MOD_LDFLAGS = -shared -fPIC -lm
ifeq ($(UNAME_S),Darwin)
  MOD_LDFLAGS += -undefined dynamic_lookup
endif

# Tests
TEST_SRC  = tests/test_main.c tests/test_events.c tests/test_config.c tests/test_resp.c \
            tests/test_queue.c tests/test_repeater.c tests/test_cwid.c \
            tests/test_integration.c tests/test_integ_repeater.c \
            tests/test_integ_dtmfcmd.c tests/test_integ_caller.c \
            tests/test_integ_voicemail.c tests/test_integ_timer.c \
            tests/test_integ_userdb.c tests/test_integ_time.c \
            tests/test_integ_decode.c tests/test_integ_recorder.c \
            tests/test_integ_txcode.c tests/test_integ_emergency.c \
            tests/test_integ_parrot.c tests/test_integ_cdr.c \
            tests/test_integ_cwid.c tests/test_integ_stats.c \
            tests/test_integ_otp.c tests/test_integ_scrambler.c \
            tests/test_integ_freeswitch.c \
            tests/test_stubs.c
TEST_CORE = src/kerchunk_events.o src/kerchunk_config.o src/kerchunk_log.o \
            src/kerchunk_queue.o src/kerchunk_wav.o src/kerchunk_timer.o src/kerchunk_user.o \
            src/kerchunk_resp.o src/kerchunk_evt_json.o
TEST_OBJ  = $(TEST_SRC:.c=.o)
TEST_BIN  = test_kerchunk

.PHONY: all clean test modules daemon cli devices install tools

all: daemon cli modules

daemon: $(DAEMON_BIN)
cli: $(CLI_BIN)
modules: $(MOD_SO)

$(PLCODE_LIB):
	$(MAKE) -C $(PLCODE_DIR)

ifneq ($(NEMO_DEP),)
$(NEMO_LIB):
	$(MAKE) -C $(NEMO_DIR)
endif

# -rdynamic (Linux) / -Wl,-export_dynamic (macOS) exports symbols
# so dlopen'd modules can resolve libplcode functions from the daemon
EXPORT_FLAGS =
ifeq ($(UNAME_S),Linux)
  EXPORT_FLAGS = -rdynamic
endif
ifeq ($(UNAME_S),Darwin)
  EXPORT_FLAGS = -Wl,-export_dynamic
endif

$(DAEMON_BIN): $(DAEMON_OBJ) $(PLCODE_LIB)
	$(CC) $(CFLAGS) $(EXPORT_FLAGS) -o $@ $(DAEMON_OBJ) $(PLCODE_LIB) $(LDFLAGS)

$(CLI_BIN): $(CLI_OBJ)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJ) -lpthread

modules/%.so: modules/%.c $(PLCODE_LIB)
	$(CC) $(CFLAGS) $(MOD_LDFLAGS) -o $@ $< $(PLCODE_LIB)

# CWID module needs libcurl (solar quiet hours via sunrise-sunset.org)
modules/mod_cwid.so: modules/mod_cwid.c $(PLCODE_LIB)
	$(CC) $(CFLAGS) $(CURL_CFLAGS) $(MOD_LDFLAGS) -o $@ $< $(PLCODE_LIB) $(CURL_LIBS)

# Weather module needs libcurl
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS   := $(shell pkg-config --libs libcurl 2>/dev/null)

modules/mod_weather.so: modules/mod_weather.c $(PLCODE_LIB)
	$(CC) $(CFLAGS) $(CURL_CFLAGS) $(MOD_LDFLAGS) -o $@ $< $(PLCODE_LIB) $(CURL_LIBS)

# NWS module needs libcurl
modules/mod_nws.so: modules/mod_nws.c $(PLCODE_LIB)
	$(CC) $(CFLAGS) $(CURL_CFLAGS) $(MOD_LDFLAGS) -o $@ $< $(PLCODE_LIB) $(CURL_LIBS)

# SDR module needs librtlsdr
RTLSDR_CFLAGS := $(shell pkg-config --cflags librtlsdr 2>/dev/null)
RTLSDR_LIBS   := $(shell pkg-config --libs librtlsdr 2>/dev/null)
modules/mod_sdr.so: modules/mod_sdr.c $(PLCODE_LIB)
	$(CC) $(CFLAGS) $(RTLSDR_CFLAGS) $(MOD_LDFLAGS) -o $@ $< $(PLCODE_LIB) $(RTLSDR_LIBS)

# Webhook module needs libcurl
modules/mod_webhook.so: modules/mod_webhook.c $(PLCODE_LIB)
	$(CC) $(CFLAGS) $(CURL_CFLAGS) $(MOD_LDFLAGS) -o $@ $< $(PLCODE_LIB) $(CURL_LIBS)

# TTS module needs libcurl (ElevenLabs API) + optional libnemo_normalize
modules/mod_tts.so: modules/mod_tts.c $(PLCODE_LIB) $(NEMO_DEP)
	$(CC) $(CFLAGS) $(CURL_CFLAGS) $(NEMO_CFLAGS) $(MOD_LDFLAGS) -o $@ $< $(PLCODE_LIB) $(CURL_LIBS) $(NEMO_LDFLAGS)

# Web module — mongoose (vendored, OpenSSL TLS)
MONGOOSE_OBJ = vendor/mongoose.o
vendor/mongoose.o: vendor/mongoose.c
	$(CC) -std=c11 -Wall -O2 -fPIC -DMG_ENABLE_LINES=0 -DMG_TLS=MG_TLS_OPENSSL -Wno-maybe-uninitialized -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -c -o $@ $<

modules/mod_web.so: modules/mod_web.c $(MONGOOSE_OBJ) $(PLCODE_LIB)
	$(CC) $(CFLAGS) -Ivendor $(MOD_LDFLAGS) -o $@ $< $(MONGOOSE_OBJ) $(PLCODE_LIB) -lssl -lcrypto

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

cli/%.o: cli/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Relax warnings for vendored linenoise
cli/linenoise.o: cli/linenoise.c
	$(CC) -std=c11 -Wall -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -c -o $@ $<

tests/%.o: tests/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_BIN): $(TEST_OBJ) $(TEST_CORE) $(PLCODE_LIB)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJ) $(TEST_CORE) $(PLCODE_LIB) $(LDFLAGS) $(CURL_LIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

devices: $(DAEMON_BIN)
	./$(DAEMON_BIN) -d

PREFIX ?= /usr/local
INSTALL_DIR = $(PREFIX)/lib/kerchunk
FAR_INSTALL_DIR = $(PREFIX)/share/kerchunk/far_export

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(DAEMON_BIN) $(DESTDIR)$(PREFIX)/bin/
	install -m 755 $(CLI_BIN) $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(INSTALL_DIR)
	install -m 755 $(MOD_SO) $(DESTDIR)$(INSTALL_DIR)/
ifneq ($(NEMO_DEP),)
	install -m 755 $(NEMO_LIB) $(DESTDIR)$(INSTALL_DIR)/
	install -d $(DESTDIR)$(FAR_INSTALL_DIR)
	cp -r $(NEMO_DIR)/far_export/en_tn_grammars_cased $(DESTDIR)$(FAR_INSTALL_DIR)/
endif

# SDR tools (standalone, no daemon deps)
tools: tools/sdr_wb_capture tools/sdr_wb_replay tools/sdr_test

tools/sdr_wb_capture: tools/sdr_wb_capture.c $(PLCODE_LIB)
	$(CC) -std=c11 -Wall -Wextra -O2 -Iinclude -Ilibplcode/include -o $@ $< $(PLCODE_LIB) $(RTLSDR_LIBS) -lm

tools/sdr_wb_replay: tools/sdr_wb_replay.c $(PLCODE_LIB)
	$(CC) -std=c11 -Wall -Wextra -O2 -Iinclude -Ilibplcode/include -o $@ $< $(PLCODE_LIB) -lm

tools/sdr_test: tools/sdr_test.c $(PLCODE_LIB)
	$(CC) -std=c11 -Wall -Wextra -O2 -Iinclude -Ilibplcode/include -o $@ $< $(PLCODE_LIB) $(RTLSDR_LIBS) -lm

clean:
	rm -f $(DAEMON_OBJ) $(CLI_OBJ) $(MOD_SO) $(TEST_OBJ)
	rm -f $(DAEMON_BIN) $(CLI_BIN) $(TEST_BIN)
	rm -f src/*.o cli/*.o tests/*.o modules/*.o vendor/*.o
	rm -f tools/sdr_wb_capture tools/sdr_wb_replay tools/sdr_test
