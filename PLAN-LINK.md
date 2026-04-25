# Plan: repeater linking — central reflector + mod_link

**Status:** Plan — not yet implemented. Author: pair, 2026-04-25.
**Branch:** `link` (proposed).
**Goal:** Link two or more kerchunk repeaters together over the
internet so a transmission on Repeater A is heard on Repeater B
(and C, D, …) with sub-200 ms one-way latency, encrypted in transit,
authenticated per node, and with proper floor control so two
operators on different repeaters don't transmit on top of each
other. Architected around a **central reflector** that nodes log
in to — not peer-to-peer mesh.
**No backwards compatibility constraints:** brand-new module,
brand-new daemon, brand-new wire protocol. Not interoperable with
AllStarLink, M17, Echolink, or DMR — those exist if interop is
the actual goal (see § 12).

---

## 1. Why

Kerchunk today controls a single repeater site. Operators commonly
want to link two or more sites together so members in adjacent
geography hear each other on a shared talkgroup. The existing
options for an open-source GMRS / part-95 controller are:

- **AllStarLink / app_rpt** — works, but requires Asterisk and
  IAX2; not a clean fit alongside kerchunk's existing
  module/audio architecture, and the protocol is showing its age
  (uLaw, no real authentication beyond shared secret strings).
- **DMR / D-STAR / YSF networks** — different physical layer
  entirely; not applicable to FM repeaters.
- **EchoLink** — proprietary-ish, GSM codec, and the directory
  server model assumes individual user nodes more than
  full-time linked repeaters.
- **M17 reflectors** — modern and clean, but built around Codec 2
  + 4FSK; bridging to/from analog FM is an extra hop.

We have a working FM repeater controller, a queued audio model,
SSE event bus, and FreeSWITCH integration for live audio
(autopatch). Adding a link is mostly **a new audio source/sink
that talks to a reflector over SRTP+Opus**.

This plan specifies that reflector and the local module that
talks to it. It is greenfield by design — small surface area,
modern primitives (Opus FEC, SRTP, TLS), and central control over
floor / mute / logging.

---

## 2. Scope and non-goals

**In scope:**

- A new daemon `kerchunk-reflectd` (separate binary, separate
  package eventually) that accepts authenticated node connections
  and bridges audio between nodes that share a talkgroup.
- A new module `mod_link.c` in kerchunk that connects out to a
  reflector, encodes local RX as Opus/SRTP toward the reflector,
  and decodes incoming Opus/SRTP into a live playback path on the
  local repeater.
- A wire protocol (`include/kerchunk_link_proto.h`) shared by
  both sides — control plane over TLS-WebSocket, audio plane
  over SRTP/UDP.
- Floor control on the reflector: per-talkgroup "current talker"
  with hangtime; reject contention.
- Admin observability: `/admin/link.html` shows current state,
  reflector reports node roster + per-TG talker.
- DTMF command to switch talkgroup at runtime (operator-initiated).
- Config + docs.

**Out of scope (v1):**

- Mesh / peer-to-peer (every node opens a TCP connection to every
  other node). Reflector model is intentional — see § 4.
- Transcoding to/from non-Opus codecs.
- Federated reflectors (one reflector talking to another to relay
  a TG). Single reflector per TG in v1.
- Mobile / handheld clients connecting to the reflector
  (Echolink-style "user nodes"). Reflector accepts repeater nodes
  only in v1.
- Web-based listen-in. The reflector doesn't expose audio over
  HTTP; if that's wanted, point Icecast at a dummy node.
- Cert-based mutual TLS auth. Pre-shared key per node in v1;
  certs are a v2 hardening step.
- Authoritative national directory (à la AllStarLink). Self-hosted
  reflector; operators publish their own node-id space.
- Bridging to AllStarLink / M17 / Echolink. § 12 lists these as
  "if you actually need this, use the existing thing."

---

## 3. Why central reflector, not mesh

