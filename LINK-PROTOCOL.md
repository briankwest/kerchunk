# Kerchunk linking — wire protocol

Specification for the protocol spoken between **kerchunk-reflectd**
(the central reflector) and **mod_link.c** (the per-repeater client).
Section numbering is stable: source comments throughout the codebase
reference `§ 4.X` and `§ 6.1` and rely on those identifiers not moving.

## Architecture

```
                ┌─────────────────────┐
                │  kerchunk-reflectd  │
                │                     │
                │  TLS-WS :8443       │  control plane
                │  UDP    :7878       │  audio plane (SRTP)
                │                     │
                │  state: roster      │
                │         talkgroups  │
                │         floor       │
                └─────────────────────┘
                       ▲          ▲
            control + SRTP audio (UDP)
                       │          │
              ┌────────┴──┐  ┌────┴────────┐
              │ kerchunk  │  │  kerchunk   │
              │ Repeater A│  │  Repeater B │
              │ mod_link  │  │  mod_link   │
              └───────────┘  └─────────────┘
                  │                │
              FM radio          FM radio
```

Two planes: a long-lived TLS WebSocket carries control / state JSON;
a UDP/SRTP socket carries Opus audio. Authentication binds both — the
SRTP master key is delivered on the WS in the `login_ok` reply, so a
new audio session can never start without a fresh control session.

The reflector arbitrates a single talker per talkgroup via an implicit
RTP-driven floor lease (§ 4.3). Per-node membership is enforced on
both sides (§ 5).

---

## 4. Wire protocol

### 4.1 Control plane — TLS WebSocket

A long-lived WebSocket from each node to the reflector carries small
JSON messages. Frames are line-based (one JSON object per WS message).
Direction notation: **N→R** node-to-reflector, **R→N**
reflector-to-one-node, **R→\*** broadcast to TG members.

#### 4.1.1 Auth and lifecycle

**`hello`** (R→N, sent immediately on TCP+TLS+WS handshake complete):
```json
{"type":"hello","challenge":"<hex>","reflector_version":"reflectd 0.1.0",
 "min_client_version":"kerchunk 1.0.0"}
```

**`login`** (N→R):
```json
{"type":"login","node_id":"WK7ABC-1","key_hmac":"<hex>",
 "nonce":"<hex>","client_version":"kerchunk 1.0.2"}
```
- `nonce` is a random value the node picked.
- `key_hmac` is `HMAC-SHA256(preshared_key, challenge || nonce)`.
- The challenge binding prevents replay across connections.

**`login_ok`** (R→N):
```json
{"type":"login_ok","node_id":"WK7ABC-1","talkgroup":4123,
 "rtp_endpoint":"203.0.113.5:7878",
 "srtp_master_key":"<32-byte hex>","srtp_master_salt":"<14-byte hex>",
 "ssrc":2876543210,"reflector_ssrc":1111111111,
 "session_id":"<uuid>","keepalive_s":15,
 "hangtime_ms":1500}
```
SRTP master key is fresh per session — never reused.

**`login_denied`** (R→N, then close): distinct from `error` so the
client knows whether to retry or stop:
```json
{"type":"login_denied","code":"bad_key","msg":"HMAC mismatch"}
```
`code` ∈ § 4.1.6. **Permanent** codes (`bad_key`, `unknown_node`,
`banned`, `version_mismatch`) → client stops reconnecting and surfaces
an admin alarm. **Transient** (`node_busy`, `internal`) → client backs
off and retries.

**`kicked`** (R→N, then close):
```json
{"type":"kicked","code":"admin_action","msg":"manual kick by op"}
```
Also includes `loss_too_high`, `idle_timeout`, `protocol_error`.
Client treats permanent codes as in `login_denied`.

**`reflector_shutdown`** (R→N, then close): graceful goodbye so
clients don't all reconnect at the same instant:
```json
{"type":"reflector_shutdown","reason":"deploy","restart_in_s":15}
```
Client waits `restart_in_s` (with ±20 % jitter) before the first
reconnect attempt.

