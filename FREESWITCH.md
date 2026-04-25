# FREESWITCH.md — AutoPatch Integration Plan for kerchunkd

```
   Status: DRAFT
   Last updated: 2026-03-28
```

## Table of Contents

- [1. What Is This?](#1-what-is-this)
- [2. Architecture Overview](#2-architecture-overview)
- [3. FreeSWITCH Integration Approach](#3-freeswitch-integration-approach)
  - [3.1. Why ESL + UnicastStream](#31-why-esl--unicaststream)
  - [3.2. How It Works](#32-how-it-works)
  - [3.3. ESL Client Implementation](#33-esl-client-implementation)
- [4. Audio Flow](#4-audio-flow)
  - [4.1. Radio-to-Phone (RX audio -> FreeSWITCH)](#41-radio-to-phone-rx-audio---freeswitch)
  - [4.2. Phone-to-Radio (FreeSWITCH -> TX audio)](#42-phone-to-radio-freeswitch---tx-audio)
  - [4.3. The Half-Duplex Dance](#43-the-half-duplex-dance)
- [5. Voice Activity Detection (VOX)](#5-voice-activity-detection-vox)
  - [5.1. Why We Need It](#51-why-we-need-it)
  - [5.2. Algorithm](#52-algorithm)
  - [5.3. Jitter Buffer](#53-jitter-buffer)
- [6. DTMF Command Integration](#6-dtmf-command-integration)
  - [6.1. New Command](#61-new-command)
  - [6.2. Call Flow](#62-call-flow)
  - [6.3. Safety Timers](#63-safety-timers)
- [7. Module Structure](#7-module-structure)
  - [7.1. Static Globals](#71-static-globals)
  - [7.2. Lifecycle](#72-lifecycle)
  - [7.3. The Tick Handler](#73-the-tick-handler)
  - [7.4. Audio Tap (Radio -> Phone)](#74-audio-tap-radio---phone)
- [8. Configuration](#8-configuration)
- [9. FreeSWITCH Server Configuration](#9-freeswitch-server-configuration)
- [10. Sound Files](#10-sound-files)
- [11. Legal Considerations](#11-legal-considerations)
  - [11.1. Amateur Radio (Part 97)](#111-amateur-radio-part-97)
  - [11.2. GMRS (Part 95 Subpart E)](#112-gmrs-part-95-subpart-e)
  - [11.3. Safety Features](#113-safety-features)
- [12. Implementation Phases](#12-implementation-phases)
  - [Phase 1: ESL Client + Call Control (no audio)](#phase-1-esl-client--call-control-no-audio)
  - [Phase 2: Audio Bridge](#phase-2-audio-bridge)
  - [Phase 3: VAD + Polish](#phase-3-vad--polish)
  - [Phase 4: Edge Cases](#phase-4-edge-cases)
- [13. Difficulty Assessment](#13-difficulty-assessment)
- [14. Estimated Size](#14-estimated-size)
- [15. The Payoff](#15-the-payoff)

## 1. What Is This?

AutoPatch connects a radio repeater to the telephone network. A repeater user
keys up, dials a DTMF sequence, and gets patched through to a phone call. The
person on the phone hears the radio user; the radio user hears the phone caller
over the repeater. When done, a DTMF hangup command tears it down.

This is old-school ham radio functionality (autopatch has existed since the
1970s) brought into the modern era: instead of dedicated POTS lines and
tone-controlled phone interfaces, we use **FreeSWITCH** as the telephony
engine. FreeSWITCH handles SIP trunking to a VoIP provider, call
origination/termination, and codec transcoding. kerchunkd handles the radio
side — PTT, audio capture, DTMF decoding, caller identification, and the
event bus that ties everything together.

The bridge between them is a new module: **mod_freeswitch.c**.

## 2. Architecture Overview

```
                    PSTN / VoIP Provider
                           │
                           │ SIP trunk
                           ▼
                   ┌───────────────┐
                   │  FreeSWITCH   │
                   │   (SIP UA)    │
                   │               │
                   │  ESL socket   │◄──── TCP 8021 (inbound ESL)
                   └──────┬────────┘
                          │
              ┌───────────┴───────────┐
              │  Audio    │  Control  │
              │  (L16     │  (ESL     │
              │  via UDP  │  commands │
              │  unicast) │  + events)│
              └─────┬─────┴─────┬─────┘
                    │           │
                    ▼           ▼
              ┌─────────────────────┐
              │   mod_freeswitch.c  │
              │                     │
              │  ╔══════════════╗   │
              │  ║ VAD engine   ║   │
              │  ║ (energy +    ║   │
              │  ║ hold timer)  ║   │
              │  ╚══════════════╝   │
              │                     │
              │  ╔══════════════╗   │
              │  ║ ESL client   ║   │
              │  ║ (TCP socket  ║   │
              │  ║  non-block)  ║   │
              │  ╚══════════════╝   │
              │                     │
              │  ╔══════════════╗   │
              │  ║ UDP audio    ║   │
              │  ║ send/recv    ║   │
              │  ╚══════════════╝   │
              └──────────┬──────────┘
                         │
                    kerchunk_core_t vtable
                         │
              ┌──────────┴──────────┐
              │      kerchunkd      │
              │                     │
              │  Audio thread (20ms)│ ◄── PortAudio capture/playback
              │  Main thread (20ms) │ ◄── HID COR/PTT, timers, socket
              └──────────┬──────────┘
                         │
                    USB HID (RIM-Lite v2)
                         │
                    ┌────┴────┐
                    │ RT-97L  │ ◄── Duplexer, separate TX/RX frequencies
                    │Repeater │
                    └─────────┘
```

## 3. FreeSWITCH Integration Approach

### 3.1. Why ESL + UnicastStream

| Approach | Verdict |
|----------|---------|
| **mod_portaudio** | Requires FreeSWITCH to share audio hardware. Useless — PortAudio belongs to kerchunkd. |
| **Custom FS module** | Max control, massive coupling. Requires FS source tree. Overkill. |
| **mod_shout / streaming** | Wrong abstraction for bidirectional real-time audio. |
| **Named pipes / FIFO** | Works one direction. Bidirectional real-time with low latency is painful. |
| **ESL + UnicastStream** | FS sends/receives raw L16 audio over UDP. ESL provides call control. Clean separation. Well-documented. **This is the answer.** |

### 3.2. How It Works

**ESL (Event Socket Library)** — FreeSWITCH's TCP-based control protocol.
mod_freeswitch connects as an inbound client to port 8021. Through ESL it can:

- Originate calls: `bgapi originate {options}sofia/gateway/provider/15551234567 &park()`
- Hang up calls: `api uuid_kill <uuid>`
- Subscribe to events: `event plain CHANNEL_ANSWER CHANNEL_HANGUP DTMF`
- Set up audio: `api uuid_unicast <uuid> <local_ip> <rx_port> <tx_port> mono 8000`

**UnicastStream** (`uuid_unicast`) tells FreeSWITCH to send the call's audio as
raw L16 (16-bit signed, host byte order) UDP packets to a specified IP:port, and
receive audio back on another port. Bidirectional 8kHz 16-bit mono PCM over
UDP — exactly what we need.

### 3.3. ESL Client Implementation

Minimal, non-blocking TCP client. We do NOT link against libesl because:

1. It drags in build dependencies
2. It uses blocking I/O by default
3. The ESL text protocol is trivial to parse

The protocol is line-oriented. Auth: `auth <password>\n\n`. Commands:
`api <command>\n\n`. Responses: `Content-Type: api/response\nContent-Length: N\n\n<body>`.

~300 lines of C: connect, authenticate, send commands, parse responses,
subscribe to events, dispatch. Non-blocking sockets polled from kerchunkd's
main thread (20ms tick).

## 4. Audio Flow

### 4.1. Radio-to-Phone (RX audio -> FreeSWITCH)

```
  Radio user keys up (COR assert)
        │
        ▼
  Audio thread captures frame (160 samples / 20ms)
        │
        ▼
  kerchunk_core_dispatch_taps() calls tap handlers
        │
        ▼
  mod_freeswitch radio_audio_tap() receives frame
        │
        ▼
  sendto() to FreeSWITCH's UnicastStream receive port
        │
        ▼
  Phone user hears radio user
```

Straightforward. The audio tap mechanism already works (mod_recorder uses it).
160 samples at 8kHz = 20ms frame = 320 bytes. Matches UnicastStream's expected
packet size. No reframing needed.

### 4.2. Phone-to-Radio (FreeSWITCH -> TX audio)

```
  Phone user speaks
        │
        ▼
  FreeSWITCH sends L16 UDP packets
        │
        ▼
  mod_freeswitch recvfrom() in tick handler (20ms poll)
        │
        ▼
  Samples land in jitter buffer (ring buffer, ~100ms)
        │
        ▼
  VAD analyzes: is the phone user talking?
        │
        ├── YES: voice activity detected
        │         │
        │         ▼
        │    If PTT not held -> request_ptt("freeswitch")
        │         │
        │         ▼
        │    queue_audio_buffer(samples, n, KERCHUNK_PRI_ELEVATED)
        │         │
        │         ▼
        │    Audio thread drains -> PortAudio -> radio TX
        │         │
        │         ▼
        │    Phone user's voice transmits over repeater
        │
        └── NO: silence detected
                  │
                  ▼
             Hold timer (configurable, e.g. 500ms)
                  │
                  ├── Timer expires -> release_ptt("freeswitch")
                  │
                  └── Voice resumes -> cancel timer, continue TX
```

mod_freeswitch holds its own PTT ref via `request_ptt("freeswitch")` controlled
by the VAD. While holding PTT, it feeds audio buffers into the queue at `KERCHUNK_PRI_ELEVATED` (3)
(above time/weather, below CW ID and emergency). When the VAD
declares silence, the PTT ref is released after the hold timer expires.

Same refcounted PTT pattern used by mod_repeater. Multiple PTT holders coexist.

### 4.3. The Half-Duplex Dance

The repeater hardware is full-duplex (separate TX/RX via duplexer). But the
radio *user* is half-duplex — when they press PTT, their radio transmits and
cannot simultaneously receive.

```
  Radio user:  ████ TX ████           ████ TX ████
  COR signal:  ████████████           ████████████

  Phone audio:           ░░░░ speak ░░░░         ░░░░
  to repeater:           ░░░░░░░░░░░░░░░         ░░░░
  (muted):     XXXX                  XXXX

  Radio->Phone:████████████           ████████████
  (always)     (phone is full-duplex, always hears radio)
```

**Rules:**

1. **COR active (radio user talking):** Radio audio goes to phone. Phone audio
   is MUTED on repeater TX (radio user can't hear while keyed anyway, and it
   would create echo/feedback).

2. **COR inactive (radio user listening):** Phone audio goes to repeater TX
   via VAD. Radio user hears phone caller.

3. **Both silent:** Nothing transmits. Repeater idle. PTT released.

mod_freeswitch subscribes to `KERCHEVT_COR_ASSERT` and `KERCHEVT_COR_DROP`, uses a
`g_cor_active` flag to gate phone-to-radio audio.

## 5. Voice Activity Detection (VOX)

### 5.1. Why We Need It

Without VAD, the repeater transmits for the entire phone call — even during
silence, hold music gaps, or the phone user listening. This blocks other
repeater users, wastes TX power, generates unnecessary RF, and potentially
violates FCC regulations around unnecessary transmissions.

### 5.2. Algorithm

Simple energy-based VAD. Telephone audio is bandlimited (300-3400 Hz) and
relatively clean. We don't need WebRTC-grade VAD.

```c
#define VAD_FRAME_SAMPLES       160    /* 20ms at 8kHz */
#define VAD_THRESHOLD_DEF       800    /* RMS threshold (configurable) */
#define VAD_HOLD_MS_DEF         500    /* Hold time after last speech */
#define VAD_ATTACK_FRAMES_DEF   5      /* Require N consecutive speech frames */
#define PREROLL_FRAMES          10     /* 200ms ring of pre-VAD audio */

static int compute_rms(const int16_t *buf, size_t n)
{
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += (int64_t)buf[i] * buf[i];
    return (int)sqrt((double)sum / (double)n);
}
```

**Configurable parameters:**

| Parameter | Default | Description |
|-----------|---------|-------------|
| `vad_threshold` | `800` | RMS energy threshold for speech detection |
| `vad_hold_ms` | `500ms` | Hold time after last speech before PTT release |
| `vad_attack_frames` | `5` | Consecutive 20 ms frames required before VAD asserts speech (raised from 2 — fewer false-trigger PTT cycles on phone-line clicks) |
| `phone_gain` | `0.5` | Per-sample gain (0.0–2.0) applied to phone audio before upsample. `0.5` (–6 dB) keeps SignalWire/FS-side hot feeds clear of clipping at the radio. |

**Pre-roll buffer.** Because `vad_attack_frames` defaults to 5, the
VAD swallows the first ~80 ms of an utterance before declaring
speech and asserting PTT. To preserve the leading edge,
mod_freeswitch keeps a 10-frame (200 ms) ring of every jitter-buffer
read regardless of VAD verdict, and replays it into the queue at
the moment of PTT engage. The listener hears the actual phoneme
onset, not the audio that arrived after VAD finally settled.

### 5.3. Jitter Buffer

UDP packets can arrive out of order or with jitter. A small ring buffer
(100-200ms = 800-1600 samples) smooths this out:

- Incoming UDP packets write into the ring
- mod_freeswitch reads at 20ms tick rate
- Underflow: substitute silence (VAD detects it, eventually releases PTT)
- Overflow: drop oldest samples (prevents unbounded latency)

```c
#define JITTER_BUF_SAMPLES  1600    /* 200ms at 8kHz */

typedef struct {
    int16_t buf[JITTER_BUF_SAMPLES];
    size_t  write_pos;
    size_t  read_pos;
    size_t  count;
} jitter_buf_t;
```

## 6. DTMF Command Integration

### 6.1. New Command

mod_freeswitch self-registers its DTMF command during `load()` using the
dynamic registration API:

```c
#define DTMF_EVT_AUTOPATCH 17  /* next available offset */

static int freeswitch_load(kerchunk_core_t *core)
{
    g_core = core;
    if (core->dtmf_register)
        core->dtmf_register("0", DTMF_EVT_AUTOPATCH, "AutoPatch", "autopatch");
    core->subscribe(KERCHEVT_CUSTOM + DTMF_EVT_AUTOPATCH, on_autopatch, NULL);
    /* ... */
}
```

The pattern `*0<digits>#` dials, `*0#` hangs up. The config key `autopatch`
allows the operator to remap the DTMF pattern in `[dtmf]`:

```ini
[dtmf]
autopatch = 0     ; default, or change to any pattern
```

mod_freeswitch differentiates on the argument:

```c
static void on_autopatch(const kerchevt_t *evt, void *ud)
{
    const char *arg = (const char *)evt->custom.data;
    if (!arg || arg[0] == '\0')
        autopatch_hangup();        /* *0#             */
    else
        autopatch_dial(arg);       /* *0<digits>#     */
}
```

### 6.2. Call Flow

```
  User dials: * 0 5 5 5 1 2 3 4 5 6 7 #
        │
        ▼
  mod_dtmfcmd fires KERCHEVT_CUSTOM+13 with arg="5551234567"
        │
        ▼
  mod_freeswitch autopatch_dial("5551234567")
        │
        ▼
  ESL: bgapi originate sofia/gateway/provider/15551234567 &park()
        │
        ▼
  Play "phone/phone_dialing.wav" on repeater
        │
        ├── CHANNEL_PROGRESS -> play ringback / "phone/phone_ringing.wav"
        ├── CHANNEL_HANGUP USER_BUSY -> "phone/phone_busy.wav", reset
        ├── CHANNEL_HANGUP NO_ANSWER -> "phone/phone_no_answer.wav", reset
        └── CHANNEL_ANSWER:
              │
              ▼
         uuid_unicast <uuid> <ip> <rx_port> <tx_port> mono 8000
              │
              ▼
         Register audio tap (radio -> phone)
         Start UDP receive (phone -> radio)
         Play "phone/phone_connected.wav"
         Start call duration timer
              │
              ▼
         ═══ CALL IN PROGRESS ═══
              │
              ▼
  User dials: * 0 #
        │
        ▼
  ESL: api uuid_kill <uuid>
  Unregister audio tap, close UDP, release PTT
  Play "phone/phone_disconnected.wav"
```

### 6.3. Safety Timers

| Timer | Default | Purpose |
|-------|---------|---------|
| `max_call_duration` | 180s | Hard cap. Prevents abandoned/stuck calls. |
| `dial_timeout` | 30s | How long to wait for answer. |
| `inactivity_timeout` | 60s | Hangup if no radio user audio for this long. |

## 7. Module Structure

### 7.1. Static Globals

```c
/* Core */
static kerchunk_core_t *g_core;
#define DTMF_EVT_AUTOPATCH 17

/* Config */
static char g_fs_host[64]         = "127.0.0.1";
static int  g_fs_esl_port         = 8021;
static char g_fs_esl_password[64] = "ClueCon";
static char g_sip_gateway[64]     = "provider";
static int  g_udp_base_port       = 16000;
static int  g_max_call_secs       = 180;
static int  g_dial_timeout_ms     = 30000;
static int  g_inactivity_ms       = 60000;
static int  g_vad_threshold       = 800;
static int  g_vad_hold_ms         = 500;
static int  g_enabled             = 0;
static int  g_admin_only          = 0;
static char g_dial_prefix[16]     = "1";
static char g_dial_whitelist[256] = "";  /* Comma-separated, empty = all */

/* Call state */
static int  g_call_active;
static char g_call_uuid[64];
static int  g_call_timer       = -1;
static int  g_inactivity_timer = -1;
static int  g_dial_timer       = -1;

/* ESL */
static int  g_esl_fd = -1;
static int  g_esl_connected;
static int  g_esl_authed;
static char g_esl_buf[4096];
static int  g_esl_buf_len;

/* UDP audio */
static int  g_udp_rx_fd = -1;
static int  g_udp_tx_fd = -1;

/* Audio state */
static int  g_cor_active;
static int  g_vox_ptt_held;
static int  g_vad_hold_remaining;
static int  g_speech_frames;
static jitter_buf_t g_jitter;
```

### 7.2. Lifecycle

```c
static int freeswitch_load(kerchunk_core_t *core)
{
    g_core = core;
    if (core->dtmf_register)
        core->dtmf_register("0", DTMF_EVT_AUTOPATCH, "AutoPatch", "autopatch");
    core->subscribe(KERCHEVT_CUSTOM + DTMF_EVT_AUTOPATCH, on_autopatch, NULL);
    core->subscribe(KERCHEVT_COR_ASSERT,  on_cor_assert, NULL);
    core->subscribe(KERCHEVT_COR_DROP,    on_cor_drop, NULL);
    core->subscribe(KERCHEVT_TICK,        on_tick, NULL);
    core->subscribe(KERCHEVT_SHUTDOWN,    on_shutdown, NULL);
    return 0;
}
```

### 7.3. The Tick Handler

```c
static void on_tick(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) return;

    esl_poll();                          /* 1. ESL events/responses */

    if (g_call_active && g_udp_rx_fd >= 0)
        udp_receive_audio();             /* 2. Phone audio from UDP */

    if (g_call_active && !g_cor_active)
        vox_process_and_queue();         /* 3. VAD + queue to repeater */
}
```

### 7.4. Audio Tap (Radio -> Phone)

```c
static void radio_audio_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_call_active || !g_cor_active || g_udp_tx_fd < 0)
        return;

    /* Send directly via UDP — fast, non-blocking */
    sendto(g_udp_tx_fd, evt->audio.samples,
           evt->audio.n * sizeof(int16_t), MSG_DONTWAIT,
           &g_fs_udp_addr, g_fs_udp_addrlen);
}
```

Runs in the audio thread (20ms). sendto() on loopback takes microseconds.
Use `MSG_DONTWAIT` — dropping a frame beats stalling the audio thread.

## 8. Configuration

```ini
[freeswitch]
enabled = off
freeswitch_host = 127.0.0.1
esl_port = 8021
esl_password = ClueCon
sip_gateway = voip_provider
udp_base_port = 16000
dial_prefix = 1
dial_whitelist =             ; empty = allow all, or: 918,539,405
max_call_duration = 3m
dial_timeout = 30s
inactivity_timeout = 60s
vad_threshold = 800          ; RMS floor to qualify as speech
vad_hold_ms = 500ms          ; keep PTT this long after last speech frame
vad_attack_frames = 5        ; consecutive 20 ms frames before VAD asserts
phone_gain = 0.5             ; phone audio gain (0.0..2.0); 0.5 = -6 dB
admin_only = off
```

## 9. FreeSWITCH Server Configuration

### ESL (autoload_configs/event_socket.conf.xml)

```xml
<configuration name="event_socket.conf" description="Socket Client">
  <settings>
    <param name="listen-ip" value="127.0.0.1"/>
    <param name="listen-port" value="8021"/>
    <param name="password" value="ClueCon"/>
  </settings>
</configuration>
```

### SIP Gateway (sip_profiles/external/provider.xml)

```xml
<include>
  <gateway name="voip_provider">
    <param name="realm" value="sip.provider.com"/>
    <param name="username" value="your_account"/>
    <param name="password" value="your_password"/>
    <param name="register" value="true"/>
  </gateway>
</include>
```

No special dialplan needed — we use `&park()` on originate and control
everything via ESL.

## 10. Sound Files

New directory: `sounds/phone/`

```
sounds/phone/
  phone_dialing.wav          "Dialing"
  phone_ringing.wav          "Ringing"
  phone_connected.wav        "Connected"
  phone_disconnected.wav     "Call ended"
  phone_busy.wav             "The number is busy"
  phone_no_answer.wav        "No answer"
  phone_error.wav            "Call failed"
  phone_timeout.wav          "Maximum call time reached"
  phone_access_denied.wav    "Access denied"
  phone_not_available.wav    "AutoPatch is not available"
```

## 11. Legal Considerations

### 11.1. Amateur Radio (Part 97)

AutoPatch on ham repeaters is **well-established and legal**, with restrictions:

- **97.113(a)(3):** No business calls. No pecuniary interest.
- **97.113(a)(5):** Third-party traffic rules apply. Phone party is a "third
  party" — control operator remains responsible.
- **97.115:** Third-party communications permitted domestically and with
  countries that have third-party agreements.
- **No encryption over the air.** SIP trunk may use TLS/SRTP on the PSTN
  side, but RF transmissions must be in the clear.
- **Station ID** per 97.119 still required (mod_cwid handles this).
- **Control operator** must be able to kill the patch (admin DTMF or CLI).

**Bottom line:** Go for it. Autopatch is as traditional as repeaters themselves.

### 11.2. GMRS (Part 95 Subpart E)

More restrictive, regulatory picture is murkier:

- **95.1733:** GMRS stations may transmit "two-way plain language voice
  communications." Interconnection is not explicitly addressed in Part 95E
  the way it is in Part 97.
- **95.1705:** GMRS is for "personal or business" communications by the
  licensee and immediate family.
- **FCC has been silent** on GMRS autopatch. Pre-2017 rules had
  interconnection provisions. The 2017 reform (FCC 17-76) simplified GMRS
  and removed detailed provisions but did not explicitly prohibit it.
- **No encryption** (95.333 general Part 95 prohibition applies).

**Bottom line:** Legality is ambiguous. The FCC has never enforcement-actioned
a GMRS autopatch. A configurable `service_type = ham|gmrs` setting could
disable autopatch on GMRS by default, with the operator making the conscious
choice.

### 11.3. Safety Features

Regardless of service type:

1. **Call duration limit** (default 3 min) — prevents runaway calls
2. **Dial whitelist** — restrict callable numbers (area codes, etc.)
3. **Admin-only mode** — restrict to users with admin access level
4. **Activity logging** — all usage logged (who, when, duration, destination)
5. **911 blocking** — SIP trunks lack E911 location data. Play warning.
6. **Kill switch** — Admin can terminate any call via `*0#` or CLI

## 12. Implementation Phases

### Phase 1: ESL Client + Call Control (no audio)

- Minimal ESL TCP client in mod_freeswitch.c
- Non-blocking connect, auth, command/response, event subscription
- autopatch_dial() / autopatch_hangup() via ESL
- DTMF command registration (`*0<digits>#` / `*0#`)
- Config parsing for `[freeswitch]` section
- Sound file prompts
- Call state machine (IDLE -> DIALING -> RINGING -> CONNECTED -> IDLE)
- Safety timers
- CLI command: `freeswitch` (show status)

**Testable without audio.** Mock infrastructure covers call control.

### Phase 2: Audio Bridge

- UDP socket pair for UnicastStream
- `uuid_unicast` setup after CHANNEL_ANSWER
- Radio-to-phone via audio tap + UDP sendto
- Phone-to-radio via UDP recvfrom + jitter buffer + queue_audio_buffer
- COR gating (mute phone audio while radio user is keyed)

**First end-to-end test.** Make a call and talk bidirectionally.

### Phase 3: VAD + Polish

- Energy-based VAD with hold timer and attack counter
- Inactivity timeout
- Dial whitelist enforcement
- Access control (admin_only)
- Activity logging
- 911 blocking with warning
- Integration tests

### Phase 4: Edge Cases

- Audio tap thread safety (tap in audio thread, UDP send is safe)
- ESL reconnection on FreeSWITCH restart
- Concurrent events (COR during phone audio, DTMF during call)
- Queue interaction (CW ID fires during active call)
- Emergency mode interaction (kill autopatch during emergency?)
- Stress test long calls, rapid dial/hangup

## 13. Difficulty Assessment

### Straightforward

- **DTMF command registration.** One line in mod_dtmfcmd, one subscription.
- **Audio tap.** mod_recorder proves the mechanism. Just add sendto().
- **Call state machine.** Finite states, well-defined transitions, easy to mock-test.
- **Config and CLI.** Copy existing module pattern.

### Moderate

- **ESL client.** Simple protocol but robust non-blocking TCP with reconnection,
  partial reads, and multi-line response parsing. ~400 lines of careful C.
- **VAD tuning.** Empirical. Too sensitive = noise triggers TX. Too insensitive =
  speech gets clipped. Hold timer helps but real calls needed for tuning.
- **Jitter buffer.** Simple ring buffer but initial fill level matters. Start
  with 80ms.

### Tricky

- **Thread safety.** Audio tap runs in audio thread (5ms), ESL/UDP in main thread
  (20ms). The sendto() in the tap doesn't share state. `g_call_active` and
  `g_cor_active` are read by the tap and written by main — simple int flags,
  worst case is one extra frame sent/not-sent. Use volatile to be proper.
- **Queue PTT interaction.** mod_freeswitch and the audio thread both manage
  independent PTT refs (refcounted). They coexist correctly but ordering
  matters during CW ID playback. Verify in testing.
- **UnicastStream quirks.** If unreliable, fallback is direct RTP with minimal
  framing (~200 extra lines). PCMU codec for compatibility.

## 14. Estimated Size

| Component | Lines | Difficulty |
|-----------|-------|------------|
| ESL client (non-blocking TCP) | ~400 | Moderate |
| Call state machine | ~200 | Easy |
| DTMF integration | ~30 | Easy |
| UDP audio send/receive | ~150 | Easy |
| Jitter buffer | ~100 | Easy |
| VAD engine | ~80 | Moderate (tuning) |
| COR gating + PTT management | ~100 | Easy |
| Config + CLI | ~120 | Easy |
| Sound file prompts | ~80 | Easy |
| Safety features + logging | ~150 | Easy |
| **Total mod_freeswitch.c** | **~1400** | **Moderate** |

Plus ~300 lines of integration tests and a few lines in mod_dtmfcmd.c.

## 15. The Payoff

Dial `*05551234567#` on your radio, hear it ring, talk to someone on the
phone through your repeater, and hang up with `*0#`. Autopatch, alive and
well in 2026.