Three forces push toward central:

1. **NAT / firewall.** Repeater sites are routinely behind NAT.
   A reflector means every node makes one outbound connection
   (TLS:443) and a UDP keepalive — no per-site port-forwarding,
   no inbound ACL changes.

2. **Floor control is trivially correct with one arbiter.** The
   reflector knows every node's RX state; granting "you have the
   floor on TG 4123" is a single-writer decision. With mesh,
   every pair has to negotiate or you risk doubling.

3. **One place to log, mute, kick.** Operations matters. A
   reflector is the natural seam for "node X is misbehaving,
   suspend it" without having to coordinate across repeater sites.

The cost is that the reflector becomes infrastructure someone
has to run. For a network with >2 repeaters this is a feature,
not a bug.

---

## 4. Architecture

```
                ┌─────────────────────┐
                │  kerchunk-reflectd  │
                │                     │
                │  TLS-WS :8443       │  control
                │  UDP    :7878       │  audio (SRTP)
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

### 4.1 Control plane — TLS WebSocket

A long-lived WebSocket from each node to the reflector carries
small JSON messages. Frames are line-based (one JSON object per
WS message). Direction notation: **N→R** node-to-reflector,
**R→N** reflector-to-one-node, **R→\*** broadcast to TG members.

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
`banned`, `version_mismatch`) → client stops reconnecting and
surfaces an admin alarm. **Transient** (`node_busy`, `internal`)
→ client backs off and retries.

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

**`ping`** / **`pong`**: either side may initiate; both sides
SHOULD send `ping` every 15 s if they haven't sent any other
message in that window. Receiver replies with `pong` immediately.
If no traffic (any direction) for 30 s, treat the connection as
dead and reconnect.
```json
{"type":"ping","seq":42}    {"type":"pong","seq":42}
```

#### 4.1.2 Talkgroup operations

**`set_tg`** (N→R, operator-initiated DTMF):
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
If the node is removed from all TGs, `new_tg` is `null` and the
node is effectively muted until an operator picks a new TG.

**`mute`** / **`unmute`** (R→N): reflector keeps the connection
but stops forwarding the node's audio (or resumes):
```json
{"type":"mute","reason":"loss_too_high","retry_in_s":60}
{"type":"unmute"}
```

#### 4.1.3 Floor control messages

**`talker`** (R→\*, broadcast to all nodes on a TG):
```json
{"type":"talker","tg":4123,"node_id":"WK7ABC-1","since":1777068600}
```
Sent when a node grabs the floor. When the talker releases (no
packets for `hangtime_ms`), reflector sends:
```json
{"type":"talker","tg":4123,"node_id":null,"since":1777068604}
```

**`floor_denied`** (R→N, when this node's RTP arrives but the TG
already has a talker — rate-limited to one per 500 ms per node):
```json
{"type":"floor_denied","tg":4123,"current_talker":"WK7DEF-1"}
```
Local mod_link plays a brief "TG busy" tone over the air
(configurable via `link_busy_tone`) so the operator hears
contention immediately.

**`floor_revoked`** (R→N, the floor was taken away from this node
mid-transmission):
```json
{"type":"floor_revoked","tg":4123,"code":"lease_expired"}
```
`code` ∈ {`lease_expired` (no packets received within hangtime
window — common, treated as normal end), `admin` (operator force-
released), `auth_failures` (too many SRTP auth fails — node is
likely misconfigured)}.

#### 4.1.4 Quality and adaptation

**`quality`** (N→R, every 10 s when connected):
```json
{"type":"quality","loss_pct":1.4,"jitter_ms":12,
 "jb_depth_ms":98,"decode_errs":0,"window_s":10}
