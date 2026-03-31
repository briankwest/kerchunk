# kerchunkd Usage Guide

A complete guide for radio users and administrators of a kerchunkd-powered GMRS repeater.

## Table of Contents

- [Part 1: For Radio Users](#part-1-for-radio-users)
  - [What is kerchunkd?](#what-is-kerchunkd)
  - [Programming Your Radio](#programming-your-radio)
  - [How You Are Identified](#how-you-are-identified)
  - [Using DTMF Commands](#using-dtmf-commands)
  - [Weather and Time](#weather-and-time)
  - [Echo Test (Parrot)](#echo-test-parrot)
  - [Voicemail](#voicemail)
  - [Emergency Mode](#emergency-mode)
  - [OTP Authentication](#otp-authentication)
  - [What You Will Hear](#what-you-will-hear)
- [Part 2: For Administrators](#part-2-for-administrators)
  - [Prerequisites](#prerequisites)
  - [Building from Source](#building-from-source)
  - [Linux Setup](#linux-setup)
  - [Initial Configuration](#initial-configuration)
  - [Configuration Reference](#configuration-reference)
  - [FCC Compliance Settings](#fcc-compliance-settings)
  - [User and Group Management](#user-and-group-management)
  - [Web Dashboard](#web-dashboard)
  - [CLI Tool](#cli-tool)
  - [Monitoring and Maintenance](#monitoring-and-maintenance)
  - [Troubleshooting](#troubleshooting)
  - [Running as a System Service](#running-as-a-system-service)

---

## Part 1: For Radio Users

This section is for anyone using the repeater over the air with a GMRS radio.

### What is kerchunkd?

kerchunkd is the software that controls this GMRS repeater. Compared to a basic repeater, it provides features you can access from your radio using DTMF tones (the sounds your radio makes when you press number keys):

- **Caller identification** -- the repeater knows who is transmitting
- **Weather reports and forecasts** -- on-demand via DTMF
- **Time announcements** -- check the current time
- **NWS weather alerts** -- severe weather notifications
- **Voicemail** -- leave and retrieve voice messages
- **Audio echo test** -- hear what your signal sounds like
- **Emergency mode** -- extended transmit for emergencies
- **Two-factor authentication** -- secure access for privileged functions

All of these features are accessed by pressing DTMF keys on your radio while transmitting.

### Programming Your Radio

To use this repeater, you need to program your radio with the correct settings. Ask the repeater administrator for the following information:

- **Output frequency** -- the frequency you listen on (e.g., 462.550 MHz)
- **Input offset** -- the offset for your transmit frequency (e.g., +5.000 MHz)
- **CTCSS tone (PL tone)** -- a sub-audible tone required to access the repeater (e.g., 131.8 Hz)

Program a channel in your radio with:

1. **Receive frequency**: the repeater's output frequency
2. **Transmit offset**: typically +5.000 MHz for GMRS repeaters
3. **CTCSS encode tone**: the PL tone the repeater expects to hear from you
4. **CTCSS decode tone**: the PL tone the repeater transmits (optional -- helps reduce noise)

If the repeater administrator has assigned you a personal CTCSS tone for identification, use that tone as your encode tone.

### How You Are Identified

The repeater can identify who is transmitting using several methods. When you are identified, the repeater can provide personalized features like voicemail and per-user settings.

**Automatic identification (no action needed):**

- **DTMF ANI** -- your radio automatically sends a short digit sequence when you first key up (if your radio supports ANI). This is the primary identification method.

**Manual identification:**

- **DTMF login** -- you key your radio and enter a DTMF code to identify yourself. Your session persists across transmissions until the timeout expires.

#### Logging In with DTMF

If you have been assigned a DTMF login code (for example, 101), here is how to log in:

1. Key up your radio (press and hold PTT)
2. Press `*101#` on your keypad
3. Release PTT

The repeater will recognize you for the rest of your session (default 30 minutes). You do not need to log in again each time you transmit -- your session persists across multiple transmissions. The session timer resets each time you key up, so it only expires after 30 minutes of inactivity.

#### What Happens When You Are Identified

When the repeater knows who you are:

- Your transmissions are logged with your name
- You can use voicemail features (check, record, play, delete messages)
- You can use OTP authentication for elevated access
- If the repeater is configured as a "closed repeater," identification is required before your transmissions will be relayed

If the repeater is closed and you are not identified, your signal will not be repeated. Log in first with your DTMF code, then release PTT and key up again.

### Using DTMF Commands

All DTMF commands follow the same pattern:

1. Key up your radio (press and hold PTT)
2. Press `*` to start the command
3. Press the command digits
4. Press `#` to end the command
5. Release PTT and wait for the repeater's response

You have about 3 seconds between each key press before the command times out and resets.

**Important:** DTMF commands are deferred until you release PTT (COR drops). The repeater acknowledges the command and waits for you to unkey before dispatching it, so the response announcement does not play while you are still transmitting.

Here is the complete list of DTMF commands:

| You Press | What It Does |
|-----------|-------------|
| `*87#` | Check voicemail -- tells you how many messages you have |
| `*86#` | Record a voicemail to your own mailbox |
| `*86<id>#` | Record a voicemail for user ID (e.g., `*862#` for user 2) |
| `*85#` | Play your voicemail messages |
| `*84#` | List your voicemail messages (same as status) |
| `*83#` | Delete a voicemail message |
| `*88#` | Echo test -- arms the parrot to record and play back your next transmission |
| `*93#` | Current weather report |
| `*94#` | Weather forecast (high, low, rain chance) |
| `*95#` | Current time |
| `*96#` | NWS weather alerts |
| `*911#` | Activate emergency mode |
| `*910#` | Deactivate emergency mode |
| `*41`_pin_`#` | Turn on a GPIO device (e.g., `*4117#` turns on pin 17) |
| `*40`_pin_`#` | Turn off a GPIO device (e.g., `*4017#` turns off pin 17) |
| `*68`_code_`#` | OTP authentication (e.g., `*68123456#` for code 123456) |
| `*97#` | Toggle voice scrambler on/off |
| `*970#` | Disable voice scrambler |
| `*971#`-`*978#` | Set scrambler code 1-8 |
| `*0`_digits_`#` | AutoPatch -- dial a phone number (e.g., `*05551234567#`) |
| `*0#` | AutoPatch -- hang up current call |

### Weather and Time

**Current weather** (`*93#`): The repeater announces current conditions, temperature, and wind. Example: "Current weather. Partly cloudy. Temperature 72 degrees. Wind from the south at 12 miles per hour."

**Weather forecast** (`*94#`): The repeater announces today's forecast with highs, lows, and rain chances.

**NWS alerts** (`*96#`): The repeater reads any active National Weather Service alerts for the local area. If there is an extreme alert (tornado warning, etc.), the repeater plays attention tones similar to the Emergency Alert System before the announcement.

**Time check** (`*95#`): The repeater announces the current time. Example: "The time is 2:30 PM central."

### Echo Test (Parrot)

The parrot feature lets you hear what your signal sounds like through the repeater. This is useful for checking your audio quality, testing your radio, or adjusting your microphone.

1. Key up and press `*88#`, then release PTT
2. You will hear two short beeps confirming the parrot is armed
3. Key up again and speak normally for up to 10 seconds
4. Release PTT
5. The repeater plays back exactly what it heard from you

### Voicemail

Voicemail lets identified users leave and receive voice messages through the repeater. You must be identified (via DTMF ANI or DTMF login) for voicemail to work.

**Check for messages** (`*87#`): The repeater tells you how many voicemail messages you have. If you have no messages, it says "No voicemail messages." If text-to-speech is not available, you will hear one beep for each message, or a low tone if there are no messages.

**Record a message** (`*86#`): Starts recording to your own mailbox. A beep confirms recording has begun. Speak your message (up to 60 seconds), then release PTT. The recording stops automatically when you unkey, and a confirmation tone plays.

**Leave a message for another user** (`*86<id>#`): Records a message into another user's mailbox. For example, `*862#` leaves a message for user 2, `*8610#` for user 10. The repeater announces "Recording message for \<name\>" before the beep so you know you're targeting the right person. If the user ID doesn't exist, you'll hear an error tone.

**Play messages** (`*85#`): Plays back the first available message in your mailbox.

**List messages** (`*84#`): Tells you how many messages are in your mailbox (works the same as the status command).

**Delete a message** (`*83#`): Deletes the first message in your mailbox. A confirmation tone plays after deletion.

### Emergency Mode

Emergency mode is for genuine emergencies when extended, uninterrupted communication through the repeater is needed.

**When to use it:** Only activate emergency mode during real emergencies that require extended repeater access without automatic interruptions.

**Activating** (`*911#`): Key up and press `*911#`. The repeater announces "Emergency mode activated." While active:

- The time-out timer (TOT) is disabled, so you can transmit for as long as needed without being cut off
- Automated announcements (weather, time) are suppressed so they do not interrupt emergency communications

**Deactivating** (`*910#`): Key up and press `*910#`. The repeater announces "Emergency mode deactivated" and returns to normal operation.

**Auto-deactivation:** Emergency mode automatically deactivates after 30 minutes (configurable by the administrator) in case someone forgets to turn it off.

### OTP Authentication

Some repeater commands or features may require elevated access. OTP (One-Time Password) authentication provides a secure way to temporarily gain elevated privileges using an authenticator app on your phone.

**What you need:**

- Your user account must have a TOTP secret configured by the administrator
- An authenticator app on your phone (Google Authenticator, Authy, Microsoft Authenticator, or any TOTP-compatible app)

**Setting up your authenticator app:**

1. Ask the administrator for your TOTP secret or scan the QR code they provide
2. Add the secret to your authenticator app -- it will begin generating 6-digit codes that change every 30 seconds

**Authenticating:**

1. You must first be identified by the repeater (via DTMF ANI or DTMF login)
2. Open your authenticator app and note the current 6-digit code
3. Key up and press `*68` followed by the 6 digits, then `#`
   - Example: if your code is 482951, press `*68482951#`
4. If successful, the repeater announces "Authentication successful. Elevated access granted" and plays a confirmation tone
5. If the code is wrong, the repeater announces "Authentication failed" and plays an error tone

**Session duration:** Your elevated access lasts for 2 minutes (configurable by the administrator). After that, the repeater announces "Elevated session expired" and you return to normal access. You can re-authenticate at any time by entering a new code.

### AutoPatch (FreeSWITCH)

AutoPatch lets you make and receive phone calls through the repeater using a FreeSWITCH telephony server. This feature is typically enabled only on Amateur (HAM) and Part 90 Business repeaters due to FCC regulations around GMRS interconnection.

**Making a call** (`*0<digits>#`): Key up and press `*0` followed by the phone number, then `#`. For example, `*05551234567#` dials 555-123-4567. The repeater announces "Dialing" and you will hear the call progress (ringing, busy, etc.) through the repeater.

**Hanging up** (`*0#`): Key up and press `*0#` to end the current call. The repeater announces "Call ended."

**During a call:**

- When you key up, your voice is sent to the phone caller
- When the phone caller speaks, their voice is transmitted through the repeater
- Voice Activity Detection (VAD) automatically keys the repeater when the phone caller is speaking
- The call automatically ends after the configured maximum duration (default 3 minutes) or after a period of inactivity (default 60 seconds)

**Access control:** The administrator may restrict autopatch to authenticated users only (`admin_only = true`), require a dial prefix, or limit dialing to a whitelist of allowed numbers.

### What You Will Hear

The repeater produces several sounds you should be aware of:

- **Courtesy tone** -- a short beep after you release PTT, confirming the repeater heard you
- **CW ID** -- Morse code identification of the repeater's callsign, sent every 10 minutes (FCC requirement). May be followed by a voice announcement of the frequency and PL tone
- **Confirmation beeps** -- short tones after DTMF commands to confirm they were received
- **Error tone** -- a lower, longer tone indicating a command failed or was not understood
- **Time-out warning** -- a tone that plays if you have been transmitting continuously for too long (default 3 minutes). Release PTT and let others use the repeater

---

## Part 2: For Administrators

This section covers installation, configuration, and ongoing management of a kerchunkd repeater.

### Prerequisites

**Hardware:**

- Raspberry Pi (3B+ or later recommended) or any Linux computer
- RIM-Lite v2 USB radio interface (CM119 chipset) -- provides audio I/O and COR/PTT via USB HID
- Retevis RT97L or compatible GMRS repeater with DB9 accessory port
- DB9 cable connecting the RIM-Lite to the repeater

**Hardware connections (RIM-Lite to RT97L via DB9):**

| RIM-Lite Pin | Signal | RT97L Pin | Notes |
|---|---|---|---|
| 2 | TX Audio out | 2 | < 100mV |
| 3 | COS in | 3 | Active low |
| 5 | PTT out | 9 | Active low |
| 6 | RX Audio in | 5 | De-emphasized discriminator |
| 8, 9 | Ground | 7 | |

**Software dependencies (Debian/Ubuntu):**

```bash
sudo apt install build-essential pkg-config portaudio19-dev libcurl4-openssl-dev
```

### Building from Source

```bash
git clone https://github.com/briankwest/kerchunk.git
cd kerchunk
autoreconf -fi
./configure
make            # Builds daemon, CLI, and all 27 modules
make check      # Runs the test suite (all must pass)
```

Build outputs:
- `kerchunkd` -- the daemon
- `kerchunk` -- the CLI tool
- `modules/*.so` -- 27 loadable modules
- `test_kerchunk` -- test binary

### Linux Setup

**Audio group membership:** PortAudio needs access to ALSA devices. Add your user to the `audio` group:

```bash
sudo usermod -aG audio $USER
# Log out and back in for the change to take effect
```

**USB HID permissions:** The RIM-Lite's HID device (`/dev/hidraw*`) for COR/PTT is only accessible by root by default. Install the included udev rule:

```bash
sudo cp 99-rimlite.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=hidraw
```

This grants `audio` group access to the C-Media CM119 HID interface.

### Initial Configuration

Copy the example config and edit it:

```bash
cp kerchunk.conf.example kerchunk.conf
cp users.conf.example users.conf
```

#### Listing Audio Devices

Find the correct audio device names for your RIM-Lite:

```bash
./kerchunkd -d
```

Use the reported device names in the `[audio]` section. For USB audio interfaces, set `hw_rate = 48000` -- many USB devices claim to support 8kHz but produce garbled audio at that rate. The daemon handles resampling internally.

#### Minimal Configuration

At minimum, edit these values in `kerchunk.conf`:

```ini
[general]
callsign = YOURCALL        ; Your FCC callsign (required for CW ID)
frequency = 462.550        ; Your repeater output frequency
sounds_dir = ./sounds      ; Path to the sounds directory

[audio]
capture_device = <from -d output>
playback_device = <from -d output>
hw_rate = 48000            ; Recommended for USB audio

[hid]
device = /dev/rimlite      ; Adjust if your device is different
```

#### Starting the Daemon

```bash
./kerchunkd -c kerchunk.conf -f    # Foreground (for initial testing)
./kerchunkd -c kerchunk.conf       # Background (daemonized)
```

### Configuration Reference

The configuration file uses INI format. Lines starting with `;` are comments.

#### `[general]` -- Global Settings

| Key | Default | Description |
|-----|---------|-------------|
| `callsign` | (none) | FCC callsign for CW ID (required) |
| `frequency` | (none) | Repeater output frequency in MHz |
| `offset` | (none) | Input offset in MHz |
| `log_level` | `info` | Logging verbosity: `error`, `warn`, `info`, `debug` |
| `sounds_dir` | `./sounds` | Base path for WAV sound files |
| `socket_path` | `/tmp/kerchunk.sock` | Unix socket path for CLI |
| `pid_file` | `/tmp/kerchunkd.pid` | PID file to prevent duplicate instances |
| `users_file` | (none) | Separate user/group database file (recommended) |
| `address` | (none) | Site street address (for dashboard display) |
| `latitude` | (none) | Site latitude in decimal degrees |
| `longitude` | (none) | Site longitude in decimal degrees |
| `elevation` | (none) | Site elevation in feet ASL |
| `google_maps_api_key` | (none) | Google Maps API key for coverage planner |

#### `[modules]` -- Module Loading

| Key | Default | Description |
|-----|---------|-------------|
| `module_path` | `./modules` | Directory containing `.so` module files |
| `load` | (none) | Comma-separated module list (order matters) |

The recommended module load order is:

```
mod_repeater,mod_cwid,mod_courtesy,mod_caller,mod_dtmfcmd,mod_otp,
mod_voicemail,mod_gpio,mod_logger,mod_weather,mod_time,mod_recorder,
mod_txcode,mod_emergency,mod_parrot,mod_cdr,mod_tts,mod_nws,mod_stats,
mod_web,mod_webhook,mod_scrambler,mod_sdr,mod_freeswitch,
mod_pocsag,mod_flex,mod_aprs
```

Load `mod_tts` before `mod_nws` so text-to-speech is available for weather alert announcements.

#### `[audio]` -- Audio Device

| Key | Default | Description |
|-----|---------|-------------|
| `sample_rate` | `48000` | Internal audio sample rate (valid: 8000, 16000, 32000, 48000) |
| `capture_device` | `default` | PortAudio capture device name |
| `playback_device` | `default` | PortAudio playback device name |
| `hw_rate` | `0` (auto) | Force hardware sample rate; use `48000` for USB |
| `tx_encode` | `off` | Mix CTCSS/DCS encoding into TX audio. Default off (repeater handles encoding) |
| `speaker_volume` | `-1` | ALSA speaker volume (0-151, -1 = don't set) |
| `mic_volume` | `-1` | ALSA mic volume (0-16, -1 = don't set) |
| `agc` | (unset) | ALSA AGC switch (`on` or `off`, unset = don't change) |
| `preemphasis` | `off` | Pre-emphasis filter (RT-97L handles this in hardware) |

#### `[hid]` -- USB HID Interface

| Key | Default | Description |
|-----|---------|-------------|
| `device` | `/dev/rimlite` | HID device path |
| `cor_bit` | `0` | GPIO bit for COR input (0-7) |
| `cor_polarity` | `active_low` | COR polarity: `active_low` or `active_high` |
| `ptt_bit` | `2` | GPIO pin number for PTT output (1-8) |

#### `[repeater]` -- State Machine and Timers

| Key | Default | Description |
|-----|---------|-------------|
| `tail_time` | `2000` | Tail timer after COR drop, in ms |
| `hang_time` | `500` | Hang time after tail before idle, in ms |
| `timeout_time` | `180000` | Time-out timer (3 min max continuous TX), in ms |
| `cor_debounce` | `150` | Kerchunk filter debounce, in ms (0 to disable) |
| `tx_delay` | `100` | Silence after PTT assert before audio, in ms |
| `tx_tail` | `200` | Silence after audio before PTT release, in ms |
| `software_relay` | `off` | Relay RX audio to TX in software |
| `relay_drain` | `500` | Continue relaying after COR drop, in ms |
| `require_identification` | `off` | Closed repeater: deny access unless identified |
| `cwid_interval` | `600000` | CW ID interval in ms (capped at 900000 per FCC) |
| `cwid_wpm` | `20` | Morse code speed in words per minute |
| `cwid_freq` | `800` | CW tone frequency in Hz |
| `voice_id` | `on` | Announce frequency/PL via TTS after CW ID |
| `tx_ctcss` | `0` | Default TX CTCSS tone (freq x 10, e.g., 1318 = 131.8 Hz) |
| `tx_dcs` | `0` | Default TX DCS code |
| `ctcss_amplitude` | `800` | CTCSS encoder amplitude (100-4000) |
| `cor_drop_hold` | `1000` | COR drop hold for DTMF COS glitch absorption, in ms (0-5000) |

#### `[caller]` -- Caller Identification

| Key | Default | Description |
|-----|---------|-------------|
| `methods` | (none) | Enabled methods: `dtmf_ani,dtmf_login` |
| `ani_window` | `500` | Window after COR for ANI digits, in ms |
| `login_timeout` | `1800000` | DTMF login session timeout (30 min), in ms |

#### `[voicemail]` -- Voicemail System

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `off` | Enable voicemail |
| `voicemail_dir` | `/var/lib/kerchunk/voicemail` | Message storage directory |
| `max_messages` | `20` | Maximum messages per user |
| `max_duration` | `60` | Maximum recording length in seconds |

#### `[weather]` -- Weather Announcements

| Key | Default | Description |
|-----|---------|-------------|
| `api_key` | (none) | weatherapi.com API key (required) |
| `location` | (none) | ZIP code or city name |
| `interval` | `1800000` | Auto-announce interval (30 min), in ms |
| `auto_announce` | `off` | Periodic announcements (off by default for FCC compliance) |
| `announce_temp` | `on` | Include temperature in announcement |
| `announce_conditions` | `on` | Include sky conditions |
| `announce_wind` | `on` | Include wind speed and direction |

#### `[time]` -- Time Announcements

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `off` | Periodic auto-announce (off by default for FCC compliance) |
| `interval` | `900000` | Auto-announce interval (15 min), in ms |
| `timezone` | (none) | `central`, `eastern`, `mountain`, or `pacific` |

#### `[nws]` -- NWS Weather Alerts

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `off` | Enable NWS alert monitoring |
| `latitude` | (none) | Location latitude (decimal degrees) |
| `longitude` | (none) | Location longitude (decimal degrees) |
| `contact` | (none) | Email address for NWS API User-Agent (required) |
| `poll_interval` | `300000` | API poll interval (5 min), in ms |
| `reannounce_interval` | `900000` | Re-announce active alerts (15 min), in ms |
| `min_severity` | `moderate` | Minimum severity: `extreme`, `severe`, `moderate`, `minor` |
| `auto_announce` | `on` | Announce new alerts automatically |
| `attention_tones` | `on` | EAS-style tones for extreme alerts |

#### `[emergency]` -- Emergency Mode

| Key | Default | Description |
|-----|---------|-------------|
| `timeout` | `1800000` | Auto-deactivate timeout (30 min), in ms |

#### `[tts]` -- Text-to-Speech (ElevenLabs)

| Key | Default | Description |
|-----|---------|-------------|
| `api_key` | (none) | ElevenLabs API key (required) |
| `voice_id` | `21m00Tcm4TlvDq8ikWAM` | ElevenLabs voice ID |
| `model` | `eleven_turbo_v2_5` | ElevenLabs model |

TTS responses are cached as WAV files. Use `tts cache-clear` via the CLI to flush the cache.

#### `[otp]` -- OTP Authentication

| Key | Default | Description |
|-----|---------|-------------|
| `session_timeout` | `120000` | Elevated session duration (2 min), in ms |
| `time_skew` | `1` | Accept +/- N time steps (each 30 seconds) |

#### `[parrot]` -- Echo Test

| Key | Default | Description |
|-----|---------|-------------|
| `max_duration` | `10` | Maximum recording length in seconds (capped at 30) |

#### `[courtesy]` -- Courtesy Tone

| Key | Default | Description |
|-----|---------|-------------|
| `freq` | `800` | Tone frequency in Hz |
| `duration` | `100` | Tone duration in ms |
| `amplitude` | `4000` | Tone amplitude (0-32767) |

#### `[dtmf]` -- DTMF Processing

| Key | Default | Description |
|-----|---------|-------------|
| `inter_digit_timeout` | `3000` | Timeout between digits before reset, in ms |
| `cor_gate_ms` | `200` | Suppress DTMF during squelch transients, in ms |

#### `[recording]` -- Transmission Recording

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `off` | Enable transmission recording |
| `directory` | `recordings` | Output directory for WAV files |
| `max_duration` | `300` | Maximum recording length in seconds (5 min) |

Recordings are saved as `recordings/YYYYMMDD_HHMMSS_RX_username.wav` (for received transmissions) and `recordings/YYYYMMDD_HHMMSS_TX_username.wav` (for transmitted audio). Filenames use the user's `username` field (not the display `name`). An activity log (`recordings/activity.log`) is maintained for FCC 95.1705 cooperative use record-keeping.

#### `[cdr]` -- Call Detail Records

| Key | Default | Description |
|-----|---------|-------------|
| `directory` | `cdr` | Output directory for CSV files |

Daily CSV files are created at `cdr/YYYY-MM-DD.csv` with fields: timestamp, date, time, user ID, user name, identification method, duration, emergency flag, and recording path.

#### `[gpio]` -- GPIO Relay Control

| Key | Default | Description |
|-----|---------|-------------|
| `allowed_pins` | (none) | Comma-separated list of allowed GPIO pins |

Only pins listed in `allowed_pins` can be controlled via DTMF. Safe general-purpose pins on Raspberry Pi include: 5, 6, 13, 16, 17, 19, 20, 21, 22, 26, 27. Avoid GPIO 0-4, 7-11, 14-15 (used by system buses).

All GPIO pins are 3.3V logic. Use a relay board or transistor driver for loads that need more current.

#### `[stats]` -- Statistics

| Key | Default | Description |
|-----|---------|-------------|
| `persist` | `off` | Save statistics to disk on shutdown |
| `persist_file` | `stats.dat` | Persistence file path |

#### `[logger]` -- Event Logger

| Key | Default | Description |
|-----|---------|-------------|
| `file` | `events.log` | Log file path |
| `max_size_mb` | `10` | Rotation threshold in MB |

#### `[web]` -- Web Dashboard

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `off` | Enable the web server |
| `port` | `8080` | HTTP listen port |
| `bind` | `127.0.0.1` | Bind address (`0.0.0.0` for external access) |
| `auth_token` | (none) | Bearer token for admin API authentication (empty = no auth) |
| `static_dir` | (none) | Path to HTML/JS/CSS files |
| `tls_cert` | (none) | Path to TLS certificate PEM file (enables HTTPS) |
| `tls_key` | (none) | Path to TLS private key PEM file |
| `ptt_enabled` | `off` | Enable WebSocket push-to-talk |
| `ptt_max_duration` | `30` | Maximum PTT transmission duration in seconds |
| `ptt_priority` | `2` | Queue priority for PTT audio |
| `registration_enabled` | `off` | Enable public user self-registration |

#### `[webhook]` -- Webhook Notifications

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `off` | Enable webhook notifications |
| `url` | (none) | Destination URL for HTTP POST |
| `secret` | (none) | Shared secret (sent as `X-Webhook-Secret` header) |
| `events` | (none) | Comma-separated event list to send |
| `timeout_ms` | `5000` | HTTP request timeout in ms |
| `retry_count` | `2` | Number of retries on failure |

#### `[scrambler]` -- Voice Scrambler

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `off` | Enable frequency inversion scrambler |
| `code` | `4` | Scrambler code 1-8 (carrier = 2600 + code*100 Hz) |
| `frequency` | (none) | Optional explicit carrier frequency in Hz |

DTMF: `*97#` toggle, `*970#` off, `*971#`-`*978#` set code. CW ID and emergency mode bypass scrambling.

#### `[freeswitch]` -- AutoPatch (FreeSWITCH)

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `off` | Enable FreeSWITCH autopatch |
| `freeswitch_host` | `127.0.0.1` | FreeSWITCH ESL host |
| `esl_port` | `8021` | ESL port |
| `esl_password` | `ClueCon` | ESL password |
| `sip_gateway` | (none) | SIP gateway name for outbound calls |
| `udp_base_port` | `16000` | Base UDP port for audio streams |
| `max_call_duration` | `180` | Maximum call duration in seconds |
| `dial_timeout` | `30` | Dial timeout in seconds |
| `inactivity_timeout` | `60` | Hang up after this many seconds of silence |
| `vad_threshold` | `200` | Voice Activity Detection threshold |
| `vad_hold_ms` | `300` | VAD hold time in ms |
| `admin_only` | `false` | Restrict autopatch to authenticated users |
| `dial_prefix` | (none) | Prefix prepended to all dialed numbers |
| `dial_whitelist` | (none) | Comma-separated list of allowed number patterns |

DTMF: `*0<digits>#` to dial, `*0#` to hang up. See [FREESWITCH.md](FREESWITCH.md) for FreeSWITCH server configuration and architecture details.

#### `[sdr]` -- SDR Channel Monitor

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `off` | Enable RTL-SDR channel monitor |
| `device_index` | `0` | RTL-SDR device index |
| `channel` | `1` | Channel number (1-22) |
| `log_file` | `sdr_activity.csv` | CSV activity log file |

Requires `librtlsdr-dev`. Tunes to a single channel at 240 kHz, FM demod with CTCSS/DCS/DTMF decoding.

#### `[pocsag]` -- POCSAG Paging

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `off` | Enable POCSAG paging encoder |

Requires [libpocsag](https://github.com/briankwest/libpocsag) (detected by pkg-config at build time).

CLI commands:

```
kerchunk> pocsag send 1234567 "Meeting at 3pm"   # Send alphanumeric page
kerchunk> pocsag numeric 1234567 5551234          # Send numeric page
kerchunk> pocsag tone 1234567                     # Send tone-only page
kerchunk> pocsag status                           # Show POCSAG status
```

#### `[flex]` -- FLEX Paging

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `off` | Enable FLEX paging encoder |

Requires [libflex](https://github.com/briankwest/libflex) (detected by pkg-config at build time).

CLI commands:

```
kerchunk> flex send 1234567 "Meeting at 3pm"      # Send alphanumeric page
kerchunk> flex numeric 1234567 5551234             # Send numeric page
kerchunk> flex tone 1234567                        # Send tone-only page
kerchunk> flex status                              # Show FLEX status
```

#### `[aprs]` -- APRS Position/Telemetry

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `off` | Enable APRS module |
| `rx_enabled` | `on` | Decode APRS packets from SDR audio |
| `tx_enabled` | `off` | Transmit position beacons |
| `tx_interval` | `600` | Seconds between beacons (600 for GMRS, 120 for HAM) |
| `tx_path` | `WIDE1-1` | Digipeater path |
| `tx_comment` | `kerchunkd repeater` | Beacon comment string |
| `tx_symbol` | `/r` | APRS symbol |
| `rx_log_file` | `aprs_rx.csv` | RX decode log file |

Requires [libaprs](https://github.com/briankwest/libaprs) (detected by pkg-config at build time). See [APRS.md](APRS.md) for architecture details.

CLI commands:

```
kerchunk> aprs beacon                              # Force immediate beacon
kerchunk> aprs send "Hello from the repeater"      # Send APRS message
kerchunk> aprs status                              # Show APRS status
```

### FCC Compliance Settings

kerchunkd includes several features for FCC compliance. These are the key settings administrators should be aware of:

- **CW ID interval** (`cwid_interval`): Automatically capped at 15 minutes maximum per FCC 95.1751. The default of 10 minutes provides margin. The repeater automatically defers the CW ID if the channel is busy and sends it during the next tail period or idle moment.

- **Auto-announce defaults**: Weather (`auto_announce`) and time (`enabled`) auto-announcements are off by default per FCC 95.1733, which restricts unsolicited one-way transmissions on GMRS. Users can still request weather, time, and alerts on-demand via DTMF at any time.

- **Kerchunk filter** (`cor_debounce`): The 150ms default filters out brief accidental key-ups.

- **Time-out timer** (`timeout_time`): Prevents stuck transmissions. Default 3 minutes.

- **Recording and CDR**: When enabled, these provide the activity logs required by FCC 95.1705 for cooperative use record-keeping.

### User and Group Management

Users and groups can be defined directly in `kerchunk.conf`, but it is recommended to use a separate `users.conf` file by setting `users_file = users.conf` in the `[general]` section. The web UI writes changes to this separate file, keeping the main config untouched. When `registration_enabled = on` in the `[web]` section, users can also self-register through the web interface.

#### Adding a User

Add a `[user.N]` section (where N is a unique integer starting from 1):

```ini
[user.3]
username = charlie        ; Lowercase, no spaces — used for PTT login
name = Charlie Smith      ; Display name
email = charlie@example.com
callsign = WRXYZ123       ; FCC callsign (optional, forced uppercase)
ani = 5553                ; Identified by DTMF ANI
dtmf_login = 103          ; Can also log in with *103#
access = 1                ; 1 = basic access
voicemail = 1             ; 1 = voicemail enabled
group = 1                 ; Member of group 1
```

#### Access Levels

| Level | Name | Description |
|-------|------|-------------|
| 0 | None | No special access |
| 1 | Basic | Standard user features (voicemail, etc.) |
| 2 | Admin | Full access to all features |

#### Identification Methods

Each user can be identified by one or both of these methods:

| Field | Example | How It Works |
|-------|---------|-------------|
| `ani` | `5551` | Automatic: radio sends DTMF digits after key-up |
| `dtmf_login` | `101` | Manual: user presses `*101#` on their radio |

CTCSS/DCS tones are NOT used for caller identification. They are reserved for repeater access (squelch gating), tone-based action routing, and selective calling (group TX tones).

#### Groups

Groups provide shared settings for sets of users. Define groups with `[group.N]` sections:

```ini
[group.1]
name = Family
tx_ctcss = 1000             ; Members of this group hear CTCSS 100.0 Hz

[group.2]
name = Friends
tx_ctcss = 1318             ; Members of this group hear CTCSS 131.8 Hz
```

Assign a user to a group with the `group` field in their `[user.N]` section.

#### TX Tone Hierarchy

The repeater can transmit different CTCSS/DCS tones to different users so their radios only open squelch for traffic intended for them. The tone used follows this priority:

1. **Group** -- `tx_ctcss` or `tx_dcs` set on the user's group
2. **Repeater default** -- `tx_ctcss` or `tx_dcs` set in the `[repeater]` section

#### Setting Up TOTP for a User

To enable OTP authentication for a user:

1. Generate a Base32 secret (16+ characters). You can use an online Base32 generator or a command like:
   ```bash
   python3 -c "import secrets, base64; print(base64.b32encode(secrets.token_bytes(10)).decode())"
   ```

2. Add the secret to the user's config:
   ```ini
   [user.1]
   name = Brian
   totp_secret = JBSWY3DPEHPK3PXP
   ```

3. Share the secret with the user so they can add it to their authenticator app (Google Authenticator, Authy, etc.). You can create a QR code for easy setup. The QR code encodes a URI in this format:
   ```
   otpauth://totp/kerchunkd:Brian?secret=JBSWY3DPEHPK3PXP&issuer=kerchunkd
   ```

4. The user can now authenticate by pressing `*68` followed by their 6-digit code and `#`.

### Web Dashboard

Enable the web dashboard by setting `enabled = on` in the `[web]` section. The public dashboard is at `http://localhost:8080` (or your configured address/port). The admin dashboard is at `/admin.html`.

Public pages (no authentication required):
- **Public dashboard** (`/`) shows repeater status, live audio streaming, current weather, NWS alerts, and coverage map
- **Registration** (`/register.html`) allows new users to self-register (when `registration_enabled = on`)
- **PTT** (`/ptt.html`) provides WebSocket push-to-talk with mic capture (users authenticate with their username and DTMF login PIN)

Admin pages (require `auth_token`):
- **Admin dashboard** (`/admin.html`) shows real-time SSE event stream, state machines, statistics, controls, TTS input, and event log
- **Users** (`/users.html`) provides user and group CRUD with TOTP QR code management
- **Config** (`/config.html`) provides a live configuration editor with reload
- **Coverage** (`/coverage.html`) is a GMRS RF coverage planner with terrain analysis

The `/api/commands` endpoint returns a JSON list of all available CLI/API commands with their names, usage, and descriptions. This is useful for building integrations or discovering available operations programmatically.

If `auth_token` is set, admin pages prompt for the token on first access. For external access, set `bind = 0.0.0.0`. For HTTPS, provide `tls_cert` and `tls_key` paths pointing to PEM files.

#### Public Dashboard

The public dashboard (`/`) is the default landing page and requires no authentication:

- **Repeater info** -- callsign, frequency, offset, address, coordinates, elevation
- **Live audio** -- listen to RX and TX audio via WebSocket (no auth needed to listen)
- **Weather** -- current conditions, temperature, wind, humidity, and forecast
- **NWS alerts** -- active weather alerts for the local area
- **Coverage map** -- shown if a coverage.png image exists
- Status badge updates every 5 seconds via polling

#### Registration

When `registration_enabled = on` in `[web]`, the registration page (`/register.html`) allows public self-registration:

- User provides a username (lowercase, no spaces), optional display name, and optional email
- The system auto-generates a 4-digit DTMF login PIN and 5-digit ANI
- New users get basic access (level 1) by default
- The generated PIN and ANI are shown after registration and needed for PTT login

#### PTT (Push-to-Talk)

The PTT page (`/ptt.html`) provides browser-based push-to-talk:

- Users authenticate with their username and DTMF login PIN (not the admin token)
- Hold the PTT button (or spacebar) to transmit
- Mic audio is captured via AudioWorklet, sent as PCM16 over WebSocket
- RX audio plays back through the browser when not transmitting
- Maximum transmit duration is configurable (`ptt_max_duration`, default 30s)

#### Admin Dashboard

The admin dashboard (`/admin.html`) requires the Bearer token and shows:

- **Repeater state** -- current RX state (IDLE, RECEIVING, TAIL_WAIT, HANG_WAIT, TIMEOUT) and TX state (TX_IDLE, TX_RELAY, TX_QUEUE, TX_TAIL)
- **PTT and COR status** -- live indicator badges
- **Current caller** -- who is currently identified on the repeater
- **Statistics** -- uptime, RX/TX counts and times, duty cycle, 24h activity histogram
- **Controls** -- buttons to trigger CW ID, simulate DTMF, reload config, send TTS
- **NWS alerts** -- active weather alerts
- **Users** -- quick user list
- **Live audio** -- listen to RX and TX audio
- **Event log** -- live scrolling log via Server-Sent Events (SSE)

#### Users Page

The users page (`/users.html`) provides user and group management:

- View all configured users with username, name, email, DTMF login, ANI, access level, group, and TOTP status
- Add, edit, and delete users (including username and email fields)
- TOTP QR code generation and display
- Add, edit, and delete groups
- Changes are saved to the config file and trigger a reload

#### Config Page

The config page (`/config.html`) provides a live configuration editor:

- View and edit the current daemon configuration
- Save changes and trigger a configuration reload
- Sensitive values (API keys, auth tokens, TOTP secrets) are masked in the display

#### Coverage Page

The coverage page (`/coverage.html`) is a GMRS RF coverage planner:

- Uses site location from `[general]` (latitude, longitude, elevation)
- Google Maps integration for terrain visualization (requires `google_maps_api_key`)
- Estimates RF coverage area with terrain analysis

### CLI Tool

The `kerchunk` CLI tool communicates with the running daemon via Unix socket.

**Interactive console:**

```bash
./kerchunk                         # Enter interactive mode
```

Features: tab completion, command history, live log streaming, auto-reconnect.

**Useful commands:**

```
kerchunk> status                   # Daemon status
kerchunk> help                     # List all commands
kerchunk> version                  # Version and git hash (e.g. 1.0.1+abc1234)
kerchunk> uptime                   # Daemon uptime
kerchunk> audio                    # Audio device and sample rate info
kerchunk> hid                      # HID device status
kerchunk> user                     # Current user info
kerchunk> log                      # Log level control
kerchunk> diag                     # Diagnostics
kerchunk> play sounds/test.wav     # Play a WAV file
kerchunk> tone 800 500             # Generate a tone (freq duration_ms)
kerchunk> stats                    # Channel statistics
kerchunk> stats user Brian         # Per-user statistics
kerchunk> caller                   # Current caller info
kerchunk> emergency                # Emergency mode status
kerchunk> otp                      # OTP session status
kerchunk> voicemail status         # Voicemail status
kerchunk> cwid now                 # Send CW ID immediately
kerchunk> dtmfcmd                  # Show DTMF command table
kerchunk> pocsag send 1234 "Test"  # Send POCSAG page
kerchunk> flex send 1234 "Test"    # Send FLEX page
kerchunk> aprs beacon              # Force APRS beacon
kerchunk> /log debug               # Start log streaming at debug level
kerchunk> /nolog                   # Stop log streaming
```

**One-shot commands:**

```bash
./kerchunk status                  # Run a single command and exit
./kerchunk -j status               # Output as JSON
./kerchunk -x 'sim dtmf *95#'     # Simulate a DTMF sequence
./kerchunk -x 'tts say hello'     # Send a TTS announcement
```

**JSON output and event streaming:**

```bash
./kerchunk -j status               # Structured JSON status
./kerchunk -j stats | jq .         # Statistics as JSON
./kerchunk -e -j                   # Live NDJSON event stream
```

**Simulation commands (useful for testing without a radio):**

```
kerchunk> sim cor on               # Simulate carrier detect
kerchunk> sim cor off              # Simulate carrier loss
kerchunk> sim dtmf *95#            # Simulate DTMF command
kerchunk> sim tx sounds/test.wav   # Queue a WAV file for playback
```

### Monitoring and Maintenance

#### Event Log

All daemon events are logged to `events.log` (configurable). The log includes timestamps, module names, and event details. When the log reaches the configured size (default 10 MB), it is automatically rotated.

View the live log from the CLI:

```
kerchunk> /log info
```

Or from the command line:

```bash
tail -f events.log
```

#### Call Detail Records

CDR files are daily CSV files in the `cdr/` directory. Each line records one transmission with: timestamp, date, time, user ID, user name, identification method, duration in seconds, emergency flag, and recording file path.

#### Transmission Recordings

When recording is enabled, every received and transmitted audio segment is saved as a WAV file in the `recordings/` directory. Files are named with a timestamp, direction (RX or TX), and the identified user name. An `activity.log` in the same directory provides a text summary for FCC compliance review.

Monitor disk usage and archive or delete old recordings periodically.

#### Statistics

View current statistics via the CLI:

```
kerchunk> stats                    # Channel statistics (total TX time, duty cycle, etc.)
kerchunk> stats user Brian         # Per-user statistics
kerchunk> stats reset              # Reset all counters
kerchunk> stats save               # Force save to disk
```

With `persist = on` in `[stats]`, statistics survive daemon restarts. The `stats.dat` file stores cumulative metrics including total uptime and restart count.

#### Log Rotation

The event logger (`mod_logger`) automatically rotates the log file when it reaches `max_size_mb` (default 10 MB). For CDR and recording files, set up your own rotation or cleanup policy using cron or systemd timers.

### Troubleshooting

#### Audio Issues

**No audio devices found:**
- Verify the RIM-Lite is connected: `lsusb | grep C-Media`
- Check that your user is in the `audio` group: `groups`
- List available devices: `./kerchunkd -d`
- After adding yourself to the `audio` group, you must log out and back in

**Garbled or chipmunk-sounding audio:**
- Set `hw_rate = 48000` in `[audio]`. Many USB audio devices do not actually support 8kHz even if they claim to. The daemon resamples internally.

**Audio cuts off mid-word:**
- Increase `relay_drain` in `[repeater]` (default 500ms). This controls how long the repeater continues relaying after the carrier drops.

#### HID Device Permissions

**"Permission denied" for /dev/rimlite:**
- Install the udev rule: `sudo cp 99-rimlite.rules /etc/udev/rules.d/`
- Reload: `sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=hidraw`
- Verify your user is in the `audio` group

**Wrong hidraw device:**
- If you have multiple HID devices, check which one is the CM119: `cat /sys/class/hidraw/hidraw*/device/uevent | grep -A1 CM119`
- Update `device` in `[hid]` to the correct path

#### Module Loading Failures

**Module fails to load:**
- Check that all `.so` files exist in the `module_path` directory
- Verify dependencies: modules using libcurl (mod_weather, mod_nws, mod_tts) require `libcurl4-openssl-dev`
- Check the event log for specific error messages
- Ensure module load order has dependencies first (e.g., `mod_tts` before `mod_nws`)

#### CLI Cannot Connect

**"Connection refused" or "No such file" from the CLI:**
- Verify the daemon is running: check for the PID file at `/tmp/kerchunkd.pid`
- Ensure the socket path matches between daemon config and CLI: default is `/tmp/kerchunk.sock`
- Check file permissions on the socket file

#### COR/PTT Not Working

**Repeater does not detect carrier:**
- Check the DB9 cable connections
- Verify `cor_polarity` matches your hardware (most RIM-Lite setups use `active_low`)
- Try toggling `cor_bit` values (0-7)
- Use `sim cor on` in the CLI to test the state machine independently of hardware

**Repeater does not transmit:**
- Check PTT wiring to the correct DB9 pin
- Verify `ptt_bit` in config
- Check the CLI `status` command for PTT state

### Running as a System Service

Create a systemd service file at `/etc/systemd/system/kerchunkd.service`:

```ini
[Unit]
Description=kerchunkd GMRS Repeater Controller
After=network.target sound.target

[Service]
Type=forking
ExecStart=/opt/kerchunk/kerchunkd -c /etc/kerchunk/kerchunk.conf
PIDFile=/tmp/kerchunkd.pid
Restart=on-failure
RestartSec=5
User=kerchunk
Group=audio

[Install]
WantedBy=multi-user.target
```

Then enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable kerchunkd
sudo systemctl start kerchunkd
```
