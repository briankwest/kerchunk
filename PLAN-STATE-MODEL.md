# Plan: Unified RX / TX state machines with a reflective UI

**Status:** Plan — not yet implemented. Author: pair, 2026-04-24.
**Branch:** `state-model`.
**Goal:** Make the backend the single source of truth for every
"what's the repeater doing right now" fact. The UI reads; it never
synthesises.
**No backwards compatibility:** this is a first-party repeater
controller; there are no external SSE consumers or third-party
dashboards to preserve. Rename freely, drop legacy fields in the
same commit that replaces them.

---

## 1. Why

### Current state is split-brained

- **RX is clean.** `mod_repeater` owns a proper 5-state FSM
  (`IDLE/RECEIVING/TAIL_WAIT/HANG_WAIT/TIMEOUT`) with a single
  `change_state()` choke-point that fires
  `KERCHEVT_STATE_CHANGE`. The UI subscribes to that event and
  reflects it. That path works.
- **TX is synthesised.** `get_tx_state()` in `src/main.c:935-949`
  returns one of four labels, derived on demand from
  `g_audio_state` fields and the PTT refcount. No events fire on
  TX transitions. The UI calls `updateTxState(...)` from SIX
  different JS handlers in `web/admin/index.html`, each flipping
  the label based on an individual edge event
  (`cor_assert`, `cor_drop`, `ptt_drop`, `queue_drain`,
  `queue_complete`). After the first `/api/status` poll, the
  browser — not the daemon — decides what TX state is.
- **Two parallel worldviews.** Whenever the daemon's derived TX
  label disagrees with the browser's synthesised one, the
  dashboard lies. Operators notice this as "the log says X but
  the web says Y" — which is the bug that prompted this plan.

### Secondary problems uncovered

- `TX_QUEUE` conflates three distinct phases (tx_delay silence /
  queue drain / tx_tail silence) so the UI shows "queue" for the
  entire TX cycle even though the radio behaviour differs by
  phase.
- `TX_TAIL` in `get_tx_state()` only fires on software-relay
  drain or on "PTT held but not by the queue." The common case —
  queue-driven tx_tail silence — never reaches that arm, so the
  label is effectively dead code for CLI-driven transmissions.
- `RPT_TIMEOUT` has no `data-state` marker in the admin flow
  diagram (`web/admin/index.html:161-167`) so when TOT fires,
  the UI's flow indicator goes dark. `stateBadgeClass` still
  renders the badge correctly but the flow diagram silently
  loses the active marker.
- There is no "connection lost" UI state. When SSE drops, the
  page keeps showing the last known state. Operators can't tell
  "nothing is happening" from "we're disconnected."
- `state_change` JSON event uses `old_state`/`new_state` without
  an `rx_` prefix. A future TX `state_change` event needs a
  disambiguating channel name or the existing field names need
  renaming.

### Principle

**The UI is a view, not a model.** Every state-ish thing the
dashboard shows must come from a single backend-authored event
or snapshot field — never inferred from combinations of lower-
level events. This is what RX state already does; TX needs to
match.

---

## 2. Target

### 2.1 RX state machine — formalised

No new states. This is documentation of what exists, so future
changes have a canonical reference. Owner: `mod_repeater`.

| State | Meaning | Entry condition |
|---|---|---|
| `IDLE` | Nothing on the air | Startup, or after `HANG_WAIT` timer expires, or after closed-repeater access denial |
| `RECEIVING` | User is transmitting to the repeater | `COR_ASSERT` (optionally after `cor_debounce`), access check passed |
| `TAIL_WAIT` | User unkeyed, repeater holds for rekey window | `COR_DROP` while in `RECEIVING` |
| `HANG_WAIT` | Tail timer expired, extended hang for quick rekey | `tail_timer` expires in `TAIL_WAIT` |
| `TIMEOUT` | TOT fired during a transmission | `tot_timer` expires in `RECEIVING` |

Transitions form the existing FSM; no changes. Documented in
`ARCHITECTURE.md` and `ARCH-STATE.md` (new, short). The
`change_state()` choke-point at `mod_repeater.c:63-79` stays the
only mutator.

### 2.2 TX state machine — new