```
Drives reflector roster display, kick/mute decisions, and
target-bitrate hints.

**`target_bitrate`** (R→N, advisory, not enforced):
```json
{"type":"target_bitrate","bps":20000}
```
Client SHOULD reconfigure its Opus encoder near this value.
Reflector sends this when a peer on the same TG reports
sustained `loss_pct > 8 %` so all senders gracefully back off.

#### 4.1.5 Generic error

For unexpected conditions that don't fit a specific message:
```json
{"type":"error","code":"protocol_error","msg":"unknown type 'foo'"}
```
The receiver SHOULD log and continue. Repeated `error` from the
same peer (>3 in 60 s) → other side closes the connection with
`kicked` code `protocol_error`.

#### 4.1.6 Error code enum

Single enum used in `login_denied.code`, `kicked.code`,
`tg_denied.code`, `floor_revoked.code`, `mute.reason`, and `error.code`.

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
- **SRTP:** AES-128-CTR + HMAC-SHA1-80 (the libsrtp default
  profile `SRTP_AES128_CM_HMAC_SHA1_80`). Master key + salt come
  from the login response. SRCP rollover is large enough to never
  recycle in a session lifetime.
- **SSRC:** node-side SSRC chosen by node, server-side SSRC chosen
  by reflector. Both communicated in login_ok so the receiver can
  filter properly.
- **Send timing:** when local repeater is RX'ing, encode and send
  every 60 ms while CoR/voice activity is asserted. When idle,
  silence — no DTX bandwidth saving (reflector treats absence as
  "node not transmitting").

### 4.3 Floor control

Per-TG state machine on the reflector. The floor is held by an
**implicit lease** that expires when no packets arrive from the
holder within `hangtime_ms` (default 1500 ms). There is no
explicit "I'm done talking" message — radio CoR can drop without
warning, and any explicit-release scheme fails open in the worst
way.

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
- Forwards N's RTP to every other node on the TG (never echoes
  back to N).
- Sends `talker` once on entry and once on exit.
- Sends `floor_denied` to anyone else who tries to talk
  (rate-limited so a stuck-PTT node doesn't flood the WS link).

**Split-brain handling.** If the local node thinks it has the
floor but the reflector doesn't (network blip dropped packets
long enough that hangtime expired), the reflector sends
`floor_revoked(lease_expired)`. Local mod_link clears its
"I'm transmitting upstream" flag immediately. The next outbound
packet starts a fresh `TALKING(N)` on the reflector if the TG
is idle, or gets `floor_denied` if someone else grabbed it during
the gap. The local operator hears nothing unusual unless they
were doubled — in which case the busy tone plays.

### 4.4 Receive side (mod_link)

When mod_link gets an SRTP packet:

1. **Length sanity.** Reject < 12 bytes (RTP header) or > MTU.
   Increment `bad_len` counter.
2. **SRTP decrypt + auth check.** On `srtp_err_status_auth_fail`
   or `_replay_fail`, drop, increment `srtp_auth_fail`. If
   counter exceeds **100 / 10 s window**, log a warning. If it
   exceeds **1000 / 10 s** (key drift or attack), send
   `error(code:"protocol_error")` and force a control-plane
   reconnect (which renegotiates the SRTP master key).
3. **SSRC check.** Must equal `reflector_ssrc` from `login_ok`.
   Anything else is silently dropped (cross-session bleed-through).
4. **Jitter buffer insert.** Target depth 100 ms, hard cap 250 ms.
   Packets older than the playout cursor are dropped (counts
   toward `late`). Buffer overflow drops the oldest packet
   (counts toward `overflow`).
5. **Playout** every 20 ms (audio-thread cadence). Pull one Opus
   frame; on a missing slot, call `opus_decode(NULL)` for PLC.
   After 5 consecutive missing slots, mute output and stop
   asserting PTT until packets resume — a "ghost stream"
   shouldn't hold the local PTT indefinitely.
6. **Decode error** (`opus_decode` < 0). Drop frame, increment
   `decode_err`, substitute a PLC frame. After 10 consecutive
   decode errors, drain the jitter buffer (it's hopelessly
   corrupt) and send a `quality` snapshot immediately so the
   reflector sees the spike.
7. **Resample 2×** from 24 kHz → audio thread's rate (48 kHz
   typical) via `kerchunk_resample()`.
8. **Mix into live playback path** — same path `mod_freeswitch`
   uses for autopatch live audio. PTT is asserted while audio
   is being mixed; released after `link_tail_ms` (default 500 ms)
   of silence from the reflector.

All counters in this list (`bad_len`, `srtp_auth_fail`, `late`,
`overflow`, `decode_err`, `consecutive_missing`) feed the
`quality` upstream message and the `/admin/api/link` snapshot.

### 4.5 Local TX → reflector (mod_link)

When the local repeater is RX (KERCHEVT_RX_AUDIO):

1. Audio tap (same mechanism mod_recorder uses) hands frames
   to mod_link's encode queue.
2. Encoder thread downsamples 2× to 24 kHz and Opus-encodes with
   60 ms frames (3× audio frames per encoded packet). FEC
   payload is enabled.
3. SRTP-encrypt and send to the reflector's RTP endpoint via
   the same UDP socket that's bound for receive.
4. When the local RX ends (KERCHEVT_RX_END), encode a few
   trailing frames (so the reflector's hangtime kicks in cleanly)
   and stop sending.

If `floor_denied` arrives mid-transmission, the encoder keeps
running (the local op may not even know — they're still keying)
but the local node plays the busy tone over the air every
2 seconds until either local PTT releases or `floor_denied`
stops arriving. We do **not** stop sending: the local op might
reasonably want to "talk over" if their packets reach the
reflector after the current talker drops.

### 4.6 Failure modes — single source of truth

Every condition that can disrupt service should map to (a) a
specific protocol message, (b) a defined client behavior, and
(c) something the operator can see. This table is the contract.

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
| NAT mapping expired                      | mod_link            | (silent — no replies)                      | UDP keepalive (§ 9.5) prevents this; if it happens, keepalive timeout fires  | Status: "Idle timeout, reconnecting"               |
| Local config invalid at load             | mod_link configure  | (module disables itself)                   | Log + don't connect                                                          | Status: "Config error: <field>"                    |
| `[link] enabled = false`                 | mod_link configure  | (module idle)                              | Don't connect                                                                | Status: "Disabled"                                 |

Counters mentioned above are exposed via the `quality` upstream
message every 10 s and via the `/admin/api/link` JSON snapshot.

---

## 5. Reflector — `kerchunk-reflectd`

Separate small daemon, written in C to match the rest of the
project. Single-threaded mongoose for the WebSocket + UDP socket
(mongoose handles both); audio bridging is in-process state with
no per-packet allocations.

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
mute_threshold_pct = 8                     ; sustained loss% from a node
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

**HTTP API** (HTTP Basic via `admin_user` / `admin_password`. Pair the
dashboard with TLS in production; both `/api/...` and `/admin/...` are
gated by the same realm so browsers cache credentials per origin):

| Endpoint                              | Purpose                                            |
|---------------------------------------|----------------------------------------------------|
| `GET  /api/state`                     | Roster + per-TG state + floor + per-node counters  |
| `GET  /api/events`                    | SSE stream of state snapshots — push, no polling   |
| `GET  /api/recordings[?date=YYYY-MM-DD]` | List CDR days, or that day's per-call rows      |
| `GET  /api/recording?path=<rel>`      | Stream a per-call WAV (sandboxed)                  |
| `POST /api/admin/kick`                | `{node_id, reason}` → `kicked(admin_action)`       |
| `POST /api/admin/mute`                | `{node_id, retry_s}` → `mute(admin_action)`        |
| `POST /api/admin/unmute`              | `{node_id}` → `unmute`                             |
| `POST /api/admin/release-floor`       | `{tg}` → `floor_revoked(admin)` to current talker  |
| `POST /api/admin/reload`              | Re-read config; emits `tg_membership_changed` etc. |

`/api/events` is the dashboard's data source — it emits a snapshot on
every state change AND a 1 s heartbeat for live counter updates, so the
UI is reactive without polling.

Per-node metrics in `/api/state` (and SSE snapshots): `connected_at`,
`uptime_s`, `quiet_ms`, `rtp_in`, `rtp_out`, `bytes_in`, `bytes_out`,
`floor_holds`, `floor_seconds` (total airtime), `last_loss_pct`,
`srtp_fail_30s`, `client_version`, `mute_reason`. Per-TG includes
`talker_for_ms` for live "talking for" display.

Each admin POST is logged with target node + timestamp.

**Logging:** every connect / disconnect / TG-change / floor-grant
to the events log. Same `events.log` style as kerchunk so a single
log-aggregation pipeline works.

---

## 6. Local module — `mod_link.c`

**Config** (`[link]` section in `kerchunk.conf`):
```ini
[link]
enabled = true
reflector_ws = wss://reflector.example.com:8443
node_id = WK7ABC-1
preshared_key_hex = <64 hex chars>
default_tg = 4123                ; matches reflector default
link_tail_ms = 500               ; PTT hold after last frame
jitter_target_ms = 100
jitter_max_ms = 250
opus_sample_rate = 24000         ; 16000 / 24000 / 48000
opus_bitrate = 32000
opus_loss_perc = 10              ; FEC tuning hint
verify_peer = true               ; reflector cert validation
busy_tone = true                 ; play TG-busy beep on floor_denied
busy_tone_interval_ms = 2000
reconnect_min_ms = 1000          ; backoff schedule (§ 6.1)
reconnect_max_ms = 60000
nat_keepalive_ms = 10000         ; UDP heartbeat when not transmitting
mute_threshold_pct = 8           ; client-side warning only
```

**Module surface:**
- `load()` — open libopus encoder/decoder, libsrtp policies,
  kick off WS connect thread.
- `configure()` — read `[link]` config; permanent-error states
  (bad_key, banned, version_mismatch) are cleared by reload.
- Subscribes to `KERCHEVT_RX_AUDIO` (encode path),
  `KERCHEVT_RX_END` (drain encoder).
- Owns one POSIX thread for the WebSocket + UDP socket
  (mongoose-driven). Audio encode and decode happen on this
  thread; jitter buffer playout is driven by the existing audio
  thread via a frame-pull callback.
- Registers DTMF command `*73<tgnum>#` to request a TG change
  (defined in mod_dtmfcmd, dispatched to mod_link via
  `KERCHEVT_CUSTOM + N`).
