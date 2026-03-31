# kerchunkd Architecture

Complete technical architecture of the kerchunkd GMRS/HAM repeater controller.

## Table of Contents

- [System Overview](#system-overview)
- [Threading Model](#threading-model)
  - [Synchronization](#synchronization)
- [Event Bus](#event-bus)
  - [Event Types](#event-types)
- [Audio Pipeline](#audio-pipeline)
  - [RX Path (Capture)](#rx-path-capture)
  - [TX Path (Playback)](#tx-path-playback)
  - [Queue Priority Levels](#queue-priority-levels)
- [RX State Machine (mod_repeater)](#rx-state-machine-mod_repeater)
- [TX State Machine](#tx-state-machine)
- [HID Driver (CM108/CM119 GPIO)](#hid-driver-cm108cm119-gpio)
  - [PTT Write Format (5 bytes)](#ptt-write-format-5-bytes)
  - [COR Read Format](#cor-read-format)
  - [COR Drop Hold Timer](#cor-drop-hold-timer)
- [WebSocket Audio Streaming](#websocket-audio-streaming)
- [Module Lifecycle](#module-lifecycle)
  - [DTMF Command Registration](#dtmf-command-registration)
- [User & Group System](#user--group-system)
- [Web Dashboard](#web-dashboard)
- [FCC Compliance](#fcc-compliance)
- [DSP Pipeline (libplcode)](#dsp-pipeline-libplcode)
  - [DTMF Decoder State Machine](#dtmf-decoder-state-machine)
- [Configuration Hierarchy](#configuration-hierarchy)
- [Build Artifacts](#build-artifacts)

---

## System Overview

```
                          ┌─────────────────────────────────────────┐
                          │            kerchunkd daemon             │
                          │                                         │
  ┌──────────┐  USB HID   │  ┌─────────┐  ┌──────────┐  ┌───────┐   │  ┌──────────┐
  │ RIM-Lite ├────────────┼──┤ HID     │  │ Event    │  │Module │   │  │ kerchunk │
  │ CM119    │  COR/PTT   │  │ Driver  │  │ Bus      │  │Loader │   │  │ CLI      │
  │          ├────────────┼──┤ GPIO    │  │ pub/sub  │  │dlopen │   │  └────┬─────┘
  └──────────┘  Audio     │  └────┬────┘  └────┬─────┘  └───┬───┘   │       │
  ┌──────────┐  I/O       │       │            │            │       │  Unix socket
  │PortAudio ├────────────┼──┐    │            │            │       │       │
  │ ALSA     │            │  │    │            │            │       │  ┌────┴─────┐
  └──────────┘            │  │  ┌─┴────────────┴────────────┴────┐  │  │ Control  │
                          │  │  │        Core vtable             │  │  │ Socket   │
                          │  │  │  (kerchunk_core_t)             │  │  └──────────┘
                          │  │  │                                │  │
                          │  │  │  subscribe/fire  queue_audio   │  │
                          │  │  │  request/release_ptt           │  │
                          │  │  │  timer_create/cancel           │  │
                          │  │  │  dtmf_register/unregister      │  │
                          │  │  │  tts_speak  user_lookup        │  │
                          │  │  └────────────────────────────────┘  │
                          │  │                                      │
                          │  │  ┌────────────────────────────────┐  │
                          │  └──┤      Audio Engine              │  │
                          │     │  capture ring ◄── PortAudio    │  │
                          │     │  playback ring ──► PortAudio   │  │
                          │     │  ring buffer: 262144 samples   │  │
                          │     │  (5.5s at 48kHz)               │  │
                          │     │  ALSA mixer init on startup    │  │
                          │     │  WAV resample via              │  │
                          │     │  kerchunk_resample() at load   │  │
                          │     └────────────────────────────────┘  │
                          │                                         │
                          │  ┌───────────────────────────────────┐  │
                          │  │         27 Loaded Modules         │  │
                          │  │  repeater  cwid      courtesy     │  │
                          │  │  caller    dtmfcmd   otp          │  │
                          │  │  voicemail gpio      logger       │  │
                          │  │  weather   time      recorder     │  │
                          │  │  txcode    emergency parrot       │  │
                          │  │  cdr       tts       nws          │  │
                          │  │  stats     web       webhook      │  │
                          │  │  scrambler sdr       freeswitch   │  │
                          │  │  pocsag    flex      aprs         │  │
                          │  └───────────────────────────────────┘  │
                          └─────────────────────────────────────────┘
```

---

## Threading Model

```
  Thread 1: Main Loop (20ms tick)
  ├── COR polling (HID read + drop hold timer)
  ├── Timer expiry dispatch
  ├── Control socket poll (CLI commands)
  ├── Config reload (SIGHUP)
  └── Module tick events

  Thread 2: Audio Thread (20ms tick, clock_nanosleep)
  ├── Capture frame from PortAudio ring
  ├── CTCSS/DCS decoders (on raw frame)
  ├── RX descrambler (in-place)
  ├── Audio tap dispatch (→ WebSocket SPSC ring)
  ├── DTMF decoder (on descrambled frame)
  ├── Software relay (when enabled)
  ├── Queue drain engine (tx_delay → audio → tx_tail → PTT release)
  └── Playback to PortAudio ring

  Thread 3: Web Server (mongoose mg_mgr_poll)
  ├── HTTP/HTTPS request handling
  ├── SSE event streaming
  ├── WebSocket audio delivery (on mg_wakeup)
  ├── WebSocket PTT receive
  └── TLS handshakes

  Thread 4: Audio Flush (5ms cadence)
  ├── Check SPSC ring for pending audio frames
  └── mg_wakeup() to trigger mongoose flush

  Thread 5: TTS Worker (cond_wait)
  ├── ElevenLabs API calls (libcurl)
  ├── PCM16 WAV conversion (resampled to configured rate)
  └── Cache management (hash-keyed WAV files)

  Thread 6: NWS Poller (cond_wait)
  ├── api.weather.gov polling
  ├── Alert tracking by ID
  └── TTS announcement trigger

  Thread 7: PortAudio Callback (real-time, ALSA poll)
  ├── Capture callback → capture ring
  └── Playback callback ← playback ring
```

### Synchronization

| Mechanism | Where | Purpose |
|-----------|-------|---------|
| `pthread_mutex` | PTT refcount | Protect `g_ptt_refcount` / `g_ptt_active` |
| `pthread_mutex` | Config access | `lock_config` / `unlock_config` for reload |
| `pthread_mutex` | Control socket | Per-client write protection |
| `atomic_size_t` | Audio ring buffers | Lock-free SPSC (PortAudio ↔ audio thread) |
| `atomic_uint` | WebSocket SPSC ring | Lock-free (audio thread → web thread) |
| `atomic_int` | WebSocket client count | Track active audio streams |
| `pthread_cond` | TTS worker | Wake on new job |
| `pthread_cond` | NWS poller | Wake on poll interval |
| `mg_wakeup()` | Audio flush → mongoose | Thread-safe cross-thread wake |
| `volatile sig_atomic_t` | Signal handling | `g_running`, `g_reload` flags |
| Snapshot dispatch | Event bus | Copy subscriber list under lock, dispatch after unlock |

---

## Event Bus

```
  Producer                    Event Bus                     Subscribers
  ─────────                   ──────────                    ───────────

  Audio thread ──┐
                 │         ┌──────────────┐
  Main loop ─────┼────────►│ kerchevt_fire│
                 │         │              │──► handler_1(evt, ud)
  Modules ───────┤         │  1. Lock     │──► handler_2(evt, ud)
                 │         │  2. Snapshot │──► handler_3(evt, ud)
  Timers ────────┘         │  3. Unlock   │       ...
                           │  4. Dispatch │──► handler_N(evt, ud)
                           └──────────────┘
                           Max 32 subscribers per event type
                           Snapshot prevents deadlock if handler fires events
```

### Event Types

| Event | Fired By | Subscribers |
|-------|----------|-------------|
| `AUDIO_FRAME` | Audio thread (20ms) | scrambler, taps |
| `CTCSS_DETECT` | Audio thread | logger, web, webhook |
| `DCS_DETECT` | Audio thread | logger, web, webhook |
| `DTMF_DIGIT` | Audio thread | dtmfcmd, caller, logger, web |
| `DTMF_END` | Audio thread | dtmfcmd, logger |
| `COR_ASSERT` | Main loop | repeater, dtmfcmd, caller, recorder, logger, web, stats |
| `COR_DROP` | Main loop | repeater, dtmfcmd, caller, recorder, courtesy, logger, web, stats |
| `PTT_ASSERT` | Core (request_ptt) | recorder, logger, web, stats |
| `PTT_DROP` | Core (release_ptt) | recorder, logger, web, stats |
| `STATE_CHANGE` | mod_repeater | cwid, logger, web, stats |
| `TAIL_START` | mod_repeater | cwid, courtesy, logger, web |
| `TAIL_EXPIRE` | mod_repeater | logger, web |
| `TIMEOUT` | mod_repeater | logger, web, stats |
| `CALLER_IDENTIFIED` | mod_caller | repeater, recorder, txcode, logger, web |
| `CALLER_CLEARED` | mod_caller | repeater, recorder, txcode, logger, web |
| `QUEUE_DRAIN` | Audio thread | logger, web |
| `QUEUE_COMPLETE` | Audio thread | courtesy, logger, web |
| `RECORDING_SAVED` | mod_recorder | cdr, logger, web |
| `ANNOUNCEMENT` | cwid, weather, time | cdr, logger, web |
| `CONFIG_RELOAD` | Main loop (SIGHUP) | logger, web |
| `SHUTDOWN` | Main loop | stats, logger, web (sends shutdown event to SSE/WebSocket; browser uses exponential backoff) |
| `TICK` | Main loop (20ms) | — |
| `CUSTOM+0..16` | mod_dtmfcmd | Target modules (voicemail, gpio, weather, etc.) |

---

## Audio Pipeline

### RX Path (Capture)

```
  PortAudio ALSA ──► Capture Ring ──► Audio Thread
                     (lock-free)          │
                                          ▼
                                    ┌───────────┐
                                    │ Raw Frame │ 960 samples at 48kHz (20ms, configurable)
                                    └─────┬─────┘
                                          │
                          ┌───────────────┼───────────────┐
                          ▼               ▼               ▼
                    CTCSS Decoder   DCS Decoder    (raw events)
                    (libplcode)    (libplcode)
                          │               │
                          ▼               ▼
                    CTCSS_DETECT    DCS_DETECT events
                                          │
                                          ▼
                                   ┌─────────────┐
                                   │ Descrambler │ (if enabled)
                                   └──────┬──────┘
                                          │
                          ┌───────────────┼───────────────┐
                          ▼               ▼               ▼
                    DTMF Decoder    Audio Taps      AUDIO_FRAME
                    (libplcode)    (recording,       event
                          │         WebSocket)
                          ▼
                    DTMF_DIGIT / DTMF_END events
```

### TX Path (Playback)

```
  Queue Items                    Audio Thread                PortAudio
  ───────────                    ────────────                ─────────

  ┌──────────┐     ┌─────────────────────────────────┐
  │ WAV file │     │                                 │
  │ PCM buf  │────►│  1. Assert PTT                  │
  │ Tone gen │     │  2. TX delay (silence + CTCSS)  │
  │ Silence  │     │  3. Drain frame (960 samples)   │
  └──────────┘     │  4. Scrambler (if enabled)      │
   (priority       │  5. Mix TX CTCSS/DCS encoder    │
    sorted)        │  6. Write to playback ring      │──► Playback Ring ──► ALSA
                   │  7. Dispatch playback taps      │    (lock-free)
                   │  8. When empty: TX tail         │
                   │  9. Wait for ring drain         │
                   │ 10. Hold 60ms for HW flush      │
                   │ 11. Release PTT                 │
                   └─────────────────────────────────┘
```

### Queue Priority Levels

```
  Priority 10  KERCHUNK_PRI_CRITICAL   Emergency, TOT warning
  Priority  5  KERCHUNK_PRI_IDENT      CW ID, voice ID
  Priority  4  KERCHUNK_PRI_HIGH       Repeater system tones, NWS errors
  Priority  3  KERCHUNK_PRI_ELEVATED   Weather, OTP, TTS announcements
  Priority  2  KERCHUNK_PRI_NORMAL     Time, parrot, courtesy, web PTT
  Priority  1  KERCHUNK_PRI_LOW        Voicemail playback, GPIO confirms

  Behavior:
  - Queue idle: items inserted by priority (higher first)
  - Queue draining: new items append to tail (no preemption)
  - Inter-file gap: 10ms silence between consecutive WAV files
```

---

## RX State Machine (mod_repeater)

```
                      COR assert
             ┌───────────────────────┐
             │                       │
             ▼        (debounce)     │
          ┌──────┐──────►┌──────────────┐
          │ IDLE │       │  RECEIVING   │◄──┐ COR re-assert (rekey)
          └──────┘       └──────┬───────┘   │
             ▲                  │           │
             │              COR drop        │
             │                  │           │
             │                  ▼           │
             │           ┌──────────────┐   │
             │           │  TAIL_WAIT   │───┘ COR re-assert
             │           └──────┬───────┘
             │                  │
             │           tail timer expires
             │                  │
             │                  ▼
             │           ┌──────────────┐
             │           │  HANG_WAIT   │───┐ COR re-assert
             │           └──────┬───────┘   │
             │                  │           │
             │           hang timer expires │
             │                  │           │
             └──────────────────┘           │
                                            │
          TIMEOUT: fires after timeout_time │
          in RECEIVING → releases PTT       │
                                            │
  Notes:                                    │
  - software_relay=on: PTT asserted during RECEIVING
  - software_relay=off: no PTT on COR (RT97L relays internally)
  - COR drop hold: configurable delay before accepting COR drop
    (absorbs DTMF-induced COS glitches, default 1000ms)
  - cor_debounce: filters brief key-ups (kerchunk filter)
```

## TX State Machine

```
  ┌───────────┐                         ┌───────────┐
  │  TX_IDLE  │──── Queue has items ───►│ TX_QUEUE  │
  └───────────┘                         │           │
       ▲                                │ tx_delay  │
       │                                │ audio     │
       │                                │ tx_tail   │
       │                                └─────┬─────┘
       │                                      │
       │                             queue empty + ring drained
       │                             + 60ms HW flush hold
       │                                      │
       └──────────────────────────────────────┘
                    PTT released

  When software_relay=on:
  ┌───────────┐                         ┌───────────┐
  │  TX_IDLE  │──── COR assert ────────►│ TX_RELAY  │
  └───────────┘                         │           │
       ▲                                │ relay RX  │
       │                                │ audio to  │
       │                                │ playback  │
       │                                └─────┬─────┘
       │                                      │
       │                             COR drop + relay drain
       │                                      │
       └──────────────────────────────────────┘
```

---

## HID Driver (CM108/CM119 GPIO)

### PTT Write Format (5 bytes)

```
  Byte:    [0]      [1]      [2]        [3]        [4]
  Field:   reserved reserved GPIO data  GPIO mask  SPDIF
  Value:   0x00     0x00     pin_value  pin_dir    0x00

  GPIO pin N → bit (N-1):
    GPIO1 = bit 0 = 0x01
    GPIO2 = bit 1 = 0x02
    GPIO3 = bit 2 = 0x04  ← typical PTT pin
    GPIO4 = bit 3 = 0x08

  PTT ON  (GPIO3): [0x00, 0x00, 0x04, 0x04, 0x00]
  PTT OFF (GPIO3): [0x00, 0x00, 0x00, 0x04, 0x00]

  Config: ptt_bit = 3 (GPIO pin number, 1-indexed)
  Write via: write(fd, buf, 5) on /dev/hidraw* (or /dev/rimlite symlink)
```

### COR Read Format

```
  read(fd, buf, 4) — non-blocking, returns on state change only

  Byte 0: GPIO input state bitmap
    bit 0 = GPIO1 input
    bit 1 = GPIO2 input  ← typical COR (RIM-Lite)
    bit 2 = GPIO3 input
    ...

  Config: cor_bit = 1, cor_polarity = active_high
  Returns -1 (EAGAIN) when no state change — NOT "COR off"
```

### COR Drop Hold Timer

```
  COR assert ──────────────────────────────────────────► Process immediately
  COR drop   ──► Start hold timer (cor_drop_hold ms)
                       │
                       ├── COR reasserts within hold ──► Cancel drop (silent)
                       │
                       └── Timer expires ──────────────► Fire COR_DROP event

  Purpose: DTMF tones interrupt CTCSS, causing RT97L to briefly drop COS.
  Without hold timer, each DTMF key press tears down the session.
  Default: 1000ms. Configurable 0-5000ms.
```

---

## WebSocket Audio Streaming

```
  Audio Thread              SPSC Ring              Flush Thread        Mongoose Thread
  ────────────              ─────────              ────────────        ───────────────

  RX tap ──► ws_ring_push ──►┌────────┐
  TX tap ──► ws_ring_push    │ 512    │◄── 5ms ── Check ring ──► mg_wakeup("A")
                             │ slots  │    cadence                      │
                             │        │                                 ▼
                             └────┬───┘                          MG_EV_WAKEUP
                                  │                                     │
                                  └─────────────── ws_flush_ring() ◄────┘
                                                        │
                                                        ▼
                                                  mg_ws_send() to all
                                                  connected clients

  Frame format (variable, depends on sample rate):
    [0]    = 0x01 (audio frame marker)
    [1]    = direction (0x00=RX, 0x01=TX)
    [2..3] = sequence number (uint16 LE)
    [4..N] = PCM16 samples (e.g. 960 samples = 1920 bytes at 48kHz)
    Server communicates sample rate dynamically; AudioWorklets adapt accordingly.

  Gates:
    RX tap: only pushes when is_receiving() (COR active)
    TX tap: only pushes when is_transmitting() (PTT active)
    → No audio when idle (no mic noise streaming)
```

---

## Module Lifecycle

```
  dlopen("modules/mod_foo.so")
       │
       ▼
  KERCHUNK_MODULE_DEFINE(mod_foo) ──► exports kerchunk_module_def_t
       │
       ▼
  mod_foo.load(core)
  ├── Receive core vtable pointer
  ├── Subscribe to events
  ├── Register DTMF commands (core->dtmf_register)
  └── Register CLI commands (via module_def_t)
       │
       ▼
  mod_foo.configure(cfg)
  ├── Read [section] from INI config
  ├── Create timers
  └── Initialize state
       │
       ▼
  ═══════ Running ═══════
       │
       ▼
  mod_foo.unload()
  ├── Unsubscribe from events
  ├── Unregister DTMF commands
  ├── Cancel timers
  └── Free resources
       │
       ▼
  dlclose()
```

### DTMF Command Registration

```
  Module load order matters: mod_dtmfcmd loads first, sets core->dtmf_register.
  Other modules call it during their load():

  mod_time.load():
    core->dtmf_register("95", 10, "Time check", "time_check")
                          │    │        │              │
                       pattern │   description    config_key
                          event offset (KERCHEVT_CUSTOM + 10)

  Config overrides in [dtmf] section:
    time_check = 95        ← default (or change to any pattern)

  mod_dtmfcmd accumulates digits: * → digits → # → dispatch
  Commands deferred until COR drops (user releases PTT)
```

---

## User & Group System

```
  [user.1]                    [group.1]
  username = bwest            name = Family
  name = Brian West           tx_ctcss = 1000
  callsign = WRDP519
  email = brian@bkw.org       TX Tone Resolution:
  dtmf_login = 101            ┌──────────────────────┐
  ani = 5551                  │ 1. User's group tone │
  access = 2                  │ 2. Repeater default  │
  group = 1                   └──────────────────────┘
  totp_secret = JBSWY...

  Identification methods:
  ├── DTMF ANI (automatic, radio sends digits on key-up)
  └── DTMF Login (*101# — session persists for login_timeout)

  Access levels: 0=none, 1=basic, 2=admin
```

---

## Web Dashboard

```
  Public (no auth):                    Admin (Bearer token):
  ├── index.html    Dashboard          ├── admin.html    Live events + controls
  ├── register.html Self-registration  ├── users.html    User/group CRUD
  ├── ptt.html      WebSocket PTT      ├── config.html   Live config editor
  └── coverage.html RF planner         └── coverage.html Coverage planner

  API Routes (auto-generated from CLI commands):
  ├── GET  /api/{command}    → dispatches CLI command, returns JSON
  ├── GET  /api/commands     → lists all available commands
  ├── GET  /api/status       → repeater state (public)
  ├── GET  /api/weather      → weather data (public)
  ├── GET  /api/nws          → NWS alerts (public)
  ├── GET  /api/events       → SSE event stream (admin)
  ├── WS   /api/audio        → WebSocket audio stream (public)
  ├── POST /api/cmd          → execute CLI command (admin)
  ├── POST /api/register     → self-registration (public)
  ├── POST /api/config/reload → reload config (admin)
  └── CRUD /api/users, /api/groups (admin)
```

---

## FCC Compliance

| Requirement | Implementation | Reference |
|-------------|----------------|-----------|
| Station ID every 15 min | CW ID interval capped at 900000ms | FCC 95.1751 |
| Voice ID | Speaks frequency + PL tone after CW ID | FCC 95.1751 |
| No unsolicited one-way | Auto-announce defaults off | FCC 95.1733 |
| Kerchunk filter | COR debounce (default 150ms) | — |
| Time-out timer | TOT (default 180s, max 600s) | — |
| Emergency override | `*911#` suppresses TOT + auto-announce | — |
| Activity logging | mod_recorder + mod_cdr | FCC 95.1705 |
| Quiet hours | CW ID suppressed during configurable window | — |

---

## DSP Pipeline (libplcode)

| Codec | Direction | Rate | Algorithm |
|-------|-----------|------|-----------|
| CTCSS encoder | TX | configurable (default 48kHz) | Sine oscillator, configurable amplitude |
| CTCSS decoder | RX | configurable (default 48kHz) | Goertzel multi-tone, 300ms early confirm |
| DCS encoder | TX | configurable (default 48kHz) | NRZ FSK at 134.4 bps, Golay(23,12) |
| DCS decoder | RX | configurable (default 48kHz) | Dual-path (normal + inverted), orbit mapping |
| DTMF encoder | TX | configurable (default 48kHz) | Dual-tone generation |
| DTMF decoder | RX | configurable (default 48kHz) | Goertzel, hysteresis state machine |
| CW ID encoder | TX | configurable (default 48kHz) | Morse at configurable WPM + frequency |
| Scrambler | RX/TX | configurable (default 48kHz) | Frequency inversion, self-inverse |

### DTMF Decoder State Machine

```
  IDLE ──► PENDING ──► ACTIVE ──► COOLDOWN ──► IDLE
   │        (2 blocks   (digit     (same-digit
   │         40ms)      reported)   blocked)
   │
   └── hits_to_begin: 2 blocks (40ms) to confirm onset
       misses_to_end: 10 blocks (200ms) to confirm offset
       min_off_frames: 8 blocks (160ms) same-digit cooldown

  Tuned for hardware repeaters where DTMF interrupts CTCSS,
  causing brief COS dropouts that would otherwise split one
  digit press into multiple detections.
```

---

## Configuration Hierarchy

```
  kerchunk.conf (main config, .gitignored)
  ├── [general]     Callsign, frequency, paths, coordinates
  ├── [modules]     Module path + load order
  ├── [audio]       PortAudio devices, sample_rate (default 48000), tx_encode, mixer levels
  ├── [hid]         HID device, COR bit/polarity, PTT GPIO pin
  ├── [repeater]    State machine timers, relay mode, TX tone, CW ID
  ├── [web]         HTTP/TLS, auth, PTT, registration
  ├── [caller]      Identification methods, ANI window, login timeout
  ├── [dtmf]        Inter-digit timeout, COR gate, pattern overrides
  ├── [voicemail]   Storage, limits
  ├── [weather]     API key, location, announce settings
  ├── [time]        Timezone, interval
  ├── [nws]         Location, severity, poll interval
  ├── [tts]         ElevenLabs API, voice, model, text normalization
  ├── [recording]   Directory, max duration
  ├── [emergency]   Auto-deactivate timeout
  ├── [otp]         Session timeout, time skew
  ├── [scrambler]   Code, frequency
  ├── [sdr]         Device, channel, log file
  ├── [webhook]     URL, secret, events
  ├── [stats]       Persistence
  ├── [courtesy]    Tone frequency, duration, amplitude
  ├── [gpio]        Allowed pins
  ├── [logger]      File, rotation size
  ├── [cdr]         Directory
  ├── [parrot]      Max duration
  ├── [pocsag]      POCSAG paging encoder settings
  ├── [flex]        FLEX paging encoder settings
  ├── [aprs]        APRS position reporting, RX/TX, beacon settings
  ├── [group.N]     Group name, TX CTCSS/DCS
  └── [user.N]      Username, callsign, email, ANI, login, access, group, TOTP

  users.conf (optional separate user DB, set via users_file in [general])
  └── [group.N] + [user.N] sections (web UI writes here)
```

---

## Build Artifacts

```
  kerchunkd           Daemon binary (links libplcode.a + PortAudio + ALSA)
  kerchunk            CLI binary (connects via Unix socket)
  modules/*.so        27 dynamically loaded modules
  test_kerchunk       Test binary (234 tests)
  libplcode           External dependency (CTCSS/DCS/DTMF/CW ID codec library)
  sounds/             WAV files: any sample rate (auto-resampled at load time)
  └── cache/tts/      TTS response cache (hash-keyed WAV files)
  web/                Static HTML/JS/CSS for dashboard
  tools/              Standalone test utilities (ptt_test, sdr_wb_capture, etc.)
```
