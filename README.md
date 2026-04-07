<p align="center">
  <img src="web/logo.png" alt="kerchunkd logo" width="400">
</p>

# kerchunkd — GMRS / Amateur / Part 90 Repeater Controller Daemon

A custom repeater controller for the Retevis RT97L repeater, built in C11 for Raspberry Pi and Linux/macOS. Interfaces with the repeater via its DB9 accessory port through a RIM-Lite v2 or AIOC (All-In-One-Cable) USB radio interface (CM119 chipset). All CTCSS/DCS/DTMF decoding and CW ID generation is handled in software using [libplcode](https://github.com/briankwest/libplcode). Supports GMRS (Part 95E), Amateur (Part 97), and Business/Industrial (Part 90) operation.

**29 modules** — repeater state machine, CW ID, caller identification, DTMF commands, voicemail, weather, time, NWS alerts, TTS (ElevenLabs / Wyoming), ASR (Wyoming speech recognition), parrot/echo, CDR, statistics, recording, burst tones, emergency mode, OTP authentication, courtesy tones, GPIO, logging, web dashboard, webhook notifications, voice scrambler, SDR channel monitor, FreeSWITCH autopatch, POCSAG paging, FLEX paging, APRS position/telemetry, PoC radio bridge.

**288 tests** — unit + integration test coverage with mock core vtable.

**Embedded CLI** — interactive console with tab completion and history when running in foreground mode (`kerchunkd -f`). Log output streams above the prompt.

**Native JSON API** — every CLI command returns structured JSON via `kerchunk -j`. Event streaming via `kerchunk -e -j` (NDJSON). Response system (`kerchunk_resp_t`) provides both formats from a single handler.

**Web Dashboard** — embedded HTTP/HTTPS server with split public/admin architecture. Public site (`/`) provides live audio monitoring, weather, NWS alerts, and repeater status with zero authentication. Admin site (`/admin/`) provides the full dashboard, user management, config editor, coverage planner, and PTT — protected by HTTP Basic Auth + optional IP-based ACL (`admin_acl`). TLS/HTTPS with Let's Encrypt. Set `public_only = on` to block admin access entirely for internet-facing deployments. Admin ACL returns 404 for non-matching IPs — no auth challenge, no hint the admin exists. Dynamic UI: modules register controls via CLI metadata (`/admin/api/commands`).

**Burst tones** — DTMF sequences, two-tone paging, Selcall, MDC-1200, CW ID, and tone burst generation via the `tones` CLI command.

**Wall-clock scheduler** — `schedule_aligned` for periodic tasks (CW ID), `schedule_at` for future one-shot events. Managed thread pool with 5 modules migrated to supervised threads.

**Configurable sample rate** — internal audio pipeline defaults to 48kHz, configurable to 8000/16000/32000/48000 Hz. Git hash embedded in version string and deb package metadata.

**Heartbeat event** — 5-second keepalive for SSE/WebSocket clients.

**19 core CLI commands, 25+ module CLI commands** with full inline help and tab completion.

## Table of Contents

- [GMRS / Amateur / Part 90 Feature Matrix](#gmrs--amateur--part-90-feature-matrix)
  - [Regulatory Notes](#regulatory-notes)
  - [Configuring for Service Type](#configuring-for-service-type)
- [Architecture](#architecture)
  - [Threading Model](#threading-model)
  - [Dual State Machines](#dual-state-machines)
  - [Core Audio Engine](#core-audio-engine)
- [Hardware](#hardware)
- [Dependencies](#dependencies)
- [Building](#building)
  - [Linux Setup](#linux-setup)
- [Running](#running)
- [CLI](#cli)
  - [Interactive Console](#interactive-console)
  - [One-shot and Scripting](#one-shot-and-scripting)
  - [JSON Output and Event Streaming](#json-output-and-event-streaming)
  - [Simulation Commands](#simulation-commands)
- [DTMF Commands](#dtmf-commands)
- [Modules](#modules)
  - [mod_repeater — RX State Machine](#mod_repeater--rx-state-machine)
  - [mod_cwid — CW Callsign Identification](#mod_cwid--cw-callsign-identification)
  - [mod_courtesy — Courtesy Tone](#mod_courtesy--courtesy-tone)
  - [mod_caller — Caller Identification](#mod_caller--caller-identification)
  - [mod_dtmfcmd — DTMF Command Router](#mod_dtmfcmd--dtmf-command-router)
  - [mod_voicemail — Voicemail](#mod_voicemail--voicemail)
  - [mod_weather — Weather Announcements](#mod_weather--weather-announcements)
  - [mod_time — Time Announcements](#mod_time--time-announcements)
  - [mod_recorder — Transmission Recording](#mod_recorder--transmission-recording)
  - [mod_tones — Burst Tones](#mod_tones--burst-tones)
  - [mod_emergency — Emergency Mode](#mod_emergency--emergency-mode)
  - [mod_otp — TOTP Authentication](#mod_otp--totp-authentication)
  - [mod_parrot — Echo/Parrot](#mod_parrot--echoparrot)
  - [mod_cdr — Call Detail Records](#mod_cdr--call-detail-records)
  - [mod_tts — Text-to-Speech (ElevenLabs)](#mod_tts--text-to-speech-elevenlabs)
  - [mod_nws — NWS Weather Alert Monitor](#mod_nws--nws-weather-alert-monitor)
  - [mod_stats — Statistics and Metrics](#mod_stats--statistics-and-metrics)
  - [mod_web — Web Dashboard](#mod_web--web-dashboard)
  - [mod_webhook — Webhook Notifications](#mod_webhook--webhook-notifications)
  - [mod_scrambler — Voice Scrambler (Part 90 only)](#mod_scrambler--voice-scrambler-part-90-only)
  - [mod_sdr — SDR Channel Monitor](#mod_sdr--sdr-channel-monitor)
  - [mod_gpio — GPIO Relay Control](#mod_gpio--gpio-relay-control)
  - [mod_logger — Event Logger](#mod_logger--event-logger)
  - [mod_pocsag — POCSAG Paging](#mod_pocsag--pocsag-paging)
  - [mod_flex — FLEX Paging](#mod_flex--flex-paging)
  - [mod_aprs — APRS Position/Telemetry](#mod_aprs--aprs-positiontelemetry)
- [General Config](#general-config)
- [Audio Config](#audio-config)
- [HID Config](#hid-config)
- [User and Group Database](#user-and-group-database)
- [GMRS Coverage Planner](#gmrs-coverage-planner)
- [Event Types](#event-types)
- [FCC Compliance](#fcc-compliance)
- [Testing](#testing)
- [License](#license)

## GMRS / Amateur / Part 90 Feature Matrix

kerchunkd supports GMRS (Part 95E), Amateur (Part 97), and Business/Industrial (Part 90) repeater operation. Some features have regulatory restrictions depending on the service type. The operator is responsible for compliance.

| Feature | GMRS | Amateur | Part 90 | Notes |
|---------|:----:|:-------:|:-------:|-------|
| Repeater state machine | Y | Y | Y | Core functionality |
| CW ID | Y | Y | Y | FCC-required station identification |
| Voice ID (TTS) | Y | Y | Y | Speaks frequency and PL tone |
| CTCSS/DCS encode/decode | Y | Y | Y | Tone squelch |
| DTMF commands | Y | Y | Y | Remote control via radio keypad |
| Caller identification | Y | Y | Y | ANI and DTMF login |
| Courtesy tones | Y | Y | Y | |
| Time/weather announcements | Y | Y | Y | On-demand via DTMF |
| NWS severe weather alerts | Y | Y | Y | Emergency public information |
| Emergency mode (*911#) | Y | Y | Y | Extended TX, suppress TOT |
| Voicemail | Y | Y | Y | |
| Parrot/echo test | Y | Y | Y | Audio quality check |
| Transmission recording | Y | Y | Y | FCC 95.1705 activity logging |
| Call detail records | Y | Y | Y | |
| Statistics and metrics | Y | Y | Y | |
| GPIO relay control | Y | Y | Y | |
| Web dashboard (listen) | Y | Y | Y | Public status, audio monitor |
| Webhook notifications | Y | Y | Y | |
| SDR channel monitor | Y | Y | Y | |
| OTP authentication | Y | Y | Y | |
| **Web PTT (transmit)** | **N** | Y | Y | GMRS: no remote/internet TX |
| **Voice scrambler** | **N** | **N** | **Y** | Part 90 only — prohibited on GMRS (FCC 95.333) and Amateur (FCC 97.113(a)(4)) |
| **AutoPatch (FreeSWITCH)** | **N** | Y | Y | GMRS: interconnection ambiguous |
| POCSAG paging | Y | Y | Y | Brief data transmission |
| FLEX paging | Y | Y | Y | Brief data transmission |
| APRS position/telemetry | Y | Y | Y | Brief data transmission, COR gated |

### Regulatory Notes

**GMRS (Part 95 Subpart E):**
- Station identification required (CW ID handles this)
- No encryption or scrambling (FCC 95.333)
- No unsolicited one-way transmissions (auto-announce defaults off)
- Interconnection (autopatch) not explicitly addressed post-2017 reform
- Web PTT constitutes remote control/internet linking — not permitted

**Amateur (Part 97):**
- Station identification per 97.119 (CW ID handles this)
- Autopatch permitted (97.113) — no business calls, third-party rules apply
- Scrambling/encryption prohibited (97.113(a)(4)) — this includes frequency inversion; do not use mod_scrambler on Amateur frequencies
- Internet linking and remote control permitted with proper identification
- Web PTT permitted with control operator oversight

**Business/Industrial (Part 90):**
- Encryption and scrambling are permitted — mod_scrambler is legal for Part 90 operation
- Station identification per 90.425 (CW ID or voice announcement)
- Remote control and interconnection (autopatch) permitted
- Web PTT permitted
- No prohibition on one-way transmissions — auto-announce features may be enabled

### Configuring for Service Type

**GMRS** — ensure these modules are **not loaded** in `[modules]`:

```ini
; Remove from load list for GMRS:
; mod_scrambler    — no encryption/scrambling on GMRS (FCC 95.333)
; mod_freeswitch   — no autopatch on GMRS
```

And disable Web PTT:

```ini
[web]
ptt_enabled = off    ; no remote TX on GMRS
```

**Amateur** — same as GMRS, except Web PTT and autopatch are permitted:

```ini
; Remove from load list for Amateur:
; mod_scrambler    — no encryption/scrambling on Amateur (FCC 97.113(a)(4))
```

**Part 90 (Business/Industrial)** — all modules may be loaded, including mod_scrambler:

```ini
[modules]
load = mod_scrambler    ; encryption permitted on Part 90
load = mod_freeswitch   ; interconnection permitted on Part 90

[web]
ptt_enabled = on        ; remote control permitted on Part 90
```

## Architecture

Modular design: a lightweight core with an event bus, dynamically loadable modules (`.so`), an outbound audio queue with priority, an interactive CLI, and an embedded web server.

```
┌───────────────────────────────────────────────────────────────┐
│                       kerchunkd (daemon)                      │
│                                                               │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
│  │ PortAudio│  │ HID      │  │ Event    │  │ Module   │       │
│  │ Audio    │  │ COR/PTT  │  │ Bus      │  │ Loader   │       │
│  │ (callback│  │ (hidraw) │  │ (mutex)  │  │ (dlopen) │       │
│  │ +ring buf│  │          │  │          │  │          │       │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘       │
│       │             │             │             │             │
│  ┌────┴─────────────┴─────────────┴─────────────┴────────┐    │
│  │              DSP Pipeline (libplcode)                 │    │
│  │  CTCSS dec . DCS dec . DTMF dec . CW ID enc . Tones   │    │
│  └───────────────────────────────────────────────────────┘    │
│                                                               │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                     │
│  │ Outbound │  │ Control  │  │ HTTP/SSE │                     │
│  │ Queue    │  │ Socket   │  │ mod_web  │                     │
│  │ (priority│  │ (per-    │  │ (port    │                     │
│  │  sorted) │  │  client) │  │   8080)  │                     │
│  └──────────┘  └──────────┘  └──────────┘                     │
│                                                               │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │                  Loaded Modules (27)                     │ │
│  │                                                          │ │
│  │  mod_repeater   RX state machine (IDLE/RECV/TAIL/HANG)   │ │
│  │  mod_cwid       Morse CW ID + voice ID via TTS           │ │
│  │  mod_courtesy   Courtesy tone on COR drop                │ │
│  │  mod_caller     Caller ID (DTMF ANI / DTMF login)        │ │
│  │  mod_dtmfcmd    DTMF command router (*XX#)               │ │
│  │  mod_voicemail  Record/play/delete voice messages        │ │
│  │  mod_gpio       GPIO relay control via DTMF              │ │
│  │  mod_logger     Event logging + rotation                 │ │
│  │  mod_weather    Weather via weatherapi.com + TTS         │ │
│  │  mod_time       Time announcements via TTS               │ │
│  │  mod_recorder   Per-transmission WAV recording           │ │
│  │  mod_tones      Burst tone toolbox (DTMF, Selcall, etc.) │ │
│  │  mod_emergency  Emergency mode (*911#/*910#)             │ │
│  │  mod_otp        TOTP authentication (*68<code>#)         │ │
│  │  mod_parrot     Echo/parrot for audio quality check      │ │
│  │  mod_cdr        Call detail records (daily CSV)          │ │
│  │  mod_tts        Text-to-speech (ElevenLabs / Wyoming)    │ │
│  │  mod_asr        Speech recognition (Wyoming ASR)         │ │
│  │  mod_poc        PoC radio server bridge (libpoc)         │ │
│  │  mod_nws        NWS weather alert monitor                │ │
│  │  mod_stats      Statistics, metrics, persistence         │ │
│  │  mod_web        HTTP server + SSE + web dashboard        │ │
│  │  mod_webhook    HTTP POST notifications on events        │ │
│  │  mod_scrambler  Frequency inversion voice scrambler      │ │
│  │  mod_sdr        RTL-SDR single-channel monitor           │ │
│  │  mod_freeswitch FreeSWITCH AutoPatch (Ham only)          │ │
│  │  mod_pocsag     POCSAG paging encoder                    │ │
│  │  mod_flex       FLEX paging encoder                      │ │
│  │  mod_aprs       APRS position reporting/telemetry        │ │
│  └──────────────────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────────────────┘
```

### Threading Model

- **Audio thread** (20ms) — captures audio, runs libplcode decoders (CTCSS/DCS/DTMF), software relay with drain, drains outbound queue, manages queue-driven PTT
- **Main thread** (20ms) — processes timers, polls control socket, handles COR/PTT, config reloads
- **Web thread** — accepts HTTP connections, serves API/SSE/static files
- **TTS thread** — async synthesis via ElevenLabs cloud or Wyoming local server (non-blocking)
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
        ┌──────────┬──────────┐
        │          │          │
        ▼          │          │
     ┌──────┐   (debounce)    │
     │ IDLE │─────►┐          │
     └──────┘      │          │
        ▲          ▼          │
        │    ┌───────────┐    │
        │    │ RECEIVING │◄───┤ COR re-assert (rekey)
        │    └───────────┘    │
        │         │           │
        │     COR drop        │
        │         │           │
        │         ▼           │
        │    ┌───────────┐    │
        │    │ TAIL_WAIT │────┘ COR re-assert (rekey)
        │    └───────────┘
        │         │
        │     tail expires
        │         │
        │         ▼
        │    ┌───────────┐
        │    │ HANG_WAIT │────┐ COR re-assert (rekey)
        │    └───────────┘
        │         │
        │     hang expires
        │         │
        └─────────┘

     TIMEOUT: fires after timeout_time in RECEIVING
```

**TX (Outbound) — what the repeater is transmitting:**

```
     ┌─────────┐
     │ TX_IDLE │  Not transmitting
     └─────────┘
          │
          ├─── COR assert (software_relay=on) ────┐
          │                                       ▼
          │                                  ┌─────────┐
          ├─── Queue has items               │ TX_RELAY│  Relaying RX audio
          │                                  └─────────┘
          ▼                                       │
     ┌─────────┐                              COR drop
     │ TX_QUEUE│  Playing queued audio            │
     └─────────┘  (TTS, weather, CW ID)           ▼
          │                                  ┌─────────┐
          ├─── queue empties                 │ TX_TAIL │  Drain + tail silence
          │                                  └─────────┘
          ▼                                       │
     ┌─────────┐                              PTT drop
     │ TX_TAIL │  TX tail silence (CTCSS)         │
     └─────────┘                                  ▼
          │                                  ┌─────────┐
      PTT drop                               │ TX_IDLE │
          │                                  └─────────┘
          ▼
     ┌─────────┐
     │ TX_IDLE │
     └─────────┘
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

- **Software relay** — when enabled, kerchunkd captures RX audio and retransmits in software. Live voice preempts queued announcements.
- **Relay drain** — on COR drop, relay continues for configurable period (default 500ms) to avoid cutting speech mid-word. Playback ring also fully drains before PTT releases.
- **PTT refcounting** — multiple modules can hold PTT simultaneously; hardware releases only when all refs drop to zero
- **Queue auto-PTT** — audio thread asserts PTT when draining, releases when empty
- **CTCSS/DCS continuous** — tone mixed into TX delay, all audio, TX tail, and relay
- **Configurable sample rate** — internal sample rate defaults to 48kHz (`sample_rate` in `[audio]`, valid: 8000/16000/32000/48000). `hw_rate` forces PortAudio to device-native rate with automatic resampling
- **WAV resampling** — queued WAV files are automatically resampled at load time via `kerchunk_resample()` to match the configured sample rate
- **Full-duplex stream** — single PortAudio stream when capture and playback are the same device (shared clock, no drift)

## Hardware

```
RT97L DB9 <──> RIM-Lite v2 / AIOC (CM108AH USB) <──> Raspberry Pi / Linux / Mac
                                                     ├── PortAudio: audio I/O
                                                     └── HID (hidraw): COR input / PTT output
```

| RIM-Lite | Signal | RT-97S | Notes |
|----------|--------|--------|-------|
| 2 | TX Voice Audio -> | 2 | < 100mV |
| 3 | <- COS in | 3 | Active Low |
| 5 | PTT -> | 9 | Active Low |
| 6 | <- RX Audio | 5 | De-emph. discriminator |
| 8,9 | Ground | 7 | |

### RIM-Lite HID GPIO Mapping

The RIM-Lite v2 uses a C-Media CM108AH chip (vendor `0d8c`, product `013a`).
COR and PTT are controlled via GPIO pins exposed through the Linux `hidraw` interface.

| GPIO | Bit | CM108 HID Usage | RIM-Lite Function |
|------|-----|-----------------|-------------------|
| GPIO0 | 0 | Volume Up | (unused) |
| GPIO1 | 1 | Volume Down | **COR input** |
| GPIO2 | 2 | Mute | **PTT output** |
| GPIO3 | 3 | (reserved) | (unused) |

Config values use 0-based GPIO/bit numbers directly — no conversion needed.

**COR polarity note:** The CM108 internally inverts GPIO inputs (active-low GPIO pin
= bit HIGH in the HID report). Use `cor_polarity = active_high` in the config because
the CM108 has already performed the inversion. Using `active_low` will double-invert
and COR drops will never be detected.

```ini
[hid]
device = /dev/rimlite      ; udev symlink (see udev rule below)
cor_bit = 1                ; GPIO1 = COR
cor_polarity = active_high ; CM108 inverts internally, do not double-invert
ptt_bit = 2               ; GPIO2 = PTT
```

The udev rule in `debian/kerchunk.udev` creates the `/dev/rimlite` symlink automatically:

```
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0d8c", ATTRS{idProduct}=="013a", \
  GROUP="audio", MODE="0660", SYMLINK+="rimlite"
```

## Dependencies

| Dependency | Purpose | Install (macOS) | Install (Linux) |
|-----------|---------|-----------------|-----------------|
| [libplcode](https://github.com/briankwest/libplcode) | CTCSS/DCS/DTMF/CWID codec | `make install` or .deb | `sudo dpkg -i libplcode-dev_*.deb` |
| PortAudio | Audio I/O | `brew install portaudio` | `apt install portaudio19-dev` |
| libcurl | HTTP (weather, NWS, TTS) | `brew install curl` | `apt install libcurl4-openssl-dev` |
| OpenFst | Optional: TTS text normalization | `brew install openfst` | `apt install libfst-dev` |
| [libnemo_normalize](https://github.com/briankwest/libnemo_normalize) | Optional: NeMo text normalization | `make install` or .deb | `sudo dpkg -i libnemo-normalize_*.deb` |
| librtlsdr | Optional: SDR channel monitor | `brew install librtlsdr` | `apt install librtlsdr-dev` |
| [libpocsag](https://github.com/briankwest/libpocsag) | Optional: POCSAG paging | `make install` or .deb | `sudo dpkg -i libpocsag-dev_*.deb` |
| [libflex](https://github.com/briankwest/libflex) | Optional: FLEX paging | `make install` or .deb | `sudo dpkg -i libflex-dev_*.deb` |
| [libaprs](https://github.com/briankwest/libaprs) | Optional: APRS position/telemetry | `make install` or .deb | `sudo dpkg -i libaprs-dev_*.deb` |
| [libpoc](https://github.com/briankwest/libpoc) | Optional: PoC radio bridge | `make install` or .deb | `sudo dpkg -i libpoc-dev_*.deb` |
| [libwyoming](https://github.com/briankwest/libwyoming) | Optional: Wyoming TTS/ASR | `make install` or .deb | `sudo dpkg -i libwyoming-dev_*.deb` |
| pkg-config | Build system | (included with Xcode) | `apt install pkg-config` |

## Building

Install dependencies first:

```bash
# Required (Debian/Ubuntu)
sudo apt install build-essential pkg-config autoconf automake libtool \
  portaudio19-dev libcurl4-openssl-dev libplcode-dev

# Optional: RTL-SDR channel monitor
sudo apt install librtlsdr-dev

# Optional: TTS text normalization (libnemo-normalize)
sudo apt install libnemo-normalize-dev libfst-dev

# Optional: Paging, APRS, PoC, Wyoming (detected by pkg-config)
sudo dpkg -i libpocsag-dev_*.deb   # POCSAG paging (mod_pocsag)
sudo dpkg -i libflex-dev_*.deb     # FLEX paging (mod_flex)
sudo dpkg -i libaprs-dev_*.deb     # APRS position/telemetry (mod_aprs)
sudo dpkg -i libpoc-dev_*.deb      # PoC radio bridge (mod_poc)
sudo dpkg -i libwyoming-dev_*.deb  # Wyoming TTS/ASR (mod_tts, mod_asr)
```

Build with autotools:

```bash
git clone https://github.com/briankwest/kerchunk.git
cd kerchunk

autoreconf -fi
./configure
make
make check       # Run test suite (234 tests)
sudo make install
```

Build outputs:
- `kerchunkd` — the daemon
- `kerchunk` — interactive CLI
- `modules/*.so` — 29 loadable modules
- `test_kerchunk` — test suite

### Linux Setup

On a fresh Linux install (Ubuntu/Debian), install all build dependencies:

```bash
sudo apt install build-essential pkg-config autoconf automake libtool \
  portaudio19-dev libcurl4-openssl-dev libplcode-dev

# Optional: for TTS text normalization (libnemo-normalize)
sudo apt install libnemo-normalize-dev libfst-dev

# Optional: for SDR channel monitor
sudo apt install librtlsdr-dev

# Optional: for paging and APRS (detected by pkg-config)
sudo dpkg -i libpocsag-dev_*.deb libflex-dev_*.deb libaprs-dev_*.deb
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

This grants `audio` group access to the C-Media CM119 HID interface (vendor `0d8c`, product `013a`) and creates a stable `SYMLINK+="rimlite"` device path at `/dev/rimlite`.

Without the audio group membership, PortAudio will report 0 devices and the daemon will run without audio. Without the udev rule, HID access requires running as root.

## Running

```bash
./kerchunkd -d                     # List audio devices
./kerchunkd -c kerchunk.conf -f    # Start in foreground
```

Public dashboard: `https://localhost:8080/` (when `[web] enabled = on`). Admin: `https://localhost:8080/admin/` (HTTP Basic Auth required).

## CLI

### Interactive Console

```bash
./kerchunk                         # Enter interactive mode
```

Features: tab completion, command history, live log streaming, auto-reconnect.

```
kerchunk> status                   # Daemon status (RX/TX state, PTT, COR)
kerchunk> help                     # Show all commands
kerchunk> version                  # Version and git hash (e.g. 1.0.1+abc1234)
kerchunk> uptime                   # Daemon uptime
kerchunk> audio                    # Audio device and sample rate info
kerchunk> hid                      # HID device status
kerchunk> user                     # Current user info
kerchunk> log                      # Log level control
kerchunk> diag                     # Diagnostics
kerchunk> play <file>              # Play a WAV file
kerchunk> tone <freq> <dur>        # Generate a tone
kerchunk> /log debug               # Start log streaming
kerchunk> /nolog                   # Stop log streaming
kerchunk> exit                     # Exit console
```

19 core commands: status, help, version, uptime, audio, hid, user, log, diag, play, tone, sim, tts, cwid, caller, emergency, dtmfcmd, threads, schedule.

Key module commands:

```
kerchunk> tones dtmf 1234            # Send DTMF sequence
kerchunk> tones twotone 1000 1500    # Send two-tone page
kerchunk> tones selcall 12345        # Send Selcall sequence
kerchunk> tones mdc 1234             # Send MDC-1200 burst
kerchunk> tones burst 1000 500       # Send tone burst (freq, duration_ms)
kerchunk> tones cwid                 # Send CW ID burst
kerchunk> threads                    # Show managed thread pool status
kerchunk> schedule                   # Show wall-clock scheduler status
kerchunk> pocsag send 1234 "Test"    # Send POCSAG page
kerchunk> flex send 1234 "Test"      # Send FLEX page
kerchunk> aprs beacon                # Force APRS beacon
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
{"rx_state":"IDLE","tx_state":"TX_IDLE","ptt":false,"cor":false,"queue":0,"modules":29,"users":2,"emergency":false}

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
| `*97#` | Toggle scrambler on/off | mod_scrambler |
| `*970#` | Disable scrambler | mod_scrambler |
| `*971#`-`*978#` | Set scrambler code 1-8 | mod_scrambler |
| `*98#` | Force immediate APRS beacon | mod_aprs |
| `*980#` | APRS status report | mod_aprs |

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
| `cor_drop_hold` | int | `1000` | COR drop hold time in ms (absorbs DTMF COS glitches) |
| `require_identification` | on/off | `off` | Closed repeater: deny unless identified |
| `voice_id` | on/off | `on` | Speak frequency/PL via TTS after CW ID |

Config section: `[repeater]`

### mod_cwid — CW Callsign Identification

Morse CW ID + voice ID ("WRDP519 repeater, 462.550, PL 131.8") via TTS. Two modes:

- **always** (default) — Fixed wall-clock timer. IDs every N minutes regardless of activity.
- **on_call** — Event-driven (RT97L-style). IDs only during and after activity: initial ID on first key-up, repeating ID during conversation, final ID after last TX, then silent.

Interval capped at 15 min (FCC 95.1751).

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cwid_mode` | string | `always` | `always` = fixed timer, `on_call` = activity-triggered |
| `cwid_interval` | ms | `600000` | ID interval during active conversation (capped at 900000) |
| `cwid_tail` | ms | (interval) | on_call: delay before final ID after last activity |
| `cwid_wpm` | int | `20` | Words per minute (min 5) |
| `cwid_freq` | Hz | `800` | Tone frequency |
| `tx_ctcss` | int | -- | CTCSS freq x10 (e.g., 1318 = 131.8 Hz). Announced in voice ID as "PL 131.8" |
| `tx_dcs` | int | -- | DCS code (e.g., 23). Announced in voice ID as "DCS 023" |
| `pl_tone` | string | -- | Explicit PL/DCS string for voice ID (overrides tx_ctcss/tx_dcs) |

Config section: `[repeater]`. Callsign from `[general] callsign`. Frequency from `[general] frequency`. Set `voice_id = on` and one of `tx_ctcss`, `tx_dcs`, or `pl_tone` to announce the access tone after the CW ID.

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

Records RX (per COR cycle) and TX (per queue drain) to timestamped WAV files. Recording filenames use the username (not display name).

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | on/off | `off` | Enable recording |
| `directory` | string | `recordings` | Output directory |
| `max_duration` | s | `300` | Max recording length |

Config section: `[recording]`

### mod_tones — Burst Tones

DTMF sequences, two-tone paging, Selcall, MDC-1200, CW ID, and tone burst generation.

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

### mod_tts — Text-to-Speech (ElevenLabs / Wyoming)

Async worker thread. Two engines: ElevenLabs (cloud API) or Wyoming (local/network via [libwyoming](https://github.com/briankwest/libwyoming)). Wyoming connects to a wyoming-server running Piper TTS — no subprocess, no Python. Responses cached as WAV files keyed by text hash in `<sounds_dir>/cache/tts/`. Use `tts cache-clear` to flush.

Optional text normalization via [libnemo_normalize](https://github.com/briankwest/libnemo_normalize) (requires OpenFst). Normalizes numbers, times, dates, and abbreviations before synthesis so TTS speaks them correctly (e.g., "3:45 PM" → "three forty five PM"). Configure `normalize_far_dir` in `[tts]` to enable.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `engine` | string | `elevenlabs` | `elevenlabs` or `wyoming` |
| `api_key` | string | | ElevenLabs API key (elevenlabs engine) |
| `voice_id` | string | `21m00Tcm4TlvDq8ikWAM` | ElevenLabs voice ID |
| `model` | string | `eleven_turbo_v2_5` | ElevenLabs model ID |
| `wyoming_host` | string | `127.0.0.1` | Wyoming server host (wyoming engine) |
| `wyoming_port` | int | `10200` | Wyoming server port |
| `wyoming_voice` | string | | Voice name (empty = server default) |
| `normalize_far_dir` | string | | Path to NeMo FAR grammars (optional) |

Config section: `[tts]`

### mod_asr — Automatic Speech Recognition

Transcribes all inbound RF transmissions via a Wyoming ASR server ([libwyoming](https://github.com/briankwest/libwyoming)). Supports batch mode (Whisper — best accuracy, ~1s delay after COR drop) and streaming mode (Zipformer — instant transcript on COR drop). Transcripts are logged, stored in a rolling history, and available via `asr history`.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | on/off | `off` | Enable ASR |
| `mode` | string | `batch` | `batch` (Whisper) or `streaming` (Zipformer) |
| `wyoming_host` | string | `127.0.0.1` | Wyoming ASR server host |
| `wyoming_port` | int | `10200` | Wyoming ASR server port |
| `language` | string | `en` | Language code |
| `max_capture` | int | `30` | Max seconds to capture per transmission |
| `min_duration` | duration | `500` | Min duration to transcribe (skip kerchunks) |

Config section: `[asr]`

### mod_poc — PoC Radio Bridge

Bridges Push-to-Talk over Cellular radios (Retevis L71, TYT, etc.) to the RF repeater via [libpoc](https://github.com/briankwest/libpoc). Bidirectional audio bridging, user/group sync from kerchunk DB, TLS support, SOS alerts, and text messaging.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | int | `1` | Enable PoC server |
| `port` | int | `29999` | Listen port |
| `rf_bridge_group` | int | `0` | Kerchunk group ID bridged to RF (0=none) |
| `rf_to_poc` | int | `1` | Forward RF audio to PoC clients |
| `poc_to_rf` | int | `1` | Forward PoC audio to RF TX |

Config section: `[poc]`. Per-user access: add `poc_password` to `[user.N]` sections.

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

Embedded HTTP/HTTPS server with split public/admin architecture, JSON API, SSE event stream, WebSocket audio streaming and PTT, and static file serving.

**Public** (`/`) — no authentication required:

- **Dashboard** (`index.html`) — repeater status, live audio (listen-only), weather, NWS alerts
- **Registration** (`register.html`) — self-registration (when `registration_enabled = on`)

**Admin** (`/admin/`) — HTTP Basic Auth required:

- **Dashboard** (`admin/index.html`) — real-time SSE event stream, controls, TTS, statistics
- **Users** (`admin/users.html`) — user/group CRUD with TOTP QR codes
- **Config** (`admin/config.html`) — live config editor with reload
- **Coverage** (`admin/coverage.html`) — GMRS RF coverage planner with terrain analysis
- **PTT** (`admin/ptt.html`) — WebSocket push-to-talk with mic capture and RX audio playback

Public API routes (`/api/status`, `/api/weather`, `/api/nws`, `/api/audio` WebSocket listen-only, `POST /api/register`) require no authentication and do not expose sensitive data (API keys, etc). Admin API routes (`/admin/api/*`) require HTTP Basic Auth. The public `/api/audio` WebSocket is listen-only; PTT commands are only accepted on `/admin/api/audio`.

Set `public_only = on` to block all `/admin/` access — for internet-facing deployments where admin is accessed via VPN.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | on/off | `off` | Enable web server |
| `port` | int | `8080` | Listen port |
| `bind` | string | `127.0.0.1` | Bind address (0.0.0.0 for external) |
| `auth_user` | string | `admin` | HTTP Basic Auth username for /admin/ |
| `auth_password` | string | | HTTP Basic Auth password for /admin/ |
| `public_only` | on/off | `off` | Block all /admin/ access |
| `static_dir` | string | | Path to HTML/JS/CSS files |
| `tls_cert` | string | | Path to TLS certificate (PEM). Enables HTTPS |
| `tls_key` | string | | Path to TLS private key (PEM). Required with tls_cert |
| `ptt_enabled` | on/off | `off` | Enable WebSocket PTT (admin only) |
| `ptt_max_duration` | duration | `30s` | Max PTT duration |
| `ptt_priority` | int | `2` | Queue priority for PTT audio |
| `registration_enabled` | on/off | `off` | Enable public user self-registration |

Config section: `[web]`

Public API: `GET /api/status`, `GET /api/weather`, `GET /api/nws`, `/api/audio` WebSocket (listen-only), `POST /api/register`

Admin API: `GET /admin/api/status` (includes sensitive fields), `GET /admin/api/stats`, `GET /admin/api/users`, `GET /admin/api/groups`, `GET /admin/api/config`, `GET /admin/api/commands`, `GET /admin/api/events` (SSE), `/admin/api/audio` WebSocket (PTT), `POST /admin/api/cmd`, `POST /admin/api/config/reload`, CRUD `POST/PUT/DELETE /admin/api/users/{id}`, `POST/PUT/DELETE /admin/api/groups/{id}`

### mod_webhook — Webhook Notifications

Fires HTTP POST to a configured URL when specified events occur. Background thread with configurable timeout and retry. Events: COR, PTT, caller identification, announcements, recordings, state changes, shutdown.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | on/off | `off` | Enable webhook |
| `url` | string | | Destination URL |
| `secret` | string | | Shared secret (sent as `X-Webhook-Secret` header) |
| `events` | string | | Comma-separated event list |
| `timeout_ms` | int | `5000` | HTTP request timeout |
| `retry_count` | int | `2` | Retry failed requests |

Config section: `[webhook]`

### mod_scrambler — Voice Scrambler (Part 90 only)

Frequency inversion voice scrambler for **Part 90 (Business/Industrial) operation only**. Prohibited on GMRS (FCC 95.333) and Amateur (FCC 97.113(a)(4)). Self-inverse: same operation scrambles and descrambles. 8 codes mapping to carrier frequencies 2700-3400 Hz (100 Hz steps). CW ID and emergency mode bypass scrambling.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | on/off | `off` | Enable scrambler |
| `code` | int | `4` | Code 1-8 (carrier = 2600 + code*100 Hz) |
| `frequency` | int | | Optional explicit carrier Hz (overrides code) |

Config section: `[scrambler]`. DTMF: `*97#` toggle, `*970#` off, `*971#`-`*978#` set code.

### mod_sdr — SDR Channel Monitor

Uses an RTL-SDR dongle (librtlsdr) to monitor a single channel. Tunes to the channel frequency at 240 kHz sample rate, FM demodulation with de-emphasis, CTCSS/DCS/DTMF decoding via libplcode, FM noise squelch, and CSV activity logging.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | on/off | `off` | Enable SDR monitor |
| `device_index` | int | `0` | RTL-SDR device index |
| `channel` | int | `1` | Channel number (1-22) |
| `log_file` | string | `sdr_activity.csv` | Activity log file |

Config section: `[sdr]`. CLI: `sdr`. Requires `librtlsdr-dev`.

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

### mod_pocsag — POCSAG Paging (experimental)

Encodes and transmits POCSAG paging messages via the repeater's TX audio path. Supports baseband and FSK modulation modes, with optional de-emphasis. Requires [libpocsag](https://github.com/briankwest/libpocsag) (detected by pkg-config at build time).

> **Experimental:** POCSAG encoding and transmission work but have not been extensively tested over the air. Use with caution.

Config section: `[pocsag]`. CLI: `pocsag send <capcode> <message>`, `pocsag numeric <capcode> <digits>`, `pocsag tone <capcode>`, `pocsag status`.

### mod_flex — FLEX Paging (experimental)

Encodes and transmits FLEX paging messages via the repeater's TX audio path. Supports 1600/3200/6400 bps speeds, baseband and FSK modulation modes, with optional de-emphasis. Requires [libflex](https://github.com/briankwest/libflex) (detected by pkg-config at build time).

> **Experimental:** FLEX encoding and transmission work but have not been extensively tested over the air. Use with caution.

Config section: `[flex]`. CLI: `flex send <capcode> <message>`, `flex numeric <capcode> <digits>`, `flex tone <capcode>`, `flex status`.

### mod_aprs — APRS Position/Telemetry

APRS position reporting and packet decoding. TX path generates AFSK 1200 position beacons queued into the audio pipeline. RX path decodes APRS packets from mod_sdr audio. Requires [libaprs](https://github.com/briankwest/libaprs) (detected by pkg-config at build time).

Config section: `[aprs]`. CLI: `aprs beacon`, `aprs send <message>`, `aprs status`. See [APRS.md](APRS.md) for architecture details.

## General Config

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `callsign` | string | | FCC callsign for CW ID |
| `frequency` | string | | Output frequency (for display/voice ID) |
| `offset` | string | | Input offset (for display) |
| `sample_rate` | int | `48000` | Internal audio sample rate (valid: 8000, 16000, 32000, 48000). Moved to `[audio]` section — see Audio Config |
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
| `sample_rate` | int | `48000` | Internal audio sample rate (valid: 8000, 16000, 32000, 48000) |
| `capture_device` | string | `default` | PortAudio capture device |
| `playback_device` | string | `default` | PortAudio playback device |
| `hw_rate` | int | `0` | Force hardware sample rate (0=auto, 48000 recommended for USB) |
| `preemphasis` | on/off | `off` | Pre-emphasis filter |
| `preemphasis_alpha` | float | `0.95` | Pre-emphasis filter coefficient |
| `speaker_volume` | int | `-1` | ALSA speaker playback volume (0-151, -1=don't set) |
| `mic_volume` | int | `-1` | ALSA mic capture volume (0-16, -1=don't set) |
| `agc` | on/off | `off` | Auto Gain Control |

Config section: `[audio]`

## HID Config

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `device` | string | `/dev/rimlite` | HID device path (udev symlink) |
| `cor_bit` | int | `1` | GPIO number for COR input (0-7) |
| `cor_polarity` | string | `active_high` | `active_high` (CM108 inverts internally) |
| `ptt_bit` | int | `2` | GPIO number for PTT output (0-7) |

Config section: `[hid]`

Use `kerchunk-diag -C` to dump raw HID reports and verify which GPIO bit
corresponds to COR on your interface. Use `kerchunk-diag -T` to test PTT.

## User and Group Database

Users and groups can be defined in the main `kerchunk.conf` or in a separate file via `users_file` in `[general]`. When `users_file` is set (e.g., `users_file = users.conf`), all `[user.N]` and `[group.N]` sections are loaded from that file instead. The web UI writes changes to the separate file only, keeping the main config untouched.

```ini
[group.1]
name = Family

[user.1]
username = bwest          # Lowercase, no spaces — login identity
name = Brian West         # Display name
callsign = KD0SBW        # Amateur/GMRS callsign
email = brian@example.com
dtmf_login = 101         # Or dial *101#
ani = 5551
access = 2               # admin
voicemail = 1
group = 1
totp_secret = JBSWY3DPEHPK3PXP  # Base32 TOTP secret (for mod_otp)
```

Users without a `username` field in config get one auto-derived from their name (lowercase, spaces replaced with underscores).

See `kerchunk.conf.example` for complete annotated reference.

## GMRS Coverage Planner

The public dashboard (`index.html`) includes a GMRS coverage map. The full coverage planner is at `coverage.html`. Uses site location (`latitude`, `longitude`, `elevation` from `[general]`) and Google Maps API (`google_maps_api_key`) to calculate and display estimated RF coverage with terrain analysis.

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

**Part 95E (GMRS) and Part 97 (Amateur):**
- **CW ID** — interval capped at 15 min max (FCC 95.1751 / 97.119)
- **Voice ID** — speaks frequency and PL tone after CW ID
- **Auto-announce** — weather/time default off (FCC 95.1733 — no unsolicited one-way TX)
- **Kerchunk filter** — COR debounce prevents brief key-ups
- **TOT** — time-out timer prevents stuck transmissions
- **Emergency mode** — suppresses TOT and auto-announcements
- **Recording** — activity log for FCC 95.1705 cooperative use
- **CDR** — structured transmission logging for compliance
- **Scrambler disabled by default** — encryption prohibited on GMRS (FCC 95.333) and Amateur (FCC 97.113(a)(4))

**Part 90 (Business/Industrial):**
- **Scrambler permitted** — frequency inversion encryption is legal on Part 90 frequencies
- **Station ID** — CW ID or voice announcement per FCC 90.425
- **Auto-announce permitted** — no one-way transmission restriction
- All other compliance features (TOT, recording, CDR) apply equally

## Testing

```bash
make test    # 234 tests
```

- **Unit tests** (63): event bus, config parser, queue, repeater state events, CW ID encoding, response system
- **Integration tests** (151): repeater state machine (incl. closed repeater), DTMF dispatch, caller identification, voicemail, timers, user database (incl. username lookup, auto-derive, group TX), time, DSP decoders, recorder, TX encoder, emergency, parrot, CDR, CW ID module, stats (incl. persistence), OTP authentication, voice scrambler (self-inverse, DTMF control, bypass)

## License

MIT