- CLI commands: `link status`, `link tg <n>`, `link reconnect`,
  `link clear-alarm` (resets a permanent-error stop).
- Publishes a per-state SSE snapshot for the admin UI on every
  state change and at most every 1 s:
  ```json
  {"type":"link","state":"connected","tg":4123,"tg_name":"PNW",
   "current_talker":"WK7DEF-1","since":1777068600,
   "session_uptime_s":1234,"reconnect_attempt":0,"last_error":null,
   "jb_depth_ms":92,"loss_pct":0.4,"jitter_ms":11,
   "bitrate_bps":32000,"target_bitrate_bps":32000,
   "counters":{"sent":12345,"recv":67890,
               "srtp_auth_fail":0,"decode_err":0,
               "late":2,"overflow":0,"reconnects":1}}
  ```
  `state` ∈ {`disabled`, `connecting`, `connected`,
  `reconnecting`, `muted`, `stopped`}. `stopped` means a
  permanent-error code was received and the operator must
  intervene; `last_error` carries the code + message.

#### 6.1 Reconnect / backoff policy

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
`link reconnect` (CLI) or `link clear-alarm` is run, or the
config is reloaded with a changed key. This avoids hammering the
reflector after an admin revokes a node.

`reflector_shutdown(restart_in_s)` overrides the schedule for
one cycle: wait `restart_in_s ±20 %` before the next attempt,
then resume normal backoff if reconnect fails.

