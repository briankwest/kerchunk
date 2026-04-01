# mod_aprs -- APRS Position Reporting and Packet Decoding

## Overview

mod_aprs integrates [libaprs](https://github.com/briankwest/libaprs) into kerchunkd for APRS position reporting and packet decoding on GMRS and HAM repeaters. Two modes: **receive** (decode APRS packets from SDR) and **transmit** (send short position/telemetry beacons via the repeater's TX audio path).

> **Status:** Implemented and working. mod_aprs is built when libaprs is detected by pkg-config. Load it by adding `mod_aprs` to the module load list in `[modules]`.
>
> **Callsign format:** AX.25 callsigns are limited to 6 alphanumeric characters plus a 4-bit SSID (0-15). The callsign from `[general]` is used directly. Ensure it fits the 6-character AX.25 limit.

## Table of Contents

- [1. Architecture](#1-architecture)
- [2. RX Path -- Decode APRS Packets from SDR](#2-rx-path----decode-aprs-packets-from-sdr)
- [3. TX Path -- Beacon Position/Telemetry](#3-tx-path----beacon-positiontelemetry)
- [4. FCC Compliance](#4-fcc-compliance)
- [5. Configuration Reference](#5-configuration-reference)
- [6. Changes Required](#6-changes-required)
- [7. Dependencies](#7-dependencies)
- [8. Web Dashboard Integration](#8-web-dashboard-integration)
- [9. CLI Commands](#9-cli-commands)
- [10. DTMF Commands](#10-dtmf-commands)
- [11. Testing](#11-testing)
- [12. Future Work](#12-future-work)

## 1. Architecture

```
┌───────────────────────────────────────────────────────────┐
│                      mod_aprs.c                           │
│                                                           │
│  ┌───────────────────┐        ┌─────────────────────────┐ │
│  │    RX Decoder     │        │     TX Beacon           │ │
│  │                   │        │                         │ │
│  │  mod_sdr demod    │        │  Timer fires            │ │
│  │  audio callback   │        │  → check COR (busy?)    │ │
│  │  ↓                │        │  → build aprs_packet_t  │ │
│  │  afsk_demod_feed()│        │  → ax25_encode_ui_frame │ │
│  │  ↓                │        │  → afsk_mod_frame       │ │
│  │  ax25_decode_ui   │        │  → queue_audio_raw()    │ │
│  │  ↓                │        │  (core handles PTT)     │ │
│  │  aprs_packet_t    │        │                         │ │
│  │  ↓                │        └─────────────────────────┘ │
│  │  log + event +    │                                    │
│  │  web dashboard    │                                    │
│  └───────────────────┘                                    │
└───────────────────────────────────────────────────────────┘
```

The RX and TX paths are independent. Either can be enabled without the other.

## 2. RX Path -- Decode APRS Packets from SDR

mod_sdr already produces 8 kHz 16-bit mono PCM from its FM demodulator. mod_aprs hooks into that audio stream and feeds it through libaprs's AFSK 1200 demodulator.

### Data Flow

1. mod_sdr exposes a demod audio callback (new hook -- currently audio is internal only)
2. mod_aprs creates an `afsk_demod_t` at 8000 Hz during `load()`
3. On each SDR audio frame, samples are fed to `afsk_demod_feed()`
4. When a complete frame is detected, the callback fires:
   - `ax25_decode_ui_frame()` parses the AX.25 frame
   - `ax25_to_aprs()` populates a structured `aprs_packet_t`
5. The decoded packet is:
   - Logged to CSV (`aprs_rx.csv`)
   - Fired as `KERCHEVT_APRS_PACKET` for other modules
   - Stored in a ring buffer for the web dashboard

### SDR Hook

mod_sdr needs a small addition -- a registered callback that receives the demodulated PCM buffer after FM demod + de-emphasis:

```c
/* In mod_sdr.c, after FM demod fills audio_buf: */
if (g_sdr_audio_cb)
    g_sdr_audio_cb(audio_buf, audio_pos, g_sdr_audio_ud);
```

mod_aprs registers this callback during `load()` if mod_sdr is present.

### Requires

- mod_sdr loaded and monitoring the target channel
- libaprs installed (provides `afsk_demod_t`)

## 3. TX Path -- Beacon Position/Telemetry

Build an APRS position packet, modulate to AFSK 1200 audio, and queue into the core audio queue. The core handles PTT automatically.

### Data Flow

1. A timer fires every `tx_interval` seconds
2. Check COR -- **do not transmit** if channel is busy (yield to voice)
3. Build an `aprs_packet_t` with:
   - Callsign from `[general]` config
   - Latitude/longitude/elevation from `[general]` config
   - Symbol `/r` (repeater) or configurable
   - Optional comment string
4. Encode: `ax25_encode_ui_frame()` → `afsk_mod_frame()` at 8000 Hz
5. Queue the resulting `int16_t` samples via `core->queue_audio_raw()` at `KERCHUNK_PRI_LOW`
6. CW ID handles station identification (already running per FCC requirements)

### Packet Duration

A typical APRS position packet at 1200 baud with 25-byte preamble:

- Preamble: ~170 ms (25 flag bytes)
- AX.25 header (14 bytes) + payload (~40 bytes) + FCS: ~380 ms
- Postamble: ~20 ms
- **Total: ~570 ms** per beacon

Well within the "brief transmission" requirement for GMRS.

## 4. FCC Compliance

### GMRS (Part 95 Subpart E)

| Rule | Requirement | How mod_aprs Complies |
|------|------------|----------------------|
| 95.1731 | Short-duration data permitted | Beacon is ~570 ms |
| 95.1733 | One-way data must be brief, yield to voice | COR check before TX; long beacon interval |
| 95.1723 | No encryption/obscuring meaning | Plain AX.25/APRS, human-readable payload |
| 95.1705 | Station identification required | Callsign in packet + CW ID every 15 min |
| 95.1751 | ID at least every 15 min and at end | mod_cwid already handles this |

**Conservative defaults for GMRS:**
- `tx_interval = 600` (10 minutes)
- `tx_enabled = off` by default
- COR gating prevents transmitting over voice

### HAM (Part 97)

No restrictions on data mode, interval, or content. APRS is a well-established Part 97 practice. Recommended defaults:
- `tx_interval = 120` (2 minutes, standard APRS)
- Digipeater path: `WIDE1-1,WIDE2-1`

### Part 90 (Business)

Data transmissions are permitted. Follow operator's license terms.

## 5. Configuration Reference

```ini
[aprs]
; Master enable
enabled = off

; ── RX (decode packets from SDR audio) ──
rx_enabled = on
rx_log_file = aprs_rx.csv

; ── TX (beacon position/telemetry) ──
tx_enabled = off
tx_interval = 600          ; seconds between beacons (600 for GMRS, 120 for HAM)
tx_path = WIDE1-1          ; digipeater path (comma-separated)
tx_comment = kerchunkd repeater
tx_symbol = /r             ; APRS symbol table/code
```

Position data is read from `[general]`:

```ini
[general]
callsign = WRDP519
latitude = 34.9513
longitude = -95.7697
elevation = 780
```

## 6. Changes Required

| File | Change | Est. Lines |
|------|--------|-----------|
| `modules/mod_aprs.c` | New module: RX decoder, TX beacon, config, lifecycle | ~500 |
| `modules/mod_sdr.c` | Add demod audio callback hook for external consumers | ~20 |
| `include/kerchunk.h` | Add `KERCHEVT_APRS_PACKET` event constant | ~5 |
| `configure.ac` | `PKG_CHECK_MODULES([APRS], [aprs])` optional dependency | ~5 |
| `Makefile.am` | Add `mod_aprs.la`, conditional on `HAVE_APRS` | ~5 |

### mod_aprs.c Structure

```c
/* Lifecycle */
static int  aprs_load(kerchunk_core_t *core);
static void aprs_configure(kerchunk_config_t *cfg);
static void aprs_unload(void);

/* RX */
static void sdr_audio_cb(int16_t *buf, size_t n, void *ud);
static void on_frame_decoded(const uint8_t *frame, size_t len, void *ud);

/* TX */
static void on_beacon_timer(const kerchevt_t *evt, void *ud);
static void send_beacon(void);

/* DTMF */
static void on_aprs_cmd(const kerchevt_t *evt, void *ud);

KERCHUNK_MODULE_DEFINE(mod_aprs);
```

## 7. Dependencies

| Dependency | Required For | Install |
|-----------|-------------|---------|
| libaprs | Both RX and TX | `sudo dpkg -i libaprs-dev_*.deb` or `make install` from source |
| mod_sdr + librtlsdr | RX path only | `apt install librtlsdr-dev` |

TX-only mode works without an SDR -- it just generates and queues audio.

## 8. Web Dashboard Integration

- **`/api/aprs`** -- JSON list of recently decoded packets (ring buffer, last 100)
- **`/api/aprs/status`** -- Beacon status (last TX time, packets decoded, TX/RX counts)
- **Map overlay** -- decoded station positions plotted on the existing Google Maps coverage page
- **Activity table** -- callsign, position, timestamp, packet type, SSID

## 9. CLI Commands

```
kerchunk> aprs beacon                              # Force immediate position beacon
kerchunk> aprs send "Hello from the repeater"      # Send an APRS message
kerchunk> aprs status                              # Show APRS status (packets decoded, last beacon, TX/RX counts)
```

## 10. DTMF Commands

mod_aprs self-registers via `core->dtmf_register()`:

| Command | Action |
|---------|--------|
| `*98#` | Force immediate beacon (TX if enabled) |
| `*980#` | Report APRS status (packets decoded, last beacon time) |

## 11. Testing

| Test | Description |
|------|------------|
| Round-trip encode/decode | Build packet → modulate → demodulate → verify fields match |
| Synthetic RX | Feed pre-generated AFSK audio into `sdr_audio_cb`, verify decode |
| COR gating | Mock COR assert before beacon timer, verify beacon deferred |
| Config parsing | Verify all `[aprs]` keys parsed with correct defaults |
| TX audio output | Verify modulated audio is correct sample rate, 16-bit mono, correct duration |

## 12. Future Work

- **APRS-IS gateway** -- forward decoded packets to the APRS Internet System (libaprs already has `aprs_is_connect()`)
- **Two-way messaging** -- receive and respond to APRS messages via DTMF or web UI
- **Weather telemetry** -- include mod_weather data in beacon packets
- **Multi-channel RX** -- monitor multiple channels simultaneously with multiple SDRs
- **Digipeater mode** -- relay heard packets (HAM only, requires Part 97 license)
- **Signal strength mapping** -- log RSSI from SDR with decoded positions for coverage analysis