A proper FSM owned by a new module or by the audio thread. 6
states that capture the full TX lifecycle distinctly:

| State | Meaning | Entry condition |
|---|---|---|
| `IDLE` | Nothing transmitting | Startup, all PTT refs released |
| `RELAY` | Software-relaying live user audio | `software_relay=on` AND `COR_ASSERT` |
| `DELAY` | PTT asserted for queue, waiting radio key-up | `queue_ptt=1`, `tx_delay_rem > 0` |
| `DRAIN` | Playing queued audio (TTS / WAV / tones) | `queue_ptt=1`, `tx_delay_rem<=0`, queue has samples |
| `TAIL` | Queue empty, feeding tx_tail silence | `queue_ptt=1`, `tx_tail_rem >= 0`, queue empty |
| `HOLD` | Waiting ptt_hold_ticks for hardware flush | `queue_ptt=0`, PTT still held, ring draining |

`RELAY` → `TAIL` sub-state (for software-relay drain window after
COR drop) is merged into `TAIL` with a source field
(see §2.4). Keeps the label set to 6.

**Transitions:**

```
     IDLE ──COR_ASSERT && sw_relay──▶ RELAY ──COR_DROP──▶ TAIL
       │                                                   │
       │                                           relay_drain expires
       │                                                   │
       │◀──────────────────────────────────────────────────┘
       │
       │──queue has items ────▶ DELAY ──tx_delay done──▶ DRAIN
                                                          │
                                                  queue empties
                                                          ▼
                                                        TAIL
                                                          │
                                                tx_tail done
                                                          ▼
                                                        HOLD
                                                          │
                                          ptt_hold_ticks=3 + ring drained
                                                          ▼
                                                        IDLE
```

Every transition goes through a single `tx_change_state()`
mutator that fires `KERCHEVT_TX_STATE_CHANGE` (new event type).

**Where does this live?** Two options:

- **Option A:** New `mod_tx_state` module. Subscribes to
  `COR_ASSERT/COR_DROP`, `PTT_ASSERT/PTT_DROP`,
  `QUEUE_DRAIN/QUEUE_COMPLETE`, plus audio-thread publishes of
  tx_delay/tx_tail/hold transitions via new events or atomic
  fields polled each main-tick.
- **Option B:** State machine lives in the audio thread itself,
  as a pure function over `g_audio_state` (matches the
  refactor pattern for `kerchunk_audio_tick_rx/_tx`). Fires
  events through `g_core->fire_event` at the end of each tick
  when the state changed.

**Recommend Option B.** The audio thread owns all the raw
signals that drive TX state. Adding a cross-thread module
means adding atomics or polling, plus an event-subscription
dance. A pure `kerchunk_tx_state_t` + `kerchunk_tx_state_tick()`
matches the existing `kerchunk_txactivity` pattern, gets unit
tests, and slots into the audio thread with a single `fire_event`
call on change.

### 2.3 New event: `KERCHEVT_TX_STATE_CHANGE`

```c
typedef enum {
    KERCHUNK_TX_STATE_IDLE  = 0,
    KERCHUNK_TX_STATE_RELAY,
    KERCHUNK_TX_STATE_DELAY,
    KERCHUNK_TX_STATE_DRAIN,
    KERCHUNK_TX_STATE_TAIL,
    KERCHUNK_TX_STATE_HOLD,
} kerchunk_tx_state_t;

/* Event payload */
struct { kerchunk_tx_state_t old_state, new_state; } tx_state;
```

JSON form:

```json
{
  "type": "tx_state_change",
  "old_state": "DRAIN",
  "new_state": "TAIL",
  "source": "queue",
  "ts": 1714048327123
}
```

`source` field distinguishes what drove the transition
(`"queue"`, `"relay"`, `"web_ptt"`, etc.) so the UI can colour
or annotate if desired. Dashboard doesn't have to use it; it's
informational.

### 2.4 Renaming for consistency

- `KERCHEVT_STATE_CHANGE` → `KERCHEVT_RX_STATE_CHANGE`. Clean
  rename — every internal consumer updated in the same commit.
- JSON `state_change` → `rx_state_change`. Dashboard event
  handlers updated at the same time.