**Live audio integration:** mod_link's decoded output is fed
into the same live-mix path `mod_freeswitch` already uses
(see `freeswitch_audio_in` in mod_freeswitch.c — the per-frame
ringbuffer pattern). Reusing that path means the audio thread
already knows how to assert/release PTT for live continuous audio.

---

## 7. Admin UI — `/admin/link.html`

Single-page status view in the existing admin style. Sections:

- **Connection**: reflector URL, status (Connected / Reconnecting /
  Down), session uptime, last error.
- **Talkgroup**: current TG number + name, button to change
  (POST to `/admin/api/link/tg`).
- **Live state**: current talker (large), jitter-buffer depth,
  packet-loss percent (rolling 30 s), bitrate.
- **Roster** (proxied from reflector `/api/state`): all nodes on
  the current TG and their state (idle / talking / offline).
- **Counters**: total packets sent / received / dropped, bytes,
  reconnects.

Updates over the existing SSE bus (`type: link` snapshot).

---

## 8. Files touched

| File | Change | Approx LOC |
|---|---|---|
| `src/reflectd/main.c` | Reflector daemon (mongoose loop, SRTP, floor control) | ~1200 |
| `src/reflectd/talkgroup.c` | Per-TG state + floor logic | ~250 |
| `src/reflectd/auth.c` | HMAC challenge / preshared-key login | ~150 |
| `src/reflectd/state_http.c` | Admin HTTP API (state + kick/mute/release/reload) | ~280 |
| `include/kerchunk_link_proto.h` | Shared wire protocol constants + JSON keys | ~80 |
| `modules/mod_link.c` | Local module: WS client, Opus encode/decode, SRTP, jitter buffer, reconnect/backoff, busy tone | ~1200 |
| `modules/mod_dtmfcmd.c` | Register `*73<n>#` and forward to mod_link | ~10 |
| `web/admin/link.html` | Admin status page | ~350 |
| `web/admin/index.html` | Add "Link" nav button | ~3 |
| `modules/mod_web.c` | Two endpoints: `GET /admin/api/link`, `POST /admin/api/link/tg` | ~120 |
| `configure.ac` + `Makefile.am` | libopus, libsrtp2 detection; new daemon binary | ~40 |
| `debian/control` | Add `libopus0`, `libsrtp2-1` runtime deps; new `kerchunk-reflectd` package | ~30 |
| `debian/kerchunk-reflectd.service` | systemd unit | ~20 |
| `README.md` | Document the link feature + reflector | ~40 |
| `USAGE.md` | Operator setup walkthrough | ~50 |
| `ARCHITECTURE.md` | Add to module list + diagram | ~25 |