**`ping`** / **`pong`**: either side may initiate; both sides SHOULD
send `ping` every 15 s if they haven't sent any other message in that
window. Receiver replies with `pong` immediately. If no traffic (any
direction) for 30 s, treat the connection as dead and reconnect.
```json
{"type":"ping","seq":42}    {"type":"pong","seq":42}
```

#### 4.1.2 Talkgroup operations

**`set_tg`** (N→R, operator-initiated DTMF or admin UI):
```json
{"type":"set_tg","tg":4124}
```

**`tg_ok`** (R→N):
```json
{"type":"tg_ok","tg":4124,"name":"Test"}
```

**`tg_denied`** (R→N):
```json
{"type":"tg_denied","tg":4124,"code":"not_authorized"}
```
`code` ∈ {`not_authorized`, `unknown_tg`}.

**`tg_membership_changed`** (R→N, reflector-initiated, e.g. config
reload removed this node from a TG):
```json
{"type":"tg_membership_changed","old_tg":4123,"new_tg":4124,
 "reason":"config_reload"}
```
If the node is removed from all TGs, `new_tg` is `null` and the node
is effectively muted until an operator picks a new TG.

**`mute`** / **`unmute`** (R→N): reflector keeps the connection but
stops forwarding the node's audio (or resumes):
```json
{"type":"mute","reason":"loss_too_high","retry_in_s":60}
{"type":"unmute"}
```

#### 4.1.3 Floor control messages

**`talker`** (R→\*, broadcast to all nodes on a TG):
```json
{"type":"talker","tg":4123,"node_id":"WK7ABC-1","since":1777068600}
```
Sent when a node grabs the floor. When the talker releases (no packets
for `hangtime_ms`), reflector sends:
```json
{"type":"talker","tg":4123,"node_id":null,"since":1777068604}
```

**`floor_denied`** (R→N, when this node's RTP arrives but the TG
already has a talker — rate-limited to one per 500 ms per node):
```json
{"type":"floor_denied","tg":4123,"current_talker":"WK7DEF-1"}
```
Local mod_link plays a brief "TG busy" tone over the air (configurable
via `link_busy_tone`) so the operator hears contention immediately.

**`floor_revoked`** (R→N, the floor was taken away from this node
mid-transmission):
```json
{"type":"floor_revoked","tg":4123,"code":"lease_expired"}
```
`code` ∈ {`lease_expired` (no packets received within hangtime window
— common, treated as normal end), `admin` (operator force-released),
`auth_failures` (too many SRTP auth fails — node is likely
misconfigured)}.

#### 4.1.4 Quality and adaptation

**`quality`** (N→R, every 10 s when connected and ≥ 30 packets
received in the window):
```json
{"type":"quality","loss_pct":1.4,"jitter_ms":12,
 "jb_depth_ms":98,"decode_errs":0,"window_s":10}
```
Drives reflector roster display, kick/mute decisions, and
target-bitrate hints.

The client snapshots-and-clears the per-window counters at every
report tick. Out-of-order arrivals within ±200 of the highest seen
seq decrement the lost counter (so LAN reordering doesn't show as
loss). Reports are suppressed when fewer than 30 packets arrived in
the window — losing 1 of 4 is statistically meaningless and would
trip a mute on noise.

**`target_bitrate`** (R→N, advisory, not enforced):
```json
{"type":"target_bitrate","bps":20000}
```
Client SHOULD reconfigure its Opus encoder near this value. Reflector
sends this when a peer on the same TG reports `loss_pct > 5 %` so
all senders gracefully back off.

#### 4.1.5 Generic error

For unexpected conditions that don't fit a specific message:
```json
{"type":"error","code":"protocol_error","msg":"unknown type 'foo'"}
```
The receiver SHOULD log and continue. Repeated `error` from the same
peer (>3 in 60 s) → other side closes the connection with `kicked`
code `protocol_error`.