- `tx_state` field in `/api/status` returns the 6-label set
  immediately. Old 4-label `tx_state` gone in the same commit
  that introduces the new enum — nothing to deprecate.

### 2.5 Snapshot model (bootstrap + reconnect)

Today: `/api/status` returns current `rx_state` and `tx_state`.
UI polls it once at load. After that, SSE takes over.

Keep that — but formalise: on SSE connect, `mod_web` emits a
single `snapshot` event containing every currently-active state
the dashboard might want to render:

```json
{
  "type": "snapshot",
  "ts": ...,
  "rx_state": "IDLE",
  "tx_state": "IDLE",
  "cor": false,
  "ptt": false,
  "caller": null,
  "emergency": false,
  "relay_drain_active": false,
  "queue_depth": 0,
  ...
}
```

The `emit_snapshot_burst` function at `mod_web.c:2528-2538`
already replays cached snapshots on connect; extend it to
include a backend-computed `snapshot` event for state. The UI's
`refreshStatus()` call goes away — everything flows through SSE,
with the first event being the full snapshot.

Also adds a "connection lost" UX: if SSE drops, the dashboard
goes into a `state = "unknown"` overlay until reconnect brings
a fresh snapshot. No more stale last-known-state after a 5-
minute disconnect.

### 2.6 UI — strictly reflective

Every `updateTxState(...)` call in
`web/admin/index.html:502-505` is deleted except for ONE handler
that listens for `tx_state_change` SSE and for the initial
`snapshot`. Same for RX. The UI becomes a mechanical translator
from event → `[data-tx=...].active` + badge text.

The `relaying` JS boolean (`admin/index.html:296, 493`) is
deleted — it's a client-side duplicate of backend state.

The SSE handlers for `cor_assert`, `cor_drop`, `ptt_drop`,
`queue_drain`, `queue_complete` no longer touch TX state.
They may still update OTHER indicators (the PTT badge, the COR
LED, the queue depth counter) — those aren't derived from
anything else, they ARE the display for that raw event. But
they stop touching the `[data-tx]` attribute.

---

## 3. Phases

Four phases on the `state-model` branch. Each compiles, tests,
deploys cleanly; the branch doesn't merge until all four are in
and validated.

### Phase 0 — snapshot contract

- Add `KERCHEVT_SNAPSHOT` event type (or use a `mod_web`-local
  SSE-only "snapshot" message — doesn't need to be a kerchevt
  if nothing else consumes it).
- `mod_web` emits a single `snapshot` JSON message on every SSE
  connect with a full state image (`rx_state`, `tx_state`, `cor`,
  `ptt`, `caller`, `emergency`, `queue_depth`, etc.).
- Dashboard reads `snapshot` on the first SSE message and deletes
  the `refreshStatus()` → `/api/status` bootstrap call entirely.
  SSE becomes the only status source.
- `/api/status` stays for `kerchunk -j status` CLI consumers —
  it's not deprecated, just no longer the UI's bootstrap path.

Risk: low. Additive on the backend, replaces one call on the UI.

### Phase 1 — TX state FSM in backend

New files:

- `include/kerchunk_tx_state.h` — `kerchunk_tx_state_t` enum
  (6 states), `kerchunk_tx_state_t` state struct, pure-function
  API.
- `src/kerchunk_tx_state.c` — `kerchunk_tx_state_tick(state,
  inputs)` that returns the new label. Pattern matches
  `kerchunk_txactivity`.
- `tests/test_tx_state.c` — ~15 unit tests: each transition edge
  plus full-cycle walks (TTS, relay, relay→queue, tail-cancel,
  HOLD skip).

Wire into the audio thread after the TX sub-tick in
`audio_thread_fn`:

- Drop the `get_tx_state()` string-diff log block at
  `main.c:1274-1287`.
- On each tick, call `kerchunk_tx_state_tick()`; if the label
  changed, fire `KERCHEVT_TX_STATE_CHANGE` and log the
  transition.
- Store the current label in an atomic
  `_Atomic kerchunk_tx_state_t g_tx_state` so `/api/status` can
  read it lock-free.

Rewrite `get_tx_state()` to return the new 6-label set directly
from `g_tx_state`. This is the ONLY label set — no old-vs-new
field, no `tx_state_v2`, no compat.