Total: ~3850 lines across 16 files. Roughly 55% reflector,
35% mod_link, 10% UI/glue/docs.

---

## 9. Risks and edge cases

> **Note:** Per-message failure handling, error codes, reconnect
> backoff, and operator-visibility mappings are in § 4.6. This
> section covers architectural / design risks that aren't a single
> protocol message away from being fixed.

1. **Audio loop.** If a node both transmits to TG and re-broadcasts
   what it receives back over the air, and a remote node hears that
   off-air and re-transmits, an echo loop forms. Reflector never
   echoes to sender (covered), and mod_link mutes its TX-to-reflector
   path while it's playing reflector audio (i.e. while local PTT is
   driven by mod_link itself). The latter is the critical
   safeguard — implement and test explicitly.

2. **PTT hangtime tuning.** Too short → choppy chops between Opus
   frames at packet-loss boundaries. Too long → next remote talker
   waits awkwardly. Default 500 ms; expose `link_tail_ms`.

3. **Clock skew between nodes.** Opus packets timestamp at 48 kHz
   per RFC 7587. Jitter buffer handles ±200 ms skew comfortably.
   Beyond that the playout cursor will silently slip — log when
   buffer continually trends low or high.

4. **Reflector single point of failure.** If reflector dies, all
   linked repeaters become independent again (graceful — the radio
   side keeps working). Document that the reflector should run on
   a small cloud VM with `Restart=always`. Multi-reflector HA is
   v3+ work.

