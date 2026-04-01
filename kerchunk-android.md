# Plan: Kerchunk Android App for Retevis L71

## Context

Build a native Android app (targeting Android 10+) for the Retevis L71 POC radio that connects to kerchunkd over WiFi/4G via the existing WebSocket PTT protocol. The L71 runs Android 10, has WiFi, 4G, hardware PTT button, and can sideload APKs. The app replaces ptt.html/admin.html with a native experience that supports hardware PTT, background audio, and all kerchunk features.

## Target Device

- **Retevis L71**: Android 10, 4G/WiFi, IP54, hardware PTT, 4000mAh, Zello-compatible
- Also works on any Android 10+ phone/tablet (touchscreen PTT fallback)

## Architecture

### Core Library: `kerchunk-client-android`

A reusable Android library (AAR) that encapsulates the WebSocket protocol. The app UI layer uses this library.

```
┌─────────────────────────────────────┐
│           Kerchunk App              │
│  ┌───────┐ ┌────────┐ ┌─────────┐  │
│  │  PTT  │ │ Paging │ │Dashboard│  │
│  │  UI   │ │   UI   │ │   UI    │  │
│  └───┬───┘ └───┬────┘ └────┬────┘  │
│      └─────────┼───────────┘        │
│           ┌────┴────┐               │
│           │ Client  │               │
│           │ Library │               │
│           └────┬────┘               │
│      ┌─────────┼───────────┐        │
│  ┌───┴───┐ ┌───┴────┐ ┌───┴───┐    │
│  │  WS   │ │ Audio  │ │  API  │    │
│  │Client │ │ Engine │ │Client │    │
│  └───────┘ └────────┘ └───────┘    │
└─────────────────────────────────────┘
         │          │          │
         └──────────┼──────────┘
                    │ WiFi / 4G
              ┌─────┴─────┐
              │ kerchunkd │
              │  server   │
              └───────────┘
```

### Library Components

**1. KerchunkConnection** — WebSocket lifecycle
- Connect/disconnect with WSS/TLS support
- Authentication (user/pin → sample_rate/frame_samples)
- Exponential backoff reconnection
- JSON command dispatch (any CLI command)
- Binary audio frame send/receive
- Connection state callbacks

**2. KerchunkAudio** — Audio engine
- AudioRecord for mic capture at device native rate
- AudioTrack for speaker playback
- Resampling: device rate ↔ server rate (linear interpolation)
- Frame accumulation to server frame_samples (960 @ 48kHz)
- RX ring buffer with priming
- RX mute after TX (1.5s)
- Echo cancellation via Android AcousticEchoCanceler

**3. KerchunkCommands** — REST/WS command API
- Fetch `/api/commands` → discover all available commands with UI metadata
- Execute any command via `/api/cmd` POST or WebSocket JSON
- Fetch `/api/status`, `/api/weather`, `/api/nws`, `/api/stats`
- Parse responses into Kotlin data classes

**4. KerchunkPTT** — PTT state machine
- States: IDLE → CONNECTING → AUTHENTICATED → PTT_HELD → TX_MUTE → IDLE
- Hardware PTT key mapping (KeyEvent.KEYCODE_PTT or vendor-specific)
- Touch PTT with press-and-hold
- Max duration enforcement
- Channel busy handling

### App Screens