### Phase 2 — event plumbing + rename

- `include/kerchunk_events.h`:
  - Rename `KERCHEVT_STATE_CHANGE` → `KERCHEVT_RX_STATE_CHANGE`.
  - Add `KERCHEVT_TX_STATE_CHANGE`.
- `src/kerchunk_evt_json.c`:
  - Rename serialised `"type":"state_change"` →
    `"type":"rx_state_change"`.
  - Add `tx_state_change` serialiser:
    `{type,old_state,new_state,source,ts}`.
  - New `tx_state_name()` + `rx_state_name()` string tables.
- `mod_web`:
  - Add `KERCHEVT_RX_STATE_CHANGE` and `KERCHEVT_TX_STATE_CHANGE`
    to both public-safe and admin SSE filter lists.
  - `snapshot` event includes `rx_state` + `tx_state` fields
    with the authoritative labels.
- Update every internal subscriber of the old name in the same
  commit: `mod_web`, `mod_logger`, `mod_cwid`, `mod_webhook`,
  `kerchunk_socket`. Grep for `KERCHEVT_STATE_CHANGE` and update
  each.

### Phase 3 — UI cutover

Rewrite `web/admin/index.html` state handlers:

- Delete the six `updateTxState(...)` calls from the event
  handlers for `cor_assert`, `cor_drop`, `ptt_drop`,
  `queue_drain`, `queue_complete`. These handlers keep updating
  their OWN indicators (PTT badge, COR LED) but stop touching
  `[data-tx]`.
- Delete the `relaying` JS flag — it's a client-side duplicate
  of backend state.
- Add a new `tx_state_change` SSE handler that calls
  `updateTxState(new_state)`. That's the only place TX state
  gets set.
- Rename the existing `state_change` handler to
  `rx_state_change`.
- Refresh the `[data-tx]` marker set to the new 6 labels:
  `TX_IDLE`, `TX_RELAY`, `TX_DELAY`, `TX_DRAIN`, `TX_TAIL`,
  `TX_HOLD`. Delete `TX_QUEUE`. Add CSS for each. Update
  `stateBadgeClass`.
- On SSE disconnect, every state marker goes to an explicit
  "UNKNOWN" class with a "connection lost" overlay; clear on
  first `snapshot` after reconnect.
- Same cleanup for RX: verify only `rx_state_change` drives
  `[data-state]`; add RPT_TIMEOUT marker (currently missing from
  the flow diagram at `admin/index.html:161-167`).
- Public dashboard (`web/index.html`): rename SSE handler from
  `state_change` → `rx_state_change`. Optional: add a
  minimalist TX "transmitting / idle" dot (decided at Phase 3
  review, see §8 Q4).

---

## 4. Test strategy

### New unit tests

- `tests/test_tx_state.c` — pure-function tests for the TX FSM.
  Every transition edge, plus "full TX cycle" walkthroughs for:
  - Queue-driven TTS: `IDLE → DELAY → DRAIN → TAIL → HOLD → IDLE`
  - Software relay: `IDLE → RELAY → TAIL → IDLE`
  - Relay then queue: `IDLE → RELAY → TAIL → DELAY → ...`
  - Tail-cancel-on-requeue: `TAIL → DRAIN`
  - `HOLD` never entered on PA output drain (play_pending=0)

### Extended integration tests

- `tests/test_integ_repeater.c` already verifies RX state
  transitions fire `KERCHEVT_STATE_CHANGE`. Add equivalent
  assertions for `KERCHEVT_TX_STATE_CHANGE` firing at the
  expected edges.

### Manual / on-radio

- Dashboard rendering: every TX cycle shows the full
  `DELAY → DRAIN → TAIL → HOLD → IDLE` progression, visible on
  the flow diagram and badge.
- Disconnect/reconnect: SSE drop renders the dashboard as
  "unknown"; reconnect restores state within one snapshot.
- `tts say` via CLI produces a journal sequence that matches the
  dashboard transitions 1:1.

### Success criteria

Dashboard and journal emit the **same** sequence of state
strings for every TX scenario. No label synthesis in JS. On SSE
reconnect, dashboard goes to "unknown" until the snapshot
arrives, then matches the daemon's live state within one RTT.

---

## 5. Risks