5. **NAT keepalive.** UDP NAT mappings expire (often 30–60 s).
   The 60 ms RTP cadence keeps the pinhole open while transmitting,
   but during long quiet periods the mapping can close. Send a
   1-byte STUN-style keepalive on the UDP socket every 10 s when
   not transmitting.

6. **Auth replay.** The login HMAC binds to a server-issued
   challenge per connection, so replaying a captured login
   attempt against a new connection fails. Document key rotation
   procedure (admin updates `preshared_key_hex` on both sides;
   node reconnects).

7. **Clock for HMAC.** No time-based component in the auth scheme,
   so no NTP dependency. (Time-of-day matters only for log
   correctness.)

8. **SRTP key reuse.** Each session gets a fresh master key + salt
   from the reflector. AES-CTR catastrophe-on-reuse is handled by
   the per-session derivation — packet counters never roll
   inside one session at 60 ms cadence (would take >2000 years
   at 2³² packets / 16 packets per second).

9. **Float vs. int audio.** Opus library wants `opus_int16` or
   `float`. Match kerchunk's existing audio convention (`int16_t`)
   and use the int variant. No conversion overhead.

10. **Packet of death.** Untrusted RTP from reflector goes through
    libsrtp's auth check first — bad packets are dropped before
    reaching the Opus decoder. Bad reflector cert (in unverified
    mode) is the operator's call; default is `verify_peer=true`.

11. **Codec CPU on Pi.** Opus encode/decode at 24 kHz SWB is
    ~1–2% CPU on a Pi 4. Not a concern. The 48 ↔ 24 kHz resample
    is 2× decimation / interpolation, cheaper than the 48/16 = 3
    case. SRTP is in the same ballpark.

12. **Talkgroup-of-the-month config drift.** Adding/removing a
    node from a TG requires editing the reflector config. No
    runtime API for TG membership in v1 (operator change → SIGHUP).

13. **Audio quality auditing.** Recording a link session: today
    mod_recorder taps the local RX. Linked-in audio coming from
    the reflector should also be recorded as a TX-side event so
    CDR captures it. mod_recorder already has TX recording — wire
    mod_link's playback through it.

14. **Loss-burst cliff.** Opus PLC handles ≤ 3 missing frames
    gracefully; beyond that intelligibility falls fast. Surface
    `loss_pct > 5%` as a warning in the admin UI so the operator
    knows the link is unhealthy before users complain.

15. **Two reflectors / accidental dual-link.** A node connects to
    reflector A on TG 4123 and also to reflector B on TG 4123 —
    audio loops between them via the radio. Single-reflector
    enforcement at config load: only one `[link]` block, only one
    `reflector_ws`. No multi-link in v1.

---

## 10. Sequencing

1. **Wire-protocol skeleton.** Define `kerchunk_link_proto.h`
   and write a tiny test client/server pair that does login +
   keepalive over plain TCP (no TLS, no SRTP yet). Confirm the
   message shapes feel right before committing them. (1 session)

2. **Reflector audio-blind bridge.** TLS-WebSocket login,
   talkgroup membership, control-plane only. Two test clients
   connect; "talker" announcements work. No audio yet. (1–2 sessions)

3. **Audio plane bring-up.** SRTP key delivery, libsrtp init,
   send/receive raw RTP between two test clients (Opus-encoded
   audio fed in via wav loop). Reflector forwards audio without
   floor control. Verify with `tcpdump` that traffic is encrypted.
   (2 sessions)

