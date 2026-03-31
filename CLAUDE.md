# CLAUDE.md — Development notes for kerchunkd

## Clone & Build

```bash
git clone git@github.com:briankwest/kerchunk.git
cd kerchunk
autoreconf -fi
./configure
make            # Build daemon, CLI, all modules
make check      # Run full test suite (must all pass)
make clean      # Remove artifacts
```

Always run `make check` after changes. If you modify `kerchunk.h` or any core header, do `make clean && make all` — stale .o files cause crashes due to struct layout changes.

### Dependencies

Libraries must be installed before building kerchunk:

- **libplcode** (required): `github.com/briankwest/libplcode` — install via `make install` or .deb package
- **libnemo_normalize** (optional): `github.com/briankwest/libnemo_normalize` — install via `make install` or .deb package
- **portaudio** (required): `apt install portaudio19-dev`
- **libcurl** (for weather/NWS/TTS modules): `apt install libcurl4-openssl-dev`
- **libssl** (for TLS): `apt install libssl-dev`
- **librtlsdr** (for SDR module): `apt install librtlsdr-dev`
- **libasound2** (for ALSA mixer): `apt install libasound2-dev`
- **libpocsag** (optional, for POCSAG paging): `github.com/briankwest/libpocsag` — detected by pkg-config
- **libflex** (optional, for FLEX paging): `github.com/briankwest/libflex` — detected by pkg-config
- **libaprs** (optional, for APRS): `github.com/briankwest/libaprs` — detected by pkg-config

## Architecture

Lightweight C11 core with event bus, dlopen'd modules (.so), outbound audio queue, Unix socket CLI. 27 modules. Up to 5 threads: audio (20ms), main (20ms), web server, TTS, and NWS polling. Version string includes git hash (e.g. "1.0.1+abc1234", deb package "1.0.1-1+gitabc1234").

Modules receive a `kerchunk_core_t *` vtable in their `load()` callback. They subscribe to events, queue audio, and use timers — never access hardware directly.

## Sound file conventions

WAV files: **16-bit, mono** (any sample rate -- the queue auto-resamples to the configured rate via `kerchunk_resample()` at load time). Organized by module under `sounds/`:

```
sounds/
├── caller/      Caller identification prompts
├── numbers/     num_0 through num_100 (number-to-speech)
├── system/      System prompts (ready, error, shutdown, etc.)
├── time/        Time announcement prompts (tm_ prefix)
├── voicemail/   Voicemail prompts (vm_ prefix)
└── weather/     Weather prompts, conditions, wind directions (wx_ prefix)
```

Module code references sounds as `<subdir>/<name>` (e.g., `speak_wav("weather/wx_clear")`, `speak_wav("numbers/num_5")`). The `sounds_dir` config provides the base path.

To generate a new WAV file:
```bash
say -o /tmp/tmp.aiff "the phrase"
ffmpeg -y -i /tmp/tmp.aiff -ar 48000 -ac 1 -sample_fmt s16 sounds/<subdir>/<name>.wav
```

**Do NOT use afconvert** — it ignores the `-r` flag and produces 22050 Hz files. While the queue will resample automatically, afconvert output quality is poor.

## Module conventions

- Static globals: `g_core`, `g_timer`, `g_config_*` etc.
- Lifecycle: `load()` → subscribe to events, `configure()` → read config, `unload()` → unsubscribe + cancel timers
- `KERCHUNK_MODULE_DEFINE(mod_name);` at the end of every module
- Modules do NOT manage PTT for queued audio — the core handles it automatically
- Queue audio at appropriate priority: emergency=10, CW ID=5, weather=3, time=2, general=0
- Modules can call `kerchunk_core_get_emergency()` / `kerchunk_core_set_emergency()` directly (not via vtable)
- TX encoder state: `kerchunk_core_set_tx_encoder()` / `kerchunk_core_get_tx_encoder()` — set by mod_txcode, mixed by audio thread

## Queue behavior

- Inter-file gap: 10ms silence between consecutive WAV files
- No preemption: while audio is playing, new items append to tail regardless of priority
- tx_delay: configurable silence after PTT assert before audio (default 100ms)
- tx_tail: configurable silence after audio before PTT release (default 200ms)

## PTT

Refcounted — multiple modules can hold PTT simultaneously. Hardware PTT releases only when all refs drop to zero. The audio thread auto-asserts PTT when draining queue audio and auto-releases when the queue empties. Modules just queue audio.