1. **State model rot over time.** The FSM needs to stay in sync
   with reality. Mitigation: `tests/test_tx_state.c` covers every
   transition; if a future change breaks a transition, the test
   fails. Document both FSMs as ASCII diagrams in
   ARCHITECTURE.md so future readers see the intent.

2. **Cross-thread visibility of tx_state.** The audio thread
   owns the FSM, but `/api/status` and the snapshot emitter
   (web thread) read the current value. Mitigation: single
   atomic `_Atomic kerchunk_tx_state_t g_tx_state` that the
   audio thread writes and status endpoints read. No mutex
   needed for scalar read of an atomic enum.

3. **PTT release visibility in UI.** `TX_HOLD` is 60 ms. At
   normal SSE RTT (5-50 ms) the UI might or might not catch it
   before `HOLD → IDLE` fires. That's fine — state-change
   events still serialise correctly, and a missed intermediate
   label is cosmetic. If it's actually invisible on the UI
   for every TX, merge HOLD into TAIL (see §8 Q1).

4. **Atomic enum portability.** `_Atomic kerchunk_tx_state_t`
   works on GCC 4.9+ (C11). Confirmed against our build target;
   no action needed but noted here.

---

## 6. Definition of done

- [ ] RX and TX state machines both have named enums + string
  tables + single mutator function in the backend.
- [ ] Both fire `*_state_change` events through the event bus.
- [ ] `mod_web` forwards both to SSE. Snapshot event delivered
  on connect includes both.
- [ ] Admin dashboard uses SSE-only for state; no JS synthesis;
  six TX states visible; connection-lost overlay on disconnect.
- [ ] Public dashboard continues to show RX state; optionally
  shows TX state (single-bit "transmitting" indicator is fine).
- [ ] Unit tests: TX FSM has ~15 cases; integration tests verify
  event dispatch.
- [ ] ARCHITECTURE.md documents both FSMs with diagrams.
- [ ] Journal log sequence and dashboard sequence match for every
  TX scenario (`tts say`, parrot, courtesy tone, AI reply,
  software relay, tail-cancel-on-requeue).
- [ ] 48 hours of on-radio runtime with no mismatches between
  backend log and dashboard state.

---

## 7. Estimate

- Phase 0 (snapshot contract): 2 hours.
- Phase 1 (TX FSM + tests): 6 hours.
- Phase 2 (event plumbing + rename): 2 hours. Pure mechanical.
- Phase 3 (UI cutover): 4 hours. Deletes synthesis code; smaller
  than with back-compat shims.

**Total active work:** ~14 hours across 4 commits on the
`state-model` branch. Branch merges to main after Phase 3 passes
on-radio soak.

---

## 8. Decisions (locked before kickoff)

1. **TX_HOLD is its own state.** Operator visibility into
   "tail done, waiting for hardware flush" is worth the 60 ms
   window. Accept that fast-cycling UIs may skip rendering it;
   the journal and snapshot still carry it.

2. **Add hysteresis to `RELAY`.** When software-relay mode is
   active and COR flaps during a session (Retevis CTCSS chop),
   the `RELAY` → `TAIL` transition needs a grace window so the
   UI doesn't flicker. Implementation: when `cor_flapped_session`
   is set on the txactivity detector, hold `RELAY` for a
   configurable linger window (default 500 ms) before
   transitioning to `TAIL`. When COR comes back up within that
   window, stay in `RELAY`. Reuses the existing flap-detection
   signal from `kerchunk_txactivity_t`.

3. **Rename `TIMEOUT` → `RX_TIMEOUT`.** Collides semantically
   with TX tail timers. Done as part of Phase 2 in the same
   event-rename commit. `KERCHEVT_TIMEOUT` →
   `KERCHEVT_RX_TIMEOUT`, `RPT_TIMEOUT` → `RPT_RX_TIMEOUT` in
   mod_repeater, and the state-name string table updated.

4. **Public dashboard gets a minimalist TX indicator.** A single
   "transmitting / idle" dot alongside the existing RX badge.
   Driven by `tx_state != TX_IDLE`. Public-safe; no sensitive
   detail leaked. Part of Phase 3.

---

*End of plan. Ready to execute on branch `state-model`.*