#### 4.1.6 Error code enum

Single enum used in `login_denied.code`, `kicked.code`,
`tg_denied.code`, `floor_revoked.code`, `mute.reason`, and
`error.code`.

| Code              | Permanent? | Meaning                                          |
|-------------------|------------|--------------------------------------------------|
| `bad_key`         | yes        | HMAC didn't verify against configured key        |
| `unknown_node`    | yes        | `node_id` not in reflector config                |
| `banned`          | yes        | Operator-banned                                  |
| `version_mismatch`| yes        | Client below `min_client_version`                |
| `not_authorized`  | yes        | Node not on this TG's allow-list                 |
| `unknown_tg`      | yes        | TG number not in reflector config                |
| `node_busy`       | no         | Same `node_id` already connected                 |
| `idle_timeout`    | no         | No traffic within keepalive window               |
| `loss_too_high`   | no         | Sustained loss above `mute_threshold_pct`        |
| `lease_expired`   | no         | Floor dropped after hangtime — normal end        |
| `auth_failures`   | no         | Too many SRTP auth failures from this node       |
| `admin_action`    | no         | Operator-initiated kick / floor release          |
| `config_reload`   | no         | Reflector config changed                         |
| `protocol_error`  | no         | Malformed message                                |
| `internal`        | no         | Reflector bug or transient error                 |
| `deploy`          | no         | Graceful reflector restart                       |

### 4.2 Audio plane — SRTP/RTP

- **Codec:** Opus 24 kHz mono (super-wideband) at ~32 kbps target,
  60 ms frames. Inband FEC enabled with
  `OPUS_SET_PACKET_LOSS_PERC(10)`. CBR off, VBR on. 24 kHz chosen
  over 16 kHz for two reasons: (1) the 48/24 = 2 resample is a
  trivial 2× decimate / interpolate, cheaper than 48/16 = 3;
  (2) headroom for future non-FM sources (studio feed, digital
  bridge) without touching the wire format.
- **RTP payload type:** dynamic, e.g. 100. Negotiated implicitly
  (only one codec).
- **SRTP:** AES-128-CTR + HMAC-SHA1-80 (the libsrtp default profile
  `SRTP_AES128_CM_HMAC_SHA1_80`). Master key + salt come from the
  login response. SRTP rollover is large enough to never recycle in
  a session lifetime.
- **SSRC:** node-side SSRC chosen by node, server-side SSRC chosen by
  reflector. Both communicated in `login_ok` so the receiver can
  filter properly. The reflector forwards each incoming packet
  verbatim — including the original sender's RTP sequence number —
  but rewrites the SSRC field to its own outbound SSRC so receivers
  see one logical stream per TG.
- **Send timing:** when local repeater is RX'ing, encode and send
  every 60 ms while CoR/voice activity is asserted. When idle,
  silence — no DTX bandwidth saving (reflector treats absence as
  "node not transmitting").

### 4.3 Floor control

Per-TG state machine on the reflector. The floor is held by an
**implicit lease** that expires when no packets arrive from the
holder within `hangtime_ms` (default 1500 ms). There is no explicit
"I'm done talking" message — radio CoR can drop without warning, and
any explicit-release scheme fails open in the worst way.

```
IDLE
  ── packet from N (auth OK) ──► TALKING(N), broadcast `talker(N)`
  ── packet from N (auth FAIL ×K in window) ──► IDLE (no state change),
                                                 send error + count toward kick
TALKING(N)
  ── packet from N (auth OK)            ──► forward to all others, refresh lease
  ── packet from M ≠ N                  ──► drop, send `floor_denied` to M
                                            (rate-limited 1/500 ms per node)
  ── no packet from N for hangtime_ms   ──► IDLE,
                                            send `talker(null)` to TG,
                                            send `floor_revoked(lease_expired)`
                                                 to N
  ── admin force-release                ──► IDLE,
                                            send `talker(null)` + `floor_revoked(admin)`
  ── auth failures from N exceed cap    ──► IDLE,
                                            send `floor_revoked(auth_failures)` + `mute(loss_too_high)`
```