## Testing patterns

Integration tests include module `.c` files directly after redefining `KERCHUNK_MODULE_DEFINE` (see `test_integ_mock.h`). This gives access to all static globals and functions for white-box testing.

- Mock core vtable records all calls (PTT, queue, timer, log)
- `mock_fire_simple(KERCHEVT_COR_ASSERT)` simulates hardware events
- `mock_fire_dtmf('5')` simulates DTMF digit
- `mock_fire_timer(timer_id)` simulates timer expiry
- DSP tests use libplcode encoders to generate synthetic signals

## DTMF command registration

Add commands in `mod_dtmfcmd.c` `dtmfcmd_load()`:
```c
register_cmd("93", 8, "Weather report");  // *93# → KERCHEVT_CUSTOM + 8
```
Subscribe in the target module: `core->subscribe(KERCHEVT_CUSTOM + 8, handler, NULL);`

Current DTMF commands: `*87#` VM status, `*86#` VM record (own), `*86<id>#` VM record (for user id), `*85#` VM play, `*83#` VM delete, `*84#` VM list, `*41<pin>#` GPIO on, `*40<pin>#` GPIO off, `*93#` weather, `*94#` forecast, `*95#` time, `*911#` emergency on, `*910#` emergency off, `*88#` parrot echo, `*96#` NWS alerts, `*68<6digits>#` OTP authenticate, `*97#` scrambler toggle, `*970#` scrambler off, `*971#`-`*978#` scrambler set code, `*0<digits>#` autopatch dial, `*0#` autopatch hangup, `*98#` APRS force beacon, `*980#` APRS status.

17 core CLI commands: status, help, version, uptime, audio, hid, user, log, diag, play, tone, sim, tts, cwid, caller, emergency, dtmfcmd.

Module CLI commands: pocsag (send/numeric/tone/status), flex (send/numeric/tone/status), aprs (beacon/send/status).

The TTS module uses ElevenLabs API via libcurl. Requires `api_key` in `[tts]` config. Responses are cached as WAV files keyed by text hash in `<sounds_dir>/cache/tts/`.
The NWS module needs libcurl: `make modules/mod_nws.so` uses `pkg-config --libs libcurl`.

## FCC compliance

- **CW ID interval** capped at 15 min max (FCC 95.1751) in mod_cwid
- **Weather/time auto-announce** default off (FCC 95.1733 — unsolicited one-way); DTMF-triggered always works
- **Kerchunk filter**: COR debounce (`cor_debounce` config, default 150ms) in mod_repeater
- **Emergency mode** (`*911#`/`*910#`): suppresses TOT and automated announcements

## Config

INI format. Modules read config in their `configure()` callback via `kerchunk_config_get(cfg, "section", "key")`. Config buffer is 1024 bytes (supports long module load lists). Key sections: `[general]`, `[modules]`, `[audio]` (includes `sample_rate` default 48000, `tx_encode` default off), `[repeater]`, `[web]`, `[weather]`, `[time]`, `[caller]`, `[recording]`, `[emergency]`, `[tts]`, `[nws]`, `[otp]`, `[stats]`, `[freeswitch]`, `[scrambler]`, `[sdr]`, `[pocsag]`, `[flex]`, `[aprs]`, `[group.N]`, `[user.N]`.

## User DB & Groups

Users have `username`, `email`, `group` fields. Groups have `tx_ctcss`, `tx_dcs`. TX tone resolution: group → repeater default (no per-user tone override). Use `kerchunk_user_lookup_group_tx()` to resolve. Users without a `username` in config get one auto-derived from name (lowercase, spaces→underscores). User array is dynamic (malloc/realloc, up to 9999).

## Logging

All log messages go to both stderr and `events.log` (via tee mechanism in mod_logger). Use `g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "message")`.

## Recording

mod_recorder captures RX audio via audio tap, TX audio via playback tap. Files saved as `recordings/YYYYMMDD_HHMMSS_{RX,TX}_username.wav`. Also appends to `recordings/activity.log` for FCC 95.1705 cooperative use record-keeping.

## Testing stubs

Core functions called directly by modules (`kerchunk_core_set_emergency`, `kerchunk_core_get_emergency`, `kerchunk_core_set_tx_encoder`, `kerchunk_core_get_tx_encoder`) are stubbed in `tests/test_stubs.c` for the test binary.