4. **Floor control + jitter buffer.** Reflector enforces single
   talker; client jitter buffer + Opus PLC. Two clients pinged
   from each side, confirm hangtime + contention rejection.
   (1–2 sessions)

5. **mod_link integration with kerchunk core.** Wire to live
   audio in/out, PTT keying, RX tap encoding. End-to-end audio
   over a real radio path between two repeaters in lab. (2 sessions)

6. **Admin UI + DTMF TG switch.** `/admin/link.html`, `*73<n>#`
   command, SSE state snapshots. (1 session)

7. **Hardening pass — exercise every row in § 4.6.** Replay
   tests, malformed packets, NAT keepalive, reconnect-on-death,
   key rotation, cert pinning option, admin kick / mute /
   release-floor, reflector graceful shutdown with backoff
   jitter, permanent-error stop-and-alarm. The §4.6 table is the
   acceptance checklist for this phase — each row gets a smoke
   test (manual or scripted) before declaring the link done.
   (2 sessions)

8. **Packaging.** Separate `kerchunk-reflectd` deb, systemd
   unit, default config + example certs. (1 session)

9. **Docs sweep.** README, USAGE walkthrough ("set up a 2-node
   link in 10 minutes"), ARCHITECTURE update. (1 session)

Each step is independently shippable — kerchunk core never
breaks because mod_link is gated by `[link] enabled = false`
default.

---

## 11. Future extensions (not v1)

- **Federated reflectors.** Reflector-to-reflector trunking so a
  TG can span multiple administrative domains (each domain runs
  its own reflector + its own auth space).
- **Mutual TLS.** Replace preshared keys with per-node client
  certs issued by an operator-run mini-CA.
- **Web client / listen-in.** Browser-based receiver via WebRTC
  — useful for emergency / event monitoring.
- **Bridging modules.** Optional gateways to AllStarLink (IAX2)
  or M17 reflectors for users who want both networks.
- **Per-TG record at the reflector.** Centralized recording for
  archival, with each node only recording its local audio.
- **DTMF signalling over the link.** Pass DTMF digits as RTP
  named-events (RFC 4733) so a remote node can trigger commands.
  Optional and policy-controlled (you probably don't want a
  remote operator running `*911#` on your repeater).
- **Adaptive bitrate.** Lower Opus bitrate when loss > 8% sustained.
- **Multi-talker mixing.** Today: only one talker at a time per
  TG. Some networks like double-keying being mixed instead of
  rejected — opt-in mode where reflector mixes up to N sources.
- **Ed25519-signed announcements.** Reflector signs `talker` /
  state events so a downstream consumer can verify provenance.
- **Configurable codec.** `opus_sample_rate` is already a config
  knob (default 24 kHz SWB); operators can drop to 16 kHz WB
  for very low-bandwidth links or bump to 48 kHz fullband for
  studio / music bridges.

These are deferred until someone actually asks for them.

---

## 12. If you actually need interop

This plan is greenfield. If the goal is "be reachable from
existing repeater operators today," consider these instead:

- **AllStarLink** — by far the largest existing FM-repeater linking
  network. Use `app_rpt` on Asterisk; kerchunk would expose a
  USRP/chan_simpleusb-style audio interface to Asterisk locally.
  Trade-off: pulls in Asterisk and IAX2.

- **M17 reflectors** — open protocol, modern codec (Codec 2),
  cleaner spec. Bridging to FM means transcoding Codec 2 ↔ PCM.
  Library: `m17-cxx-demod` / `mvoice`. Smaller install base
  than AllStarLink but growing.

- **Echolink** — proprietary protocol but reverse-engineered;
  conference servers exist. Not a great fit for "always-linked"
  repeaters (the model is more user-station oriented).

The decision tree is: **existing user base matters more than
clean design? → AllStarLink. Clean design matters more than
existing user base? → this plan or M17.** Both are defensible.