While `TALKING(N)` the reflector:
- Forwards N's RTP to every other node on the TG (never echoes back
  to N).
- Sends `talker` once on entry and once on exit.
- Sends `floor_denied` to anyone else who tries to talk
  (rate-limited so a stuck-PTT node doesn't flood the WS link).

**Split-brain handling.** If the local node thinks it has the floor
but the reflector doesn't (network blip dropped packets long enough
that hangtime expired), the reflector sends
`floor_revoked(lease_expired)`. Local mod_link clears its "I'm
transmitting upstream" flag immediately. The next outbound packet
starts a fresh `TALKING(N)` on the reflector if the TG is idle, or
gets `floor_denied` if someone else grabbed it during the gap. The
local operator hears nothing unusual unless they were doubled — in
which case the busy tone plays.

### 4.4 Receive side (mod_link)

When mod_link gets an SRTP packet:

1. **Length sanity.** Reject < 12 bytes (RTP header) or > MTU.
   Increment `bad_len` counter.
2. **SRTP decrypt + auth check.** On `srtp_err_status_auth_fail` or
   `_replay_fail`, drop, increment `srtp_auth_fail`. If counter
   exceeds **100 / 10 s window**, log a warning. If it exceeds
   **1000 / 10 s** (key drift or attack), send
   `error(code:"protocol_error")` and force a control-plane reconnect
   (which renegotiates the SRTP master key).
3. **SSRC check.** Must equal `reflector_ssrc` from `login_ok`.
   Anything else is silently dropped (cross-session bleed-through).
4. **Jitter buffer insert.** Target depth 100 ms, hard cap 250 ms.
   Packets older than the playout cursor are dropped (counts toward
   `late`). Buffer overflow drops the oldest packet (counts toward
   `overflow`).
5. **Playout** every 20 ms (audio-thread cadence). Pull one Opus
   frame; on a missing slot, call `opus_decode(NULL)` for PLC.
   After 5 consecutive missing slots, mute output and stop asserting
   PTT until packets resume — a "ghost stream" shouldn't hold the
   local PTT indefinitely.
