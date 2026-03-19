# CLAUDE.md — Development notes for kerchunkd

## Clone & Build

```bash
git clone --recurse-submodules git@github.com:briankwest/kerchunk.git
cd kerchunk

# If you already cloned without --recurse-submodules:
git submodule update --init

make            # Build libplcode, daemon, CLI, all modules
make test       # Run full test suite (204 tests, must all pass)
make clean      # Remove artifacts (does NOT clean libplcode)
make modules    # Build just the .so modules
```

The `libplcode/` directory is a git submodule (`github.com/briankwest/libplcode`). The Makefile builds it automatically via `make -C libplcode`. To fully clean both:

```bash
make -C libplcode clean && make clean
```

Always run `make test` after changes. If you modify `kerchunk.h` or any core header, do `make clean && make all` — stale .o files cause crashes due to struct layout changes.

### Dependencies

- **portaudio** (required): `brew install portaudio` / `apt install portaudio19-dev`
- **libcurl** (for weather/NWS/TTS modules): `brew install curl` / `apt install libcurl4-openssl-dev`

## Architecture

Lightweight C11 core with event bus, dlopen'd modules (.so), outbound audio queue, Unix socket CLI. Up to 5 threads: audio (20ms), main (20ms), web server, TTS, and NWS polling.

Modules receive a `kerchunk_core_t *` vtable in their `load()` callback. They subscribe to events, queue audio, and use timers — never access hardware directly.

## Sound file conventions

WAV files: **8kHz, 16-bit, mono**. Organized by module under `sounds/`:

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
ffmpeg -y -i /tmp/tmp.aiff -ar 8000 -ac 1 -sample_fmt s16 sounds/<subdir>/<name>.wav
```

**Do NOT use afconvert** — it ignores the `-r` flag and produces 22050 Hz files that play as garbled chipmunk audio at 8kHz.

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

Current DTMF commands: `*87#` VM status, `*86#` VM record (own), `*86<id>#` VM record (for user id), `*85#` VM play, `*83#` VM delete, `*84#` VM list, `*41<pin>#` GPIO on, `*40<pin>#` GPIO off, `*93#` weather, `*94#` forecast, `*95#` time, `*911#` emergency on, `*910#` emergency off, `*88#` parrot echo, `*96#` NWS alerts, `*68<6digits>#` OTP authenticate, `*97#` scrambler toggle, `*970#` scrambler off, `*971#`-`*978#` scrambler set code.

The TTS module uses ElevenLabs API via libcurl. Requires `api_key` in `[tts]` config. Responses are cached as WAV files keyed by text hash in `<sounds_dir>/cache/tts/`.
The NWS module needs libcurl: `make modules/mod_nws.so` uses `pkg-config --libs libcurl`.

## FCC compliance

- **CW ID interval** capped at 15 min max (FCC 95.1751) in mod_cwid
- **Weather/time auto-announce** default off (FCC 95.1733 — unsolicited one-way); DTMF-triggered always works
- **Kerchunk filter**: COR debounce (`cor_debounce` config, default 150ms) in mod_repeater
- **Emergency mode** (`*911#`/`*910#`): suppresses TOT and automated announcements

## Config

INI format. Modules read config in their `configure()` callback via `kerchunk_config_get(cfg, "section", "key")`. Key sections: `[general]`, `[modules]`, `[audio]`, `[repeater]`, `[web]`, `[weather]`, `[time]`, `[caller]`, `[recording]`, `[emergency]`, `[tts]`, `[nws]`, `[otp]`, `[stats]`, `[group.N]`, `[user.N]`.

## User DB & Groups

Users have `username`, `email`, `group` fields. Groups have `tx_ctcss`, `tx_dcs`. TX tone resolution: group → repeater default (no per-user tone override). Use `kerchunk_user_lookup_group_tx()` to resolve. Users without a `username` in config get one auto-derived from name (lowercase, spaces→underscores). User array is dynamic (malloc/realloc, up to 9999).

## Logging

All log messages go to both stderr and `events.log` (via tee mechanism in mod_logger). Use `g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "message")`.

## Recording

mod_recorder captures RX audio via audio tap, TX audio via playback tap. Files saved as `recordings/YYYYMMDD_HHMMSS_{RX,TX}_username.wav`. Also appends to `recordings/activity.log` for FCC 95.1705 cooperative use record-keeping.

## Testing stubs

Core functions called directly by modules (`kerchunk_core_set_emergency`, `kerchunk_core_get_emergency`, `kerchunk_core_set_tx_encoder`, `kerchunk_core_get_tx_encoder`) are stubbed in `tests/test_stubs.c` for the test binary.