**1. Login Screen**
- Server URL (https://repeater.mcalester.net:8443)
- Username / PIN
- Remember credentials (EncryptedSharedPreferences)
- Connection status indicator

**2. PTT Screen** (main screen)
- Large PTT button (fullscreen touch target on L71)
- RX/TX state indicator with color
- Caller info display
- Mic level meter
- Audio volume slider
- Duration timer during TX
- RX mute toggle
- Hardware PTT button support (KeyEvent listener)

**3. Paging Screen**
- POCSAG: address, baud (512/1200/2400), message, send
- FLEX: capcode, speed (1600/3200/6400), message, send
- APRS: beacon button, send message (callsign + text)
- All forms built dynamically from `/api/commands` UI metadata

**4. Dashboard Screen**
- Repeater status (callsign, frequency, RX/TX state)
- Version and sample rate
- Dynamic controls from `/api/commands` (same framework as admin.html)
- Event log (via SSE at `/api/events`)
- Weather/NWS alerts

**5. Settings Screen**
- Server URL
- Credentials
- Audio: speaker/earpiece selection, volume
- PTT: hardware key mapping, touch sensitivity
- Notifications: alert on NWS, paging activity

### Technology Stack

- **Language**: Kotlin
- **Min SDK**: 29 (Android 10)
- **Build**: Gradle with Kotlin DSL
- **WebSocket**: OkHttp (built-in WS support, TLS, connection pooling)
- **HTTP**: OkHttp + Retrofit for REST APIs
- **JSON**: Moshi (fast, Kotlin-friendly)
- **Audio**: Android AudioRecord + AudioTrack (low-latency)
- **UI**: Jetpack Compose (modern, declarative)
- **Architecture**: MVVM with StateFlow
- **DI**: Hilt
- **Storage**: EncryptedSharedPreferences for credentials
- **Background**: Foreground Service for persistent audio/connection

### WebSocket Protocol Implementation

Matches the protocol spec from ptt.html exactly:

**Authentication:**
```kotlin
ws.send("""{"cmd":"auth","user":"$user","pin":"$pin"}""")
// Response: {"ok":true,"user":"...","ptt_enabled":true,"sample_rate":48000,"frame_samples":960}
```

**PTT:**
```kotlin
ws.send("""{"cmd":"ptt_on"}""")
// Stream audio frames...
ws.send("""{"cmd":"ptt_off"}""")
```

**RX Audio Frame** (server → app):
```
[0x01][dir][seq_lo][seq_hi][PCM16 × 960 samples] = 1924 bytes
```

**TX Audio Frame** (app → server):
```
[0x02][PCM16 × 960 samples] = 1921 bytes
```

### Hardware PTT Button

The L71's hardware PTT button generates Android KeyEvents. Common mappings:
- `KeyEvent.KEYCODE_PTT` (standard Android PTT)
- `KeyEvent.KEYCODE_HEADSETHOOK` (some radios)
- Vendor-specific keycode (needs testing on actual L71)

```kotlin
override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
    if (keyCode == KeyEvent.KEYCODE_PTT || keyCode == pttKeyCode) {
        viewModel.pttOn()
        return true
    }
    return super.onKeyDown(keyCode, event)
}

override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
    if (keyCode == KeyEvent.KEYCODE_PTT || keyCode == pttKeyCode) {
        viewModel.pttOff()
        return true
    }
    return super.onKeyUp(keyCode, event)
}
```

### Audio Pipeline

**TX (Mic → Server):**
```
AudioRecord (device rate, e.g. 44100)
  → Resampler (linear interpolation → 48000)
  → Frame accumulator (buffer to 960 samples)
  → Pack [0x02 + PCM16]
  → WebSocket.send(binary)
```

**RX (Server → Speaker):**
```
WebSocket.onMessage(binary)
  → Parse [0x01 + dir + seq + PCM16]
  → Ring buffer (2 seconds, ~96000 samples)
  → Resampler (48000 → device rate)
  → AudioTrack.write()
```

### Project Structure

```
kerchunk-android/
├── app/                          # Main application
│   ├── src/main/
│   │   ├── java/net/mcalester/kerchunk/
│   │   │   ├── KerchunkApp.kt
│   │   │   ├── ui/
│   │   │   │   ├── login/LoginScreen.kt
│   │   │   │   ├── ptt/PTTScreen.kt
│   │   │   │   ├── paging/PagingScreen.kt
│   │   │   │   ├── dashboard/DashboardScreen.kt
│   │   │   │   └── settings/SettingsScreen.kt
│   │   │   ├── viewmodel/
│   │   │   │   ├── PTTViewModel.kt
│   │   │   │   ├── PagingViewModel.kt
│   │   │   │   └── DashboardViewModel.kt
│   │   │   └── service/
│   │   │       └── KerchunkService.kt    # Foreground service
│   │   ├── res/
│   │   └── AndroidManifest.xml
│   └── build.gradle.kts
├── lib/                          # Reusable client library
│   ├── src/main/java/net/mcalester/kerchunk/client/
│   │   ├── KerchunkClient.kt          # Main entry point
│   │   ├── KerchunkConnection.kt      # WebSocket lifecycle
│   │   ├── KerchunkAudio.kt           # Audio engine
│   │   ├── KerchunkCommands.kt        # REST/WS commands
│   │   ├── KerchunkPTT.kt             # PTT state machine
│   │   ├── AudioResampler.kt          # Linear interpolation
│   │   ├── RingBuffer.kt              # Lock-free audio ring
│   │   └── model/
│   │       ├── ConnectionState.kt
│   │       ├── PTTState.kt
│   │       ├── Command.kt
│   │       └── AudioFrame.kt
│   └── build.gradle.kts
├── build.gradle.kts
├── settings.gradle.kts
└── gradle.properties
```

### Foreground Service

Required for persistent audio and WebSocket connection when app is in background:

```kotlin
class KerchunkService : Service() {
    // Holds KerchunkClient instance
    // Manages AudioRecord/AudioTrack lifecycle
    // Shows persistent notification with RX/TX state
    // Survives screen off / app backgrounding
    // Hardware PTT works even when screen is off
}
```

### Implementation Phases

**Phase 1: Library core** (KerchunkConnection + KerchunkAudio)
- WebSocket connect/auth/disconnect
- Binary audio frame encode/decode
- AudioRecord capture + resampling
- AudioTrack playback + ring buffer
- PTT state machine

**Phase 2: PTT app** (minimal viable product)
- Login screen
- PTT screen with touch button
- Hardware PTT key mapping
- Foreground service
- Basic RX/TX indicators

**Phase 3: Paging**
- Fetch /api/commands for UI metadata
- Dynamic form generation from command fields
- POCSAG/FLEX/APRS send forms
- Response display

**Phase 4: Dashboard**
- SSE event stream via EventSource or OkHttp SSE
- Repeater status display
- Dynamic controls from /api/commands
- Weather/NWS alerts
- Event log

**Phase 5: Polish**
- Settings screen
- Notification actions (PTT from notification)
- Tablet/landscape layout
- Dark/light theme
- Connection quality indicator
- Audio codec optimization (optional: Opus for low bandwidth)

### Verification

1. Library unit tests: WebSocket frame encode/decode, resampler accuracy, ring buffer
2. Integration test: connect to kerchunkd, authenticate, send/receive audio
3. PTT test on L71: hardware button maps correctly, audio flows both directions
4. Paging test: send POCSAG/FLEX from app, verify on RTL-SDR
5. Dashboard test: status updates, controls work, events stream