6. **Decode error** (`opus_decode` < 0). Drop frame, increment
   `decode_err`, substitute a PLC frame. After 10 consecutive decode
   errors, drain the jitter buffer (it's hopelessly corrupt) and send
   a `quality` snapshot immediately so the reflector sees the spike.
7. **Resample 2×** from 24 kHz → audio thread's rate (48 kHz typical)
   via `kerchunk_resample()`.
8. **Mix into live playback path** — same path `mod_freeswitch` uses
   for autopatch live audio. PTT is asserted while audio is being
   mixed; released after `link_tail_ms` (default 500 ms) of silence
   from the reflector.

All counters in this list (`bad_len`, `srtp_auth_fail`, `late`,
`overflow`, `decode_err`, `consecutive_missing`) feed the `quality`
upstream message and the `/admin/api/link` snapshot.

### 4.5 Local TX → reflector (mod_link)

When the local repeater is RX (KERCHEVT_RX_AUDIO):

1. Audio tap (same mechanism mod_recorder uses) hands frames to
   mod_link's encode queue.
2. Encoder thread downsamples 2× to 24 kHz and Opus-encodes with
   60 ms frames (3× audio frames per encoded packet). FEC payload
   is enabled.
3. SRTP-encrypt and send to the reflector's RTP endpoint via the
   same UDP socket that's bound for receive.
4. When the local RX ends (KERCHEVT_RX_END), encode a few trailing
   frames (so the reflector's hangtime kicks in cleanly) and stop
   sending.

If `floor_denied` arrives mid-transmission, the encoder keeps running
(the local op may not even know — they're still keying) but the local
node plays the busy tone over the air every 2 seconds until either
local PTT releases or `floor_denied` stops arriving. We do **not**
stop sending: the local op might reasonably want to "talk over" if
their packets reach the reflector after the current talker drops.

### 4.6 Failure modes — single source of truth

Every condition that can disrupt service maps to (a) a specific
protocol message, (b) a defined client behavior, and (c) something
the operator can see. This table is the contract.

| Failure                                  | Detected by         | Protocol response                          | Client behavior                                                              | Operator visibility                                |
|------------------------------------------|---------------------|--------------------------------------------|------------------------------------------------------------------------------|-----------------------------------------------------|
| TLS handshake fails                      | mod_link            | (none — connection never opens)            | Backoff schedule (§ 6.1), surface last error                                 | "Connecting…" + last_error in `/admin/link.html`    |
| Bad preshared key                        | reflector login     | `login_denied(bad_key)`                    | Stop reconnecting, log alarm                                                 | Red banner: "Auth failed — fix preshared_key"       |
| Unknown node_id                          | reflector login     | `login_denied(unknown_node)`               | Stop reconnecting, log alarm                                                 | Red banner: "Reflector doesn't know this node"      |
| Client too old                           | reflector login     | `login_denied(version_mismatch)`           | Stop reconnecting, log alarm                                                 | Red banner: "Upgrade kerchunk"                      |
| Same node_id already connected           | reflector login     | `login_denied(node_busy)`                  | Backoff 30 s ±20 %                                                           | Yellow: "Another instance with this id is online"   |
| Operator banned this node                | reflector login     | `login_denied(banned)`                     | Stop reconnecting, log alarm                                                 | Red banner: "Banned by reflector operator"          |
| WS disconnect mid-session                | mod_link            | (no clean message — TCP close)             | Backoff schedule, mute received audio                                        | Status: "Reconnecting (attempt N, next in Ms)"      |
| Reflector graceful restart               | reflector           | `reflector_shutdown(deploy, restart_in_s)` | Wait `restart_in_s` ±20 % then reconnect                                     | Status: "Reflector restarting, retry in Ns"         |
| Idle keepalive timeout (30 s)            | either side         | TCP close (no clean message — peer dead)   | Same as WS disconnect                                                        | Same                                                |
| TG change requested, not authorized      | reflector           | `tg_denied(not_authorized)`                | Keep current TG, beep over the air                                            | Status: "TG NNN denied"                            |
| TG change requested, unknown TG          | reflector           | `tg_denied(unknown_tg)`                    | Keep current TG, beep over the air                                           | Status: "TG NNN unknown"                           |
| Reflector config reload moved this node  | reflector           | `tg_membership_changed(old, new, reason)`  | Switch local TG, announce on SSE                                             | Status: "TG changed by reflector to NNN"           |
| Operator on reflector mutes node         | reflector admin     | `mute(reason, retry_in_s)`                 | Stop encoding/sending; keep WS alive                                         | Status: "Muted (reason). Retry in Ns"              |
| Sustained loss > mute_threshold_pct      | reflector quality   | `mute(loss_too_high, retry_in_s)`          | Same as admin mute                                                            | Status: "Muted: link too lossy"                    |
| Two operators key simultaneously         | reflector floor     | `floor_denied(current_talker)` to loser    | Play busy tone over the air every 2 s; keep encoding                         | SSE event; status: "TG busy"                        |
| Local op keys, reflector lost the floor  | reflector floor     | `floor_revoked(lease_expired)`             | Clear local upstream-flag; next packet starts a new floor (or gets denied)   | (silent — normal operation)                        |
| Admin force-released floor               | reflector admin     | `floor_revoked(admin)`                     | Clear local upstream-flag                                                     | Status: "Floor released by op"                     |
| SRTP auth failure (single)               | mod_link            | (drop silently, count)                     | Drop, increment `srtp_auth_fail`                                              | Counter in `/admin/link.html`                       |
| SRTP auth failures > 1000 / 10 s         | mod_link            | `error(protocol_error)` + force reconnect  | Reconnect (renegotiates master key)                                          | Warning: "SRTP key drift, reconnecting"            |
| SRTP auth failures from a node (server)  | reflector           | `floor_revoked(auth_failures)` + `mute`    | (server-side mute, see above)                                                 | Reflector log + dashboard                           |
| Opus decode error                        | mod_link            | (drop frame, PLC substitute)               | Counter; after 10 consecutive, drain JB                                      | Counter in `/admin/link.html`                       |
| Jitter buffer underflow (5 missing)      | mod_link            | (silence + release PTT)                    | Stop asserting PTT until packets resume                                      | Counter; "stream stalled"                          |
| Jitter buffer overflow                   | mod_link            | (drop oldest)                              | Counter                                                                       | Counter in `/admin/link.html`                       |
| Out-of-order packet within JB window     | mod_link            | (insert in order)                          | (normal)                                                                      | Counter                                             |
| Packet older than playout cursor         | mod_link            | (drop)                                     | Counter `late`                                                                | Counter                                             |
| SSRC mismatch                            | mod_link            | (drop silently)                            | Counter                                                                       | Counter                                             |
| Malformed control-plane JSON             | either side         | `error(protocol_error)`                    | Log; if peer sends > 3 in 60 s, kick with `protocol_error`                   | Log entry                                           |
| Reflector internal error                 | reflector           | `error(internal)` then close                | Backoff schedule                                                              | Status                                              |
| Bad TLS cert (verify_peer=true)          | mod_link            | (handshake fails)                          | Same as TLS handshake fails                                                   | Status: "Reflector cert untrusted"                 |
| NAT mapping expired                      | mod_link            | (silent — no replies)                      | UDP keepalive prevents this; if it happens, keepalive timeout fires          | Status: "Idle timeout, reconnecting"               |
| Local config invalid at load             | mod_link configure  | (module disables itself)                   | Log + don't connect                                                          | Status: "Config error: <field>"                    |
| `[link] enabled = false`                 | mod_link configure  | (module idle)                              | Don't connect                                                                | Status: "Disabled"                                 |

Counters mentioned above are exposed via the `quality` upstream
message every 10 s and via the `/admin/api/link` JSON snapshot.

---

## 5. Reflector — `kerchunk-reflectd`

Single-binary daemon. Mongoose handles both the WebSocket and the
UDP socket; audio bridging is in-process state with no per-packet
allocations.

**Config** (`/etc/kerchunk-reflectd/reflectd.conf`, INI — see
`reflectd.conf.example` shipped with the deb for the canonical form):

```ini
[reflector]
listen_url = wss://0.0.0.0:8443/link
tls_cert = /etc/letsencrypt/live/example.com/fullchain.pem
tls_key  = /etc/letsencrypt/live/example.com/privkey.pem
rtp_port = 7878
rtp_advertise_host = reflector.example.com

; HTTP Basic for the admin dashboard + JSON API. Both fields required.
admin_user     = admin
admin_password = <strong>

dashboard_dir = /usr/share/kerchunk-reflectd/web   ; static assets

min_client_version =                       ; lex compare; empty = no minimum
keepalive_s = 15                           ; ping every N s
hangtime_ms = 1500                         ; floor lease

; Auto-mute thresholds (server-driven)
mute_threshold_pct = 15                    ; sustained loss% from a node
                                           ; (mute on ≥2 of last 3 reports)
mute_window_s = 30
auth_fail_kick = 200                       ; SRTP auth fails before kick (per 30s)
max_reconnects_per_node_per_min = 6

; Per-call recording + CDR (opt-in)
recording_enabled       = off
recording_dir           = /var/lib/kerchunk-reflectd/recordings
recording_max_age_days  = 30               ; auto-pruned hourly

[talkgroup.4123]
name = "Pacific Northwest"
nodes = WK7ABC-1, WK7DEF-1, WK7GHI-1

[node.WK7ABC-1]
preshared_key_hex = <64 hex chars>  ; 32 bytes
allowed_tgs = 4123, 4124
default_tg = 4123
```

The dashboard's `[talkgroup.N]` editor automatically extends each
listed member's `[node.X] allowed_tgs` so both sides agree (the auth
check requires both to list each other). Hand-edited configs need to
maintain that invariant manually.

**HTTP API** (HTTP Basic via `admin_user` / `admin_password`. Pair the
dashboard with TLS in production; both `/api/...` and `/admin/...` are
gated by the same realm so browsers cache credentials per origin):

| Endpoint                                  | Purpose                                            |
|-------------------------------------------|----------------------------------------------------|
| `GET  /api/state`                         | Roster + per-TG state + floor + per-node counters  |
| `GET  /api/events`                        | SSE stream of state snapshots — push, no polling   |
| `GET  /api/recordings[?date=YYYY-MM-DD]`  | List CDR days, or that day's per-call rows         |
| `GET  /api/recording?path=<rel>`          | Stream a per-call WAV (sandboxed)                  |
| `DELETE /api/recording?path=<rel>`        | Remove one WAV + its CDR row                       |
| `DELETE /api/recordings?date=YYYY-MM-DD`  | Remove a whole day's WAVs + CSV                    |
| `POST /api/admin/kick`                    | `{node_id, reason}` → `kicked(admin_action)`       |
| `POST /api/admin/mute`                    | `{node_id, retry_s}` → `mute(admin_action)`        |
| `POST /api/admin/unmute`                  | `{node_id}` → `unmute`                             |
| `POST /api/admin/release-floor`           | `{tg}` → `floor_revoked(admin)` to current talker  |
| `POST /api/admin/reload`                  | Re-read config; emits `tg_membership_changed` etc. |
| `POST /api/admin/prune-now`               | Run the recording-prune sweep immediately          |
| `GET  /api/admin/config`                  | Editable view: roster + TGs (PSKs included)        |
| `POST/DELETE /api/admin/node`             | Create/update/delete `[node.<id>]` sections        |
| `POST/DELETE /api/admin/talkgroup`        | Create/update/delete `[talkgroup.N]` sections      |

`/api/events` is the dashboard's data source — it emits a snapshot on
every state change AND a 1 s heartbeat for live counter updates, so
the UI is reactive without polling.

Per-node metrics in `/api/state` (and SSE snapshots): `connected_at`,
`uptime_s`, `quiet_ms`, `rtp_in`, `rtp_out`, `bytes_in`, `bytes_out`,
`floor_holds`, `floor_seconds` (total airtime), `last_loss_pct`,
`srtp_fail_30s`, `client_version`, `mute_reason`. Per-TG includes
`talker_for_ms` for live "talking for" display.

Each admin POST is logged with target node + timestamp.

**Logging:** every connect / disconnect / TG-change / floor-grant to
the events log. Same `events.log` style as kerchunk so a single
log-aggregation pipeline works.

---

## 6.1 Reconnect / backoff policy

Network-level disconnects (TLS handshake fail, TCP close, idle
timeout, transient `login_denied`, `error(internal)`) trigger
exponential backoff:

```
delay_ms = min(reconnect_max_ms,
               reconnect_min_ms * 2^(attempt - 1))
delay_ms *= jitter_factor   ; uniform [0.8, 1.2]
attempt = min(attempt + 1, 16)
```

Successful login resets `attempt` to 0. **Permanent codes**
(`bad_key`, `unknown_node`, `banned`, `version_mismatch`) move
mod_link to `state:"stopped"`. The module does not retry until
`link reconnect` (CLI) or `link clear-alarm` is run, or the config is
reloaded with a changed key. This avoids hammering the reflector
after an admin revokes a node.

`reflector_shutdown(restart_in_s)` overrides the schedule for one
cycle: wait `restart_in_s ±20 %` before the next attempt, then resume
normal backoff if reconnect fails.
