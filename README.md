<p align="center">
  <img src="web/logo.png" alt="kerchunkd logo" width="400">
</p>

# kerchunkd — GMRS/Amateur Repeater Controller Daemon

A custom repeater controller for the Retevis RT97L GMRS repeater, built in C11 for Raspberry Pi and Linux/macOS. Interfaces with the repeater via its DB9 accessory port through a RIM-Lite v2 USB radio interface (CM119 chipset). All CTCSS/DCS/DTMF decoding and CW ID generation is handled in software using [libplcode](https://github.com/briankwest/libplcode).

**20 modules** — repeater state machine, CW ID, caller identification, DTMF commands, voicemail, weather, time, NWS alerts, TTS (ElevenLabs), parrot/echo, CDR, statistics, recording, TX encoding, emergency mode, OTP authentication, courtesy tones, GPIO, logging, web dashboard.

**185 tests** — unit + integration test coverage with mock core vtable.

**Native JSON API** — every CLI command returns structured JSON via `kerchunk -j`. Event streaming via `kerchunk -e -j` (NDJSON). Response system (`kerchunk_resp_t`) provides both formats from a single handler.

**Web Dashboard** — embedded HTTP server with real-time SSE event stream, served at `http://localhost:8080`. Four pages: Dashboard, Users, Config, and GMRS Coverage Planner.

## Architecture

Modular design: a lightweight core with an event bus, dynamically loadable modules (`.so`), an outbound audio queue with priority, an interactive CLI, and an embedded web server.

```
+---------------------------------------------------------------+
|                       kerchunkd (daemon)                      |
|                                                               |
|  +----------+  +----------+  +----------+  +----------+       |
|  | PortAudio|  | HID      |  | Event    |  | Module   |       |
|  | Audio    |  | COR/PTT  |  | Bus      |  | Loader   |       |
|  | (callback|  | (hidraw) |  | (mutex)  |  | (dlopen) |       |
|  | +ring buf|  |          |  |          |  |          |       |
|  +----+-----+  +----+-----+  +----+-----+  +----+-----+       |
|       |             |             |             |             |
|  +----+-------------+-------------+-------------+--------+    |
|  |              DSP Pipeline (libplcode)                 |    |
|  |  CTCSS dec . DCS dec . DTMF dec . CW ID enc . Tones   |    |
|  +-------------------------------------------------------+    |
|                                                               |
|  +----------+  +----------+  +----------+                     |
|  | Outbound |  | Control  |  | HTTP/SSE |                     |
|  | Queue    |  | Socket   |  | mod_web  |                     |
|  | (priority|  | (per-    |  | (port    |                     |
|  |  sorted) |  |  client) |  |   8080)  |                     |
|  +----------+  +----------+  +----------+                     |
|                                                               |
|  +----------------------------------------------------------+ |
|  |                  Loaded Modules (20)                     | |
|  |                                                          | |
|  |  mod_repeater   RX state machine (IDLE/RECV/TAIL/HANG)   | |
|  |  mod_cwid       Morse CW ID + voice ID via TTS           | |
|  |  mod_courtesy   Courtesy tone on COR drop                | |
|  |  mod_caller     Caller ID (DTMF ANI / DTMF login)        | |
|  |  mod_dtmfcmd    DTMF command router (*XX#)               | |
|  |  mod_voicemail  Record/play/delete voice messages        | |
|  |  mod_gpio       GPIO relay control via DTMF              | |
|  |  mod_logger     Event logging + rotation                 | |
|  |  mod_weather    Weather via weatherapi.com + TTS         | |
|  |  mod_time       Time announcements via TTS               | |
|  |  mod_recorder   Per-transmission WAV recording           | |
|  |  mod_txcode     Dynamic TX CTCSS/DCS encoding            | |
|  |  mod_emergency  Emergency mode (*911#/*910#)             | |
|  |  mod_otp        TOTP authentication (*68<code>#)         | |
|  |  mod_parrot     Echo/parrot for audio quality check      | |
|  |  mod_cdr        Call detail records (daily CSV)          | |
|  |  mod_tts        Text-to-speech (ElevenLabs API)          | |
|  |  mod_nws        NWS weather alert monitor                | |
|  |  mod_stats      Statistics, metrics, persistence         | |
|  |  mod_web        HTTP server + SSE + web dashboard        | |
|  +----------------------------------------------------------+ |
+---------------------------------------------------------------+
```

### Threading Model

- **Audio thread** (20ms) — captures audio, runs libplcode decoders (CTCSS/DCS/DTMF), software relay with drain, drains outbound queue, mixes TX CTCSS/DCS tones into all outgoing audio, manages queue-driven PTT
- **Main thread** (20ms) — processes timers, polls control socket, handles COR/PTT, config reloads
- **Web thread** — accepts HTTP connections, serves API/SSE/static files
- **TTS thread** — async ElevenLabs API calls (non-blocking)
- **NWS thread** — async weather alert polling (non-blocking)

#### Thread Safety

- **Config access** — protected by `pthread_mutex` (`kerchunk_core_lock_config` / `kerchunk_core_unlock_config`) for safe reads/writes across threads
- **Audio ring buffer** — lock-free with `atomic_size_t` head/tail pointers for producer/consumer between PortAudio callback and main thread
- **Audio taps** — mutex-protected tap registration/unregistration with snapshot pattern to avoid holding locks during callbacks
- **Control socket** — per-client mutex protects concurrent writes and deferred event flushing
- **Signal handling** — `volatile sig_atomic_t` for `g_running` and `g_reload` flags

### Dual State Machines

The repeater tracks two independent state machines:

**RX (Inbound) — what the repeater is hearing:**

```
                 COR assert
        +----------+----------+
        |          |          |
        v          |          |
     +------+   (debounce)    |
     | IDLE |----->+          |
     +------+      |          |
        ^          v          |
        |    +-----------+    |
        |    | RECEIVING |<---+ COR re-assert (rekey)
        |    +-----------+    |
        |         |           |
        |     COR drop        |
        |         |           |
        |         v           |
        |    +-----------+    |
        |    | TAIL_WAIT |----+ COR re-assert (rekey)
        |    +-----------+
        |         |
        |     tail expires
        |         |
        |         v
        |    +-----------+
        |    | HANG_WAIT |----+ COR re-assert (rekey)
        |    +-----------+
        |         |
        |     hang expires
        |         |
        +---------+

     TIMEOUT: fires after timeout_time in RECEIVING
```

**TX (Outbound) — what the repeater is transmitting:**

```
     +---------+
     | TX_IDLE |  Not transmitting
     +---------+
          |
          +--- COR assert (software_relay=on) ----+
          |                                       v
          |                                  +---------+
          +--- Queue has items               | TX_RELAY|  Relaying RX audio
          |                                  +---------+
          v                                       |
     +---------+                              COR drop
     | TX_QUEUE|  Playing queued audio            |
     +---------+  (TTS, weather, CW ID)           v
          |                                  +---------+
          +--- queue empties                 | TX_TAIL |  Drain + tail silence
          |                                  +---------+
          v                                       |
     +---------+                              PTT drop
     | TX_TAIL |  TX tail silence (CTCSS)         |
     +---------+                                  v
          |                                  +---------+
      PTT drop                               | TX_IDLE |
          |                                  +---------+
          v
     +---------+
     | TX_IDLE |
     +---------+
```

Both state machines are visible in the web dashboard and reported via `/api/status`:

```json
{"rx_state":"RECEIVING","tx_state":"TX_RELAY","ptt":true,"cor":true,...}
```

**RX Timers:**
- **Tail** (`tail_time`, default 2s) — silence after COR drop, courtesy tone plays
- **Hang** (`hang_time`, default 500ms) — PTT held after tail for quick rekey
- **TOT** (`timeout_time`, default 180s) — max continuous receive
- **Debounce** (`cor_debounce`, default 150ms) — kerchunk filter

**TX Timers:**
- **TX delay** (`tx_delay`, default 100ms) — silence after PTT assert (skipped if PTT already held)
- **TX tail** (`tx_tail`, default 200ms) — silence after audio, CTCSS continues
- **Relay drain** (`relay_drain`, default 500ms) — continue relaying after COR drops

### Core Audio Engine

- **Software relay** — when enabled, kerchunkd captures RX audio and retransmits in software with TX CTCSS/DCS mixed in. Live voice preempts queued announcements.
- **Relay drain** — on COR drop, relay continues for configurable period (default 500ms) to avoid cutting speech mid-word. Playback ring also fully drains before PTT releases.
- **PTT refcounting** — multiple modules can hold PTT simultaneously; hardware releases only when all refs drop to zero
- **Queue auto-PTT** — audio thread asserts PTT when draining, releases when empty
- **CTCSS/DCS continuous** — tone mixed into TX delay, all audio, TX tail, and relay
- **Hardware rate adaptation** — `hw_rate` forces PortAudio to device-native sample rate (e.g. 48kHz for USB), with software resampling to/from 8kHz
- **Full-duplex stream** — single PortAudio stream when capture and playback are the same device (shared clock, no drift)

## Hardware

```
RT97L DB9 <--> RIM-Lite v2 (CM119 USB) <--> Raspberry Pi / Linux / Mac
                                             +-- PortAudio: audio I/O
                                             +-- HID: COR input / PTT output
```

| RIM-Lite | Signal | RT-97S | Notes |
|----------|--------|--------|-------|
| 2 | TX Voice Audio -> | 2 | < 100mV |
| 3 | <- COS in | 3 | Active Low |
| 5 | PTT -> | 9 | Active Low |
| 6 | <- RX Audio | 5 | De-emph. discriminator |
| 8,9 | Ground | 7 | |

## Dependencies

| Dependency | Purpose | Install (macOS) | Install (Linux) |
|-----------|---------|-----------------|-----------------|
| [libplcode](https://github.com/briankwest/libplcode) | CTCSS/DCS/DTMF/CWID codec | git submodule | git submodule |
| PortAudio | Audio I/O | `brew install portaudio` | `apt install portaudio19-dev` |
| libcurl | HTTP (weather, NWS, TTS) | `brew install curl` | `apt install libcurl4-openssl-dev` |
| pkg-config | Build system | (included with Xcode) | `apt install pkg-config` |

## Building

```bash
git clone --recurse-submodules https://github.com/briankwest/kerchunk.git
cd kerchunk

# If you already cloned without --recurse-submodules:
git submodule update --init

make            # Build libplcode, daemon, CLI, all modules
make test       # Run test suite (185 tests)
make devices    # List audio devices
make clean      # Remove artifacts
```

Build outputs:
- `kerchunkd` — the daemon
- `kerchunk` — interactive CLI
- `modules/*.so` — 20 loadable modules
- `test_kerchunk` — test suite

### Linux Setup

On a fresh Linux install (Ubuntu/Debian), install all build dependencies:

```bash
sudo apt install build-essential pkg-config portaudio19-dev libcurl4-openssl-dev
```

**Audio group** — PortAudio needs access to ALSA devices (`/dev/snd/*`), which are owned by the `audio` group:

```bash
sudo usermod -aG audio $USER
# Log out and back in for the group change to take effect
```

**USB radio interface (RIM-Lite / CM119)** — the HID device (`/dev/hidraw*`) used for COR/PTT is only accessible by root by default. Install the included udev rule:

```bash
sudo cp 99-rimlite.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=hidraw
```

This grants `audio` group access to the C-Media CM119 HID interface (vendor `0d8c`, product `013a`).

Without the audio group membership, PortAudio will report 0 devices and the daemon will run without audio. Without the udev rule, HID access requires running as root.

## Running

```bash
./kerchunkd -d                     # List audio devices
./kerchunkd -c kerchunk.conf -f    # Start in foreground
```

Web dashboard: `http://localhost:8080` (when `[web] enabled = on`)

## CLI

### Interactive Console

```bash
./kerchunk                         # Enter interactive mode
```

Features: tab completion, command history, live log streaming, auto-reconnect.

```
kerchunk> status                   # Daemon status (RX/TX state, PTT, COR)
kerchunk> help                     # Show all commands
kerchunk> /log debug               # Start log streaming
kerchunk> /nolog                   # Stop log streaming
kerchunk> exit                     # Exit console
```

### One-shot and Scripting

```bash
./kerchunk status                  # One-shot command
./kerchunk -x 'sim dtmf *95#'     # Execute from script
./kerchunk -x 'tts say hello'     # TTS from cron
```

### JSON Output and Event Streaming

```bash
./kerchunk -j status
{"rx_state":"IDLE","tx_state":"TX_IDLE","ptt":false,"cor":false,"queue":0,"modules":20,"users":2,"emergency":false}

./kerchunk -j stats | jq .channel.duty_pct

./kerchunk -e -j                   # Structured event stream (NDJSON)
./kerchunk -e -j | jq 'select(.type=="state_change")'
```

### Simulation Commands

```
kerchunk> sim cor on               # Simulate COR assert
kerchunk> sim cor off              # Simulate COR drop
kerchunk> sim dtmf *95#            # Simulate DTMF sequence
kerchunk> sim tx sounds/test.wav   # Queue a WAV file
```

## DTMF Commands

| Sequence | Action | Module |
|----------|--------|--------|
| `*87#` | Voicemail status | mod_voicemail |
| `*86#` | Record voicemail (own mailbox) | mod_voicemail |
| `*86<id>#` | Record voicemail for user ID | mod_voicemail |
| `*85#` | Voicemail play | mod_voicemail |
| `*84#` | Voicemail list | mod_voicemail |
| `*83#` | Voicemail delete | mod_voicemail |
| `*41<pin>#` | GPIO on | mod_gpio |
| `*40<pin>#` | GPIO off | mod_gpio |
| `*93#` | Current weather | mod_weather |
| `*94#` | Weather forecast | mod_weather |
| `*95#` | Time check | mod_time |
| `*911#` | Emergency mode on | mod_emergency |
| `*910#` | Emergency mode off | mod_emergency |
| `*88#` | Parrot/echo mode | mod_parrot |
| `*96#` | NWS weather alerts | mod_nws |
| `*68<code>#` | OTP authenticate (6-digit TOTP) | mod_otp |

## Modules

### mod_repeater — RX State Machine

Controls the IDLE/RECEIVING/TAIL_WAIT/HANG_WAIT/TIMEOUT RX state machine and closed repeater access control.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `tail_time` | ms | `2000` | Tail timer after COR drop |
| `hang_time` | ms | `500` | Hang timer (PTT held for quick rekey) |
| `timeout_time` | ms | `180000` | Time-out timer (3 min max) |
| `cor_debounce` | ms | `150` | Kerchunk filter (0 to disable) |
| `tx_delay` | ms | `100` | Silence after PTT assert before audio |
| `tx_tail` | ms | `200` | Silence after audio before PTT release |
| `software_relay` | on/off | `off` | Relay RX audio to TX in software |
| `relay_drain` | ms | `500` | Continue relaying after COR drop (0-5000) |
| `require_identification` | on/off | `off` | Closed repeater: deny unless identified |
| `tx_ctcss` | int | `0` | Default TX CTCSS (freq x10) |
| `tx_dcs` | int | `0` | Default TX DCS code |
| `voice_id` | on/off | `on` | Speak frequency/PL via TTS after CW ID |

Config section: `[repeater]`

### mod_cwid — CW Callsign Identification

Morse CW ID + voice ID ("WRDP519 repeater, 462.550, PL 131.8") via TTS. Sends immediately when idle, defers when busy. Interval capped at 15 min (FCC 95.1751).

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cwid_interval` | ms | `600000` | ID interval (capped at 900000) |
| `cwid_wpm` | int | `20` | Words per minute (min 5) |
| `cwid_freq` | Hz | `800` | Tone frequency |

Config section: `[repeater]`. Callsign from `[general] callsign`. Frequency from `[general] frequency`.

### mod_courtesy — Courtesy Tone

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `freq` | Hz | `800` | Tone frequency |
| `duration` | ms | `100` | Tone duration |
| `amplitude` | int | `4000` | Amplitude (0-32767) |

Config section: `[courtesy]`

### mod_caller — Caller Identification

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `methods` | string | | Comma-separated: `dtmf_ani,dtmf_login` |
| `ani_window` | ms | `500` | Window after COR for ANI digits |
| `login_timeout` | ms | `1800000` | Login session timeout (30 min) |

Config section: `[caller]`

### mod_dtmfcmd — DTMF Command Router

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `inter_digit_timeout` | ms | `3000` | Reset timeout between digits |
| `cor_gate_ms` | ms | `200` | Suppress DTMF during squelch transients (0 to disable) |

Config section: `[dtmf]`

### mod_voicemail — Voicemail

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | on/off | `off` | Enable voicemail |
| `voicemail_dir` | string | `/var/lib/kerchunk/voicemail` | Storage directory |
| `max_messages` | int | `20` | Max per user |
| `max_duration` | s | `60` | Max recording length |

Config section: `[voicemail]`

### mod_weather — Weather Announcements

Uses TTS ("Current weather. Partly cloudy. Temperature 72 degrees. Wind from the south at 12 miles per hour.") with WAV fallback.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `api_key` | string | | weatherapi.com API key |
| `location` | string | | ZIP code or city |
| `interval` | ms | `1800000` | Fetch/announce interval |
| `auto_announce` | on/off | `off` | Periodic (FCC 95.1733) |
| `announce_temp` | on/off | `on` | Include temperature |
| `announce_conditions` | on/off | `on` | Include conditions |
| `announce_wind` | on/off | `on` | Include wind |

Config section: `[weather]`

### mod_time — Time Announcements

Uses TTS ("The time is 2:30 PM central.") with WAV fallback.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | on/off | `off` | Periodic auto-announce |
| `interval` | ms | `900000` | Interval |
| `timezone` | string | | `central`, `eastern`, `mountain`, `pacific` |

Config section: `[time]`

### mod_recorder — Transmission Recording

Records RX (per COR cycle) and TX (per queue drain) to timestamped WAV files.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | on/off | `off` | Enable recording |
| `directory` | string | `recordings` | Output directory |
| `max_duration` | s | `300` | Max recording length |

Config section: `[recording]`

### mod_txcode — Dynamic TX Encoding

TX CTCSS/DCS per caller. Resolution: user -> group -> repeater default.

### mod_emergency — Emergency Mode

`*911#` activates, `*910#` deactivates. Suppresses TOT and auto-announcements.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `timeout` | ms | `1800000` | Auto-deactivate timeout |

Config section: `[emergency]`

### mod_otp — TOTP Authentication

RFC 6238 TOTP with embedded SHA-1/HMAC-SHA1 (no external crypto dependencies). Users dial `*68<6-digit code>#` to authenticate via Google Authenticator, Authy, or any TOTP app. Grants time-limited elevated access for privileged commands.

Other modules gate commands via `kerchunk_core_get_otp_elevated(user_id)`.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `session_timeout` | ms | `120000` | Elevated session duration (2 min) |
| `time_skew` | int | `1` | Accept +/- N time steps (each 30s) |

Config section: `[otp]`

User config: add `totp_secret = <base32 key>` to `[user.N]` sections.

### mod_parrot — Echo/Parrot

`*88#` arms. Records next transmission (max 10s), plays back for audio quality check.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `max_duration` | s | `10` | Max recording (capped at 30) |

Config section: `[parrot]`

### mod_cdr — Call Detail Records

Daily CSV files with caller, method, duration, emergency flag, recording path.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `directory` | string | `cdr` | Output directory |

Config section: `[cdr]`

### mod_tts — Text-to-Speech (ElevenLabs)

Async worker thread. Posts to ElevenLabs, receives PCM at 16kHz, decimates to 8kHz. Responses cached as WAV files keyed by text hash in `<sounds_dir>/cache/tts/`. Use `tts cache-clear` to flush.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `api_key` | string | | ElevenLabs API key |
| `voice_id` | string | `21m00Tcm4TlvDq8ikWAM` | Voice ID |
| `model` | string | `eleven_turbo_v2_5` | Model ID |

Config section: `[tts]`

### mod_nws — NWS Weather Alert Monitor

Polls api.weather.gov, tracks alerts by ID, announces via TTS. EAS-style tones for extreme.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | on/off | `off` | Enable monitoring |
| `latitude` | float | | Location latitude |
| `longitude` | float | | Location longitude |
| `contact` | string | | Email for User-Agent |
| `poll_interval` | ms | `300000` | Poll interval (5 min) |
| `reannounce_interval` | ms | `900000` | Re-announce (15 min) |
| `min_severity` | string | `moderate` | Minimum severity |
| `auto_announce` | on/off | `on` | Auto-announce new alerts |
| `attention_tones` | on/off | `on` | EAS tones for extreme |

Config section: `[nws]`

### mod_stats — Statistics and Metrics

Channel, per-user, and system metrics. 24h histogram. Persistence across restarts.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `persist` | on/off | `off` | Save to disk on shutdown |
| `persist_file` | string | `stats.dat` | Persistence file |

Config section: `[stats]`. CLI: `stats`, `stats user <name>`, `stats reset`, `stats save`

### mod_web — Web Dashboard

Embedded HTTP server with JSON API, SSE event stream, and static file serving. Serves four pages: Dashboard (real-time status and event stream), Users (user/group management), Config (live config editor with reload), and Coverage (GMRS coverage planner with terrain analysis).

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | on/off | `off` | Enable web server |
| `port` | int | `8080` | Listen port |
| `bind` | string | `127.0.0.1` | Bind address (0.0.0.0 for external) |
| `auth_token` | string | | Bearer token for API auth |
| `static_dir` | string | | Path to HTML/JS/CSS files |
| `tls_cert` | string | | Path to TLS certificate (PEM). Enables HTTPS |
| `tls_key` | string | | Path to TLS private key (PEM). Required with tls_cert |

Config section: `[web]`

API: `GET /api/status`, `GET /api/stats`, `GET /api/nws`, `GET /api/users`, `GET /api/events` (SSE), `POST /api/cmd`, `POST /api/config/reload`

### mod_gpio — GPIO Relay Control

DTMF `*41<pin>#` on, `*40<pin>#` off. Only pins listed in `allowed_pins` can be controlled. All GPIO pins are 3.3V logic — use a relay board or transistor driver for loads requiring more current.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `allowed_pins` | string | | Comma-separated GPIO pins |

Config section: `[gpio]`

**Raspberry Pi recommended GPIO pins:**

| GPIO | Header Pin | Notes |
|------|-----------|-------|
| 5 | 29 | General purpose |
| 6 | 31 | General purpose |
| 13 | 33 | General purpose, PWM capable |
| 16 | 36 | General purpose |
| 17 | 11 | General purpose (default in example config) |
| 19 | 35 | General purpose |
| 20 | 38 | General purpose |
| 21 | 40 | General purpose |
| 22 | 15 | General purpose (default in example config) |
| 26 | 37 | General purpose |
| 27 | 13 | General purpose (default in example config) |

**Pins to avoid:** GPIO 0-1 (HAT EEPROM I2C), 2-3 (I2C with pull-ups), 4 (1-Wire), 7-11 (SPI), 14-15 (UART serial console).

COR/PTT uses the CM119 USB HID interface, not GPIO — no pin conflicts.

### mod_logger — Event Logger

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `file` | string | `events.log` | Log file path |
| `max_size_mb` | int | `10` | Rotation threshold (MB) |

Config section: `[logger]`

## General Config

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `callsign` | string | | FCC callsign for CW ID |
| `frequency` | string | | Output frequency (for display/voice ID) |
| `offset` | string | | Input offset (for display) |
| `sample_rate` | int | `8000` | Audio sample rate |
| `log_level` | string | `info` | `error`, `warn`, `info`, `debug` |
| `sounds_dir` | string | `./sounds` | WAV file base path |
| `socket_path` | string | `/tmp/kerchunk.sock` | CLI socket path |
| `pid_file` | string | `/tmp/kerchunkd.pid` | PID file (prevents duplicate instances) |
| `users_file` | string | | Separate user/group database file (see below) |
| `address` | string | | Site street address (for dashboard display) |
| `latitude` | float | | Site latitude (decimal degrees, for coverage planner) |
| `longitude` | float | | Site longitude (decimal degrees, for coverage planner) |
| `elevation` | int | | Site elevation (ft ASL, for coverage planner) |
| `google_maps_api_key` | string | | Google Maps API key (for coverage planner map) |

Config section: `[general]`

## Audio Config

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `capture_device` | string | `default` | PortAudio capture device |
| `playback_device` | string | `default` | PortAudio playback device |
| `hw_rate` | int | `0` | Force hardware sample rate (0=auto, 48000 recommended for USB) |
| `preemphasis` | on/off | `off` | Pre-emphasis filter |
| `preemphasis_alpha` | float | `0.95` | Pre-emphasis filter coefficient |

Config section: `[audio]`

## HID Config

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `device` | string | `/dev/hidraw0` | HID device path |
| `cor_bit` | int | `0` | GPIO bit for COR input (0-7) |
| `cor_polarity` | string | `active_low` | `active_low` or `active_high` |
| `ptt_bit` | int | `2` | GPIO bit for PTT output (0-7) |

Config section: `[hid]`

## User and Group Database

Users and groups can be defined in the main `kerchunk.conf` or in a separate file via `users_file` in `[general]`. When `users_file` is set (e.g., `users_file = users.conf`), all `[user.N]` and `[group.N]` sections are loaded from that file instead. The web UI writes changes to the separate file only, keeping the main config untouched.

```ini
[group.1]
name = Family
tx_ctcss = 1000          # 100.0 Hz

[user.1]
name = Brian
ctcss = 1000             # Identify by CTCSS 100.0 Hz
dtmf_login = 101         # Or dial *101#
access = 2               # admin
voicemail = 1
group = 1
tx_ctcss = 1000          # Per-user TX override
totp_secret = JBSWY3DPEHPK3PXP  # Base32 TOTP secret (for mod_otp)
```

TX tone resolution: user `tx_ctcss`/`tx_dcs` -> group -> repeater default.

See `kerchunk.conf.example` for complete annotated reference.

## GMRS Coverage Planner

The web dashboard includes a GMRS coverage planner at `web/coverage.html`. Uses site location (`latitude`, `longitude`, `elevation` from `[general]`) and Google Maps API (`google_maps_api_key`) to calculate and display estimated RF coverage with terrain analysis. Accessible from the Coverage tab in the web dashboard.

## Event Types

| Event | Fired when |
|-------|-----------|
| `COR_ASSERT` | Carrier detected |
| `COR_DROP` | Carrier lost |
| `PTT_ASSERT` | Transmitter keyed |
| `PTT_DROP` | Transmitter unkeyed |
| `STATE_CHANGE` | RX state transition |
| `TAIL_START` | Tail timer started |
| `TAIL_EXPIRE` | Tail timer expired |
| `TIMEOUT` | TOT fired |
| `CALLER_IDENTIFIED` | Caller identified |
| `CALLER_CLEARED` | Caller cleared |
| `CTCSS_DETECT` | CTCSS tone detected/lost |
| `DCS_DETECT` | DCS code detected/lost |
| `DTMF_DIGIT` | DTMF digit onset |
| `DTMF_END` | DTMF digit released |
| `QUEUE_DRAIN` | TX queue playback started |
| `QUEUE_COMPLETE` | TX queue empty, tail starts |
| `RECORDING_SAVED` | Recording WAV saved |
| `CONFIG_RELOAD` | Config file reloaded |
| `SHUTDOWN` | Daemon shutting down |
| `TICK` | Main loop tick (20ms) |
| `AUDIO_FRAME` | 20ms audio frame captured |

## FCC Compliance

- **CW ID** — interval capped at 15 min max (FCC 95.1751)
- **Voice ID** — speaks frequency and PL tone after CW ID
- **Auto-announce** — weather/time default off (FCC 95.1733)
- **Kerchunk filter** — COR debounce prevents brief key-ups
- **TOT** — time-out timer prevents stuck transmissions
- **Emergency mode** — suppresses TOT and auto-announcements
- **Recording** — activity log for FCC 95.1705 cooperative use
- **CDR** — structured transmission logging for compliance

## Testing

```bash
make test    # 185 tests
```

- **Unit tests** (63): event bus, config parser, queue, repeater state events, CW ID encoding, response system
- **Integration tests** (122): repeater state machine (incl. closed repeater), DTMF dispatch, caller identification, voicemail, timers, user database, time, DSP decoders, recorder, TX encoder, emergency, parrot, CDR, CW ID module, stats (incl. persistence), OTP authentication

## License

MIT
