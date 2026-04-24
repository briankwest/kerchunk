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
- **libpocsag** (optional, experimental, for POCSAG paging): `github.com/briankwest/libpocsag` — detected by pkg-config
- **libflex** (optional, experimental, for FLEX paging): `github.com/briankwest/libflex` — detected by pkg-config
- **libaprs** (optional, for APRS): `github.com/briankwest/libaprs` — detected by pkg-config
- **libpoc** (optional, for PoC radio bridge): `github.com/briankwest/libpoc` — detected by pkg-config
- **libwyoming** (optional, for Wyoming TTS/ASR): `github.com/briankwest/libwyoming` — detected by pkg-config. Also provides `wyoming-tools` (server binaries), `wyoming-voice-*` (Piper voices), and `wyoming-model-*` (Sherpa-ONNX ASR models) deb packages.

## Architecture

Lightweight C11 core with event bus, dlopen'd modules (.so), outbound audio queue, Unix socket CLI. 31 modules (POCSAG and FLEX are experimental). Up to 7 threads: audio (20ms), main (20ms), web server, audio flush, TTS, NWS polling, and SDR capture. Managed thread pool (`threads_init`/`threads_shutdown`) supervises 5 module threads. Wall-clock scheduler (`sched_init`/`sched_shutdown`) provides `schedule_aligned` (drift-free periodic, used by CW ID) and `schedule_at` (future one-shot). Shutdown order: threads_shutdown, sched_shutdown, modules_shutdown. Version string includes git hash (e.g. "1.0.1+abc1234", deb package "1.0.1-1+gitabc1234").

Modules receive a `kerchunk_core_t *` vtable in their `load()` callback. They subscribe to events, queue audio, and use timers — never access hardware directly.

### Audio-thread decomposition

The audio thread's decisions live in pure functions (no I/O, no globals) so they can be unit-tested without PortAudio:

- **`kerchunk_audio_state_t`** (include/kerchunk_audio_tick.h) — all audio-thread-owned state (software_relay, tx_delay_rem, queue_ptt, relay_drain, ptt_hold_ticks, etc.) in one struct.
- **`kerchunk_audio_tick_rx()`** — descramble-already-applied → decoder reset-on-COR-assert-edge → DTMF process + event emit → relay drain-start edge → relay write decision + silence-floor early-stop. Returns events + `relay_write` intent.
- **`kerchunk_audio_tick_tx()`** — queue-pause gate → PTT assert + tx_delay setup → queue drain (1 frame/tick) → FIRE_COMPLETE at tail → tail silence → RELEASE with hold-ticks. Returns an ordered action list; shell executes each action. Multi-pass protocol (up to 3 passes/tick) handles tail-cancellation on courtesy-tone requeue.
- **`kerchunk_audio_ring_commit()`** (include/kerchunk_audio_ring.h) — lifted from `cap_cb`/`duplex_cb`. Handles `paInputUnderflow` drop + capture resample.
- **`kerchunk_audio_repeat_fill()`** — under-run fill (zero → tail-copy → tile).

See `ARCH-COR-DTMF.md` for the COR/DTMF re-architecture context and `PLAN-AUDIO-TICK.md` for the refactor history.

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

## Queue behavior

- Inter-file gap: 10ms silence between consecutive WAV files
- No preemption: while audio is playing, new items append to tail regardless of priority
- tx_delay: configurable silence after PTT assert before audio (default 100ms)
- tx_tail: configurable silence after audio before PTT release (default 200ms)

## PTT

Refcounted — multiple modules can hold PTT simultaneously. Hardware PTT releases only when all refs drop to zero. The audio thread auto-asserts PTT when draining queue audio and auto-releases when the queue empties. Modules just queue audio.

## Testing patterns

Two styles:

**Pure unit tests** (no mocks, no module includes) — drive the extracted pure functions directly. Examples: `test_txactivity.c` for the fused presence detector, `test_audio_ring.c` for the SPSC ring + PA-callback commit + repeat-fill, `test_audio_tick_rx.c` / `test_audio_tick_tx.c` for the RX/TX audio sub-ticks. Uses real libplcode encoders to generate tones.

**Integration tests** (`test_integ_*.c`) — include module `.c` files directly after redefining `KERCHUNK_MODULE_DEFINE` (see `test_integ_mock.h`). This gives access to all static globals and functions for white-box testing.

- Mock core vtable records all calls (PTT, queue, timer, log)
- `mock_fire_simple(KERCHEVT_COR_ASSERT)` simulates hardware events
- `mock_fire_dtmf('5')` simulates DTMF digit
- `mock_fire_timer(timer_id)` simulates timer expiry
- DSP tests use libplcode encoders to generate synthetic signals

Suite currently runs 318 cases across both styles.

