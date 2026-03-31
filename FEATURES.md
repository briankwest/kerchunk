# Feature Ideas — Kerchunk Brainstorm

## What Exists Today

27 modules, 4 external API integrations (weatherapi.com, NWS, ElevenLabs TTS, Google Maps),
REST/WebSocket/SSE web dashboard, DTMF command system (16 commands), user/group management,
CW ID with quiet hours, voicemail, recording, CDR, emergency mode, GPIO control, TOTP auth,
closed repeater mode, software relay, coverage planner, POCSAG paging, FLEX paging, APRS
position reporting/telemetry.

---

## API Integrations Worth Adding

### ~~APRS Gateway (aprs.fi / APRS-IS)~~ -- IMPLEMENTED as mod_aprs
- ~~Report repeater position to APRS network as an object~~
- ~~Show nearby APRS stations on the coverage map~~
- ~~Announce APRS messages addressed to the repeater callsign via TTS~~
- ~~DTMF command to query position of a callsign (`*97<call>#`)~~
- ~~Config: `[aprs]` with callsign, passcode, server, symbol~~
- See [APRS.md](APRS.md) for full architecture. CLI: `aprs beacon`, `aprs send`, `aprs status`. DTMF: `*98#` force beacon, `*980#` status.
- Also implemented: **mod_pocsag** (POCSAG paging via libpocsag) and **mod_flex** (FLEX paging via libflex)

### Reverse Autopatch / SIP Gateway
- Outbound SIP/VoIP calls triggered by DTMF sequence
- Inbound calls announced and bridged to repeater audio
- Integrates with FreeSWITCH, Asterisk, or a SIP provider API
- Access-controlled per user (admin only by default)
- Config: `[autopatch]` with SIP registrar, credentials, allowed numbers

### Email/SMS Notifications (SendGrid / Twilio)
- Send alerts on: emergency activation, TOT, NWS severe alerts, long idle periods
- Per-user notification preferences (email, SMS, both)
- Daily digest: CDR summary, duty cycle, alert count
- DTMF command to send a canned message to a predefined number
- Config: `[notify]` with provider, API key, recipient list

### Prometheus / Metrics Export
- `/api/metrics` endpoint in Prometheus exposition format
- Counters: rx_total, tx_total, tot_events, emergency_activations
- Gauges: uptime_seconds, duty_cycle_pct, queue_depth, connected_clients
- Histograms: rx_duration, tx_duration
- Enables Grafana dashboards, alerting, long-term trending

### MQTT Broker Integration
- Publish events to MQTT topics (`kerchunk/events/cor_assert`, etc.)
- Subscribe to command topics for remote control
- Bridges to Home Assistant, Node-RED, IoT dashboards
- Lightweight — single persistent connection
- Config: `[mqtt]` with broker URL, topic prefix, username/password

### Aviation Weather (aviationweather.gov / METAR)
- Fetch METAR/TAF for nearest airport
- Announce ceiling, visibility, altimeter setting
- Useful for GMRS users coordinating with aviation activities
- DTMF command: `*98#` for METAR
- Config: `[metar]` with ICAO station ID

### Seismic / USGS Earthquake Alerts
- Poll USGS earthquake API (earthquake.usgs.gov)
- Announce earthquakes above configurable magnitude within radius
- Auto-announce significant events, DTMF for recent activity
- Config: `[seismic]` with min_magnitude, radius_miles, poll_interval

### Wildfire / NIFC InciWeb
- Monitor active fires near repeater location
- Announce new incidents or significant growth
- Useful for rural GMRS repeaters in fire-prone areas
- Config: `[fire]` with radius, poll_interval

### ADS-B Aircraft Tracking (dump1090 / ADSBexchange)
- Monitor aircraft near the repeater site
- Announce low-flying aircraft, helicopters, or emergency squawks (7500/7600/7700)
- Show on coverage map overlay
- Config: `[adsb]` with source URL, altitude_filter, radius

### Sunrise/Sunset Scheduling (sunrise-sunset.org API)
- Auto-adjust quiet hours to sunset/sunrise instead of fixed clock hours
- Announce sunrise/sunset times on demand
- Drive other time-based behaviors (lighting, GPIO relays)
- Config: `quiet_mode = solar` in `[repeater]`

---

## Non-API Feature Ideas

### Multi-Site Linking
- Connect multiple kerchunkd instances over IP (Opus/PCM stream)
- Selective linking: link on demand via DTMF, unlink with timeout
- Site announcements: "linked to <sitename>"
- Shared user database across sites

### Scheduled Announcements
- Cron-style scheduler for TTS or WAV playback
- "Net night reminder" every Tuesday at 7 PM
- Holiday greetings, community announcements
- Config: `[schedule]` with cron expressions + action

### Voice Mailbox Groups
- Group voicemail: message all users in a group
- Broadcast announcements stored as voicemail
- Notification tone on next COR from user with pending messages

### Signal Quality Reporting
- Track CTCSS decode confidence, DTMF SNR per user
- Report signal quality trends over time via stats
- "Your signal report is 4 by 5" via TTS after COR drop
- Helps users optimize antenna/power

### Repeater Linking via Allstar/Echolink
- Bridge to Allstar or Echolink nodes
- DTMF connect/disconnect to remote nodes
- Announce linked node name/callsign
- Would require audio pipe integration

### Power Management / UPS Monitoring
- Monitor UPS status via NUT (Network UPS Tools) or USB HID
- Announce power loss/restore
- Graceful shutdown on low battery
- Log power events to CDR

### Audio Watermarking
- Embed inaudible digital watermark in TX audio
- Identify repeater origin of retransmitted audio
- Useful for interference investigation

### Automatic Logbook
- Web page showing chronological activity log with audio playback
- Filter by user, date, duration
- Download recordings linked from CDR entries
- Search voicemail transcripts (if TTS provider offers STT)

---

## Priority Suggestions

| Priority | Feature | Effort | Impact |
|----------|---------|--------|--------|
| High | Prometheus metrics | Small | Monitoring/alerting for unattended operation |
| High | Email/SMS notifications | Medium | Know when something happens without watching |
| High | MQTT integration | Small | Bridges to entire IoT/automation ecosystem |
| Medium | Scheduled announcements | Small | Community engagement, net reminders |
| ~~Medium~~ | ~~APRS gateway~~ | ~~Medium~~ | **DONE** — implemented as mod_aprs (position reporting, packet decoding, AFSK 1200) |
| Medium | Sunrise/sunset quiet hours | Small | Smarter quiet hour scheduling |
| Medium | Automatic logbook web page | Medium | Compliance, dispute resolution |
| Low | SIP gateway | Large | Full autopatch — complex but high value |
| Low | Multi-site linking | Large | Ambitious but differentiating |
| Low | ADS-B tracking | Medium | Niche but interesting for rural sites |