## DTMF command registration

Add commands in `mod_dtmfcmd.c` `dtmfcmd_load()`:
```c
register_cmd("93", 8, "Weather report");  // *93# → KERCHEVT_CUSTOM + 8
```
Subscribe in the target module: `core->subscribe(KERCHEVT_CUSTOM + 8, handler, NULL);`

Current DTMF commands: `*87#` VM status, `*86#` VM record (own), `*86<id>#` VM record (for user id), `*85#` VM play, `*83#` VM delete, `*84#` VM list, `*41<pin>#` GPIO on, `*40<pin>#` GPIO off, `*93#` weather, `*94#` forecast, `*95#` time, `*911#` emergency on, `*910#` emergency off, `*88#` parrot echo, `*96#` NWS alerts, `*68<6digits>#` OTP authenticate, `*97#` scrambler toggle, `*970#` scrambler off, `*971#`-`*978#` scrambler set code, `*0<digits>#` autopatch dial, `*0#` autopatch hangup, `*99#` AI arm.

APRS beacon/status are CLI-only (`aprs beacon`, `aprs status`) — not DTMF-dialable.

19 core CLI commands: status, version, uptime, audio, hid, user, log, diag, play, tone, ptt, queue, module, event, config, sim, threads, schedule, shutdown.

Module CLI commands register their own subcommand trees: ai, aprs, asr, caller, cdr, cwid, dtmfcmd, emergency, flex, freeswitch, gpio, nws, otp, parrot, pocsag, recorder, repeater, scrambler, sdr, stats, sys, time, tones, tts, voicemail, weather, web, webhook. Example: `pocsag send|numeric|tone|status`, `aprs beacon|send|status`.

The TTS module supports two engines: ElevenLabs (cloud API via libcurl, requires `api_key`) and Wyoming (local/network via libwyoming, connects to a wyoming-piper server). Responses are cached as WAV files keyed by text hash in `<sounds_dir>/cache/tts/`.
The ASR module (mod_asr) transcribes all inbound transmissions via a Wyoming ASR server (wyoming-asr-server with Sherpa-ONNX, or wyoming-faster-whisper). Transcripts are logged and available via `asr history`.
The NWS module needs libcurl: `make modules/mod_nws.so` uses `pkg-config --libs libcurl`.

## FCC compliance

- **CW ID interval** capped at 15 min max (FCC 95.1751) in mod_cwid
- **Weather/time auto-announce** default off (FCC 95.1733 — unsolicited one-way); DTMF-triggered always works
- **Kerchunk filter**: COR debounce (`cor_debounce` config, default 150ms) in mod_repeater (filters the IDLE→RECEIVING state transition; not the same as `[txactivity] end_silence_ms` which controls the fused TX_END detector — see ARCH-COR-DTMF.md)
- **Emergency mode** (`*911#`/`*910#`): suppresses TOT and automated announcements

## Config

INI format. Modules read config in their `configure()` callback via `kerchunk_config_get(cfg, "section", "key")`. Config buffer is 1024 bytes (supports long module load lists). Key sections: `[general]`, `[modules]`, `[audio]` (includes `sample_rate` default 48000), `[hid]`, `[repeater]`, `[txactivity]` (fused presence detector — end_silence_ms, end_silence_dtmf_ms, dtmf_grace_ms, trust_cos_bit), `[dtmf]` (decoder thresholds — hits_to_begin, misses_to_end, min_off_frames), `[web]`, `[weather]`, `[time]`, `[caller]`, `[recording]`, `[emergency]`, `[tts]`, `[asr]`, `[ai]`, `[nws]`, `[otp]`, `[stats]`, `[cdr]`, `[courtesy]`, `[gpio]`, `[logger]`, `[parrot]`, `[freeswitch]`, `[scrambler]`, `[sdr]`, `[pocsag]`, `[flex]`, `[aprs]`, `[poc]`, `[webhook]`, `[group.N]`, `[user.N]`.

## User DB & Groups

Users have `username`, `email`, `group` fields. Users without a `username` in config get one auto-derived from name (lowercase, spaces→underscores). User array is dynamic (malloc/realloc, up to 9999).

## Logging

All log messages go to both stderr and `events.log` (via tee mechanism in mod_logger). Use `g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "message")`.

## Recording

mod_recorder captures RX audio via audio tap, TX audio via playback tap. Files saved as `recordings/YYYYMMDD_HHMMSS_{RX,TX}_username.wav`. Also appends to `recordings/activity.log` for FCC 95.1705 cooperative use record-keeping.

## Testing stubs

Core functions called directly by modules (`kerchunk_core_set_emergency`, `kerchunk_core_get_emergency`) are stubbed in `tests/test_stubs.c` for the test binary.
