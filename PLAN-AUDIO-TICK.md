# Plan: Extract `kerchunk_audio_tick()` and PA-callback ring commit

**Status:** Plan — not yet implemented. Author: pair, 2026-04-24.
**Companion:** [ARCH-COR-DTMF.md](./ARCH-COR-DTMF.md) — §12 documents
the four audio-thread items this plan makes testable.

---

## 1. Motivation

Four behavioral changes we shipped in the COR/DTMF re-architecture
are **not unit-testable** today, and they all live on the audio
thread side:

1. `paInputUnderflow` drop (`cap_cb` / `duplex_cb` in
   `src/kerchunk_audio.c`) — the callback discards any buffer PA
   flags as an input under-run so zero-padded frames don't feed the
   decoder.
2. Repeat-last-sample on empty ring (`audio_thread_fn` in
   `src/main.c` line 1029) — when the capture ring is empty we
   fill the frame with the last known-good sample instead of
   zeros.
3. Queue-pause guard independent of `software_relay`
   (`audio_thread_fn` line 1198) — the fix where the queue waits
   for COR clear even on hardware-relay setups.
4. DTMF decoder reset **before** processing the first post-assert
   frame (`audio_thread_fn` line 1061) — avoids stale
   accumulators/hysteresis leaking a spurious digit into a new
   session.

All four work on-radio. None has a regression test. The blocker is
shape, not difficulty: the decisions are tangled with PortAudio
calls, scheduler syscalls, event-bus fires, and file-scope globals,
so tests can't reach them without standing up the whole daemon.

We already used the extract-to-pure-function pattern once this
cycle — `kerchunk_txactivity` — and it gained 11 unit tests that
caught two real edge cases during the extraction itself (init
clamp behavior, HID-returns-`-1` stickiness). This plan applies the
same move to the audio thread.

**Scope note — 407 lines:** `audio_thread_fn` runs from
`src/main.c:951` to `src/main.c:1357`. It reads/writes ~15
file-scope globals and calls into ~12 external subsystems (PA,
event bus, PTT refcount, queue, scrambler hooks, decoder). The
refactor is not small; this plan breaks it into five phases so no
single commit carries more than a day of on-radio validation
exposure.

## 2. Goal & non-goals

**Goal:** Move all *decisions* out of `audio_thread_fn` and the PA
callbacks into pure functions that take state + inputs and return
outputs, so tests can drive them with scripted inputs and assert on
outputs. Side-effecting I/O (PA read/write, event bus fires, PTT
refcount changes) stays in thin shells that invoke the pure
function.

**Non-goals:**

- No behavioral changes. Every subtle timing guard — RX catch-up
  burst, `tx_active` skip, `ptt_hold_ticks`, tail-cancellation —
  moves byte-for-byte into the new shape.
- No performance work. If a test forces a hot-path restructure, we
  back out.
- No change to the module-facing API (`kerchunk_core_t *` vtable,
  event bus types). Module authors see nothing.
- No change to what config keys mean or how they're parsed.
- No new features. This is purely a shape change.

## 3. Target shape

### 3.1 Audio-thread tick

```c
/* include/kerchunk_audio_tick.h */

typedef struct {
    /* Config (written at init, read-only at tick time) */
    int sample_rate;
    int frame_samples;
    int software_relay;
    int tx_delay_samples;
    int tx_tail_samples;
    int relay_drain_samples;
    int ptt_hold_ticks_max;   /* 3 today */

    /* Decoder + presence detector — owned here, not global */
    plcode_dtmf_dec_t     *dtmf_dec;       /* alloc'd outside, borrowed */
    kerchunk_txactivity_t  tx;             /* already a pure state struct */

    /* Edge-detection state (was file-scope in main.c) */
    int prev_dtmf;
    int relay_was_active;
    int relay_drain;
    int queue_ptt;
    int queue_fired_drain;
    uint64_t queue_drain_start_us;
    int tx_delay_rem;
    int tx_tail_rem;
    int ptt_hold_ticks;
} kerchunk_audio_state_t;

typedef struct {
    const int16_t *captured;     /* what came off the capture ring */
    int            nread;        /* samples in captured */
    int            cap_underflow;/* did PA flag paInputUnderflow? */
    int            cos_raw;      /* -1 if HID unreadable */
    int            relay_active; /* kerchunk_core_get()->is_receiving() */
    int            ptt_held;     /* kerchunk_core_get_ptt() */
    int            queue_depth;  /* kerchunk_queue_depth() */
    int            queue_draining;/* kerchunk_queue_is_draining() */
    size_t         play_writable;/* kerchunk_audio_playback_writable() */
    size_t         play_pending; /* kerchunk_audio_playback_pending() */
    uint64_t       now_us;
} kerchunk_audio_input_t;

typedef enum {
    KERCHUNK_AUDIO_OUT_NONE = 0,
    KERCHUNK_AUDIO_OUT_RELAY,       /* write to playback ring + TX tap */
    KERCHUNK_AUDIO_OUT_QUEUE_AUDIO, /* write to playback ring + TX tap */
    KERCHUNK_AUDIO_OUT_SILENCE,     /* tx_delay or tx_tail silence */
} kerchunk_audio_out_kind_t;

typedef struct {
    /* RX-side events (audio frame taps, DTMF events) */
    const int16_t *rx_frame;     /* post-descramble; NULL if no fire */
    int            rx_frame_n;
    char           dtmf_digit;   /* 0 if none */
    int            dtmf_end;     /* 1 on DTMF_END */

    /* TX-side playback intent */
    kerchunk_audio_out_kind_t  out_kind;
    int16_t       *out_samples;  /* points into a caller-provided scratch buffer */
    int            out_n;

    /* PTT refcount intent (shell applies via core vtable) */
    int ptt_request_queue;  /* 1 = assert our ref */
    int ptt_release_queue;  /* 1 = release our ref */

    /* Event-bus fires (shell calls kerchevt_fire) */
    kerchevt_t events[8];
    int        event_count;
} kerchunk_audio_output_t;

void kerchunk_audio_tick(kerchunk_audio_state_t *s,
                         const kerchunk_audio_input_t *in,
                         int16_t *scratch_playback,
                         size_t  scratch_playback_cap,
                         kerchunk_audio_output_t *out);
```

The shell (`audio_thread_fn` in `main.c`) becomes:

```c
while (g_running) {
    advance_deadline();
    fill_input_from_hardware(&in);   /* PA + HID + core state reads */
    kerchunk_audio_tick(&state, &in, scratch, CAP, &out);
    apply_output_to_hardware(&out);  /* PA writes + tap dispatch +
                                      * PTT refcount + event fires */
    clock_nanosleep(..., &deadline, NULL);
}
```

### 3.2 PA-callback ring commit

`cap_cb`/`duplex_cb` in `kerchunk_audio.c` already have minimal
logic: "if underflow, drop the frame; else commit to ring". Extract
that into:

```c
/* src/kerchunk_audio_ring.c */
void kerchunk_audio_ring_commit(ring_t *ring,
                                const int16_t *samples,
                                size_t n,
                                int underflow);
```

The callback becomes a four-line wrapper. This is a much smaller
seam than the tick function, but it's the only way to get
`paInputUnderflow` coverage without dragging PortAudio into a test.

### 3.3 What stays in `main.c`

- `audio_thread_fn` loop, deadline math, `clock_nanosleep` call.
- `fill_input_from_hardware()` / `apply_output_to_hardware()`
  helpers — pure I/O, no decisions.
- RX catch-up burst logic — this is pacing, not tick behavior.
  (We could pull it into the tick later, but it doesn't gate any
  of the four uncovered items, so leave it alone.)
- Scrambler hook calls — `kerchunk_core_get_rx_scrambler()` etc.
  are dynamic vtable lookups. We call them around
  `kerchunk_audio_tick`: shell descrambles RX *before* tick, tick
  returns `out_samples`, shell scrambles TX *after* tick.

## 4. Phased execution

Every phase compiles, passes `make check`, and can be deployed
to the Pi for on-radio validation before moving on. **Do not stack
phases in a single deploy** — each is an independent bisect point.

### Phase 0 — prep (no behavioral change, low risk)

**Branch:** `audio-tick-prep`.

- [ ] Create `include/kerchunk_audio_tick.h` with the types in
  §3.1 but no implementation yet.
- [ ] Move all file-scope audio-thread globals in `main.c`
  (`g_software_relay`, `g_relay_drain`, `g_relay_was_active`,
  `g_queue_ptt`, `g_queue_fired_drain`, `g_queue_drain_start_us`,
  `g_tx_delay_rem`, `g_tx_tail_rem`, `g_ptt_hold_ticks`,
  `g_tx_delay_ms`, `g_tx_tail_ms`, `g_relay_drain_ms`) into a
  single `static kerchunk_audio_state_t g_audio_state;` and update
  all references. `g_tx_dtmf_active` stays atomic — it's crossed
  by the decoder publishing to the main thread.
- [ ] Confirm every external touch (PA calls, core vtable, event
  bus) is on a named boundary we can mock. Write these down in a
  short comment block at the top of `audio_thread_fn` as the
  "dependency budget."
- [ ] **Deploy + on-radio smoke:** key up a few times, run
  `*101#`, confirm nothing regressed.

**Commit message:** `audio: consolidate audio-thread state into a
struct (prep for kerchunk_audio_tick extraction)`

### Phase 1 — extract the PA-ring commit (2 hours + 1 deploy)

**Branch:** `audio-ring-commit`.

- [ ] Add `src/kerchunk_audio_ring.c` + header with
  `kerchunk_audio_ring_commit()`.
- [ ] Rewrite `cap_cb` and `duplex_cb` to call it.
- [ ] Add `tests/test_audio_ring.c` with cases: commit without
  underflow appends exactly n samples; commit with
  `underflow=1` appends zero samples; commit of N > ring free
  space wraps correctly; commit of `underflow=1 && n=0` is a
  no-op.
- [ ] Wire into `test_main.c`.
- [ ] **Deploy + on-radio smoke.** This changes zero behavior —
  the check is that it *really* is zero.

**Commit:** `audio: extract ring-commit from PA callbacks; add
tests for underflow drop path`

**At this point item #1 of the four is covered.**

### Phase 2 — extract the RX sub-tick (DTMF + relay decisions)

**Branch:** `audio-tick-rx`.

The RX path in `audio_thread_fn` is lines 1033–1172 (descramble,
DTMF decode + events, relay decisions). Lift into:

```c
void kerchunk_audio_tick_rx(kerchunk_audio_state_t *s,
                            const kerchunk_audio_input_t *in,
                            kerchunk_audio_output_t *out);
```

Decoder reset-on-edge, `prev_dtmf` edge detection, relay drain
countdown, silence-floor early-stop — all of it moves. The shell
still does the descramble-before and scramble-after.

- [ ] Add `src/kerchunk_audio_tick.c` with `_tick_rx` only.
- [ ] Shell in `main.c` fills a minimal `kerchunk_audio_input_t`,
  calls `_tick_rx`, then publishes the events and writes the relay
  bytes it returned.
- [ ] Add `tests/test_audio_tick_rx.c` covering:
  - decoder reset on COR-assert edge (the 3rd uncovered item)
  - DTMF_DIGIT fires once per tone, DTMF_END on trailing edge
  - relay drain countdown, silence-floor early-stop
  - relay disabled when `software_relay=0`
  - relay suppressed when `g_queue_ptt=1` (feedback guard)
- [ ] **Deploy + on-radio full-run:** 30+ min with parrot,
  real-life DTMF sequences, CW ID triggers. Watch for any
  regression in audio quality, drops, or stuck PTT.

**Commit:** `audio: extract RX sub-tick (DTMF + relay); cover
decoder-reset-before-process`

**At this point item #4 is covered.**

### Phase 3 — extract the TX sub-tick (queue drain + tx_delay/tail)

**Branch:** `audio-tick-tx`.

TX path is lines 1175–1349 (queue pause guard, PTT assert, tx_delay
silence, queue drain, tx_tail, PTT release with `ptt_hold_ticks`).
Lift into:

```c
void kerchunk_audio_tick_tx(kerchunk_audio_state_t *s,
                            const kerchunk_audio_input_t *in,
                            int16_t *scratch,
                            size_t  scratch_cap,
                            kerchunk_audio_output_t *out);
```

Tricky parts to preserve exactly:
- The `continue` after tail-cancellation on courtesy-tone requeue —
  in the new shape this becomes `out->out_kind = NONE` with a flag
  that tells the shell to re-run the next tick as-if. Needs
  care; write it up in the commit message.
- `ptt_release_queue = 1` only when tail done AND ring drained
  AND `ptt_hold_ticks` ≥ 3.
- `ptt_already_held` check before our own `request_ptt` — the
  shell passes `ptt_held` in the input, but the check must be
  pre-assert. The tick sets `ptt_request_queue=1` *and* a
  "skip_tx_delay" bit the shell honors; or we pass a pre-assert
  snapshot. Either works; pick one in the commit.

- [ ] Add `_tick_tx` to `src/kerchunk_audio_tick.c`.
- [ ] Rewrite the shell.
- [ ] Add `tests/test_audio_tick_tx.c` covering:
  - queue pause when `relay_active=1`, `queue_ptt=0` — the
    software_relay-independent guard (item #3)
  - queue resumes after COR clears
  - tx_delay countdown feeds silence (no audio) for N samples
  - queue drain fires KERCHEVT_QUEUE_DRAIN once
  - tx_tail silence feed after empty queue
  - tail cancellation on courtesy-tone requeue
  - PTT released only after `ptt_hold_ticks == 3` AND
    `play_pending == 0`
- [ ] **Deploy + on-radio full-run.** This is the highest-risk
  phase — PTT release and queue draining drive every single TX.

**Commit:** `audio: extract TX sub-tick; cover software-relay-independent
queue-pause guard`

**At this point item #3 is covered.**

### Phase 4 — extract repeat-last and fold the two sub-ticks

**Branch:** `audio-tick-final`.

- [ ] Factor `kerchunk_audio_capture_repeat_last()` in
  `kerchunk_audio.c` so the last-sample state is reachable as
  pure function state (today it's a file-scope `static int16_t
  g_last_sample`; move it into a small struct and expose a
  `*_reset()` for tests).
- [ ] Add `tests/test_audio_capture.c` covering:
  - repeat-last fills N samples all equal to the last-commit
    value
  - after a fresh `_reset()`, repeat-last fills silence (0)
  - repeat-last preserves value across multiple calls
- [ ] Fold `_tick_rx` + `_tick_tx` into a single
  `kerchunk_audio_tick()` that dispatches internally. This is
  optional — we could leave them split. Decide during review.
- [ ] **Deploy + on-radio full-run.**

**Commit:** `audio: cover repeat-last-sample path; unify audio tick
entrypoint`

**At this point item #2 is covered — all four are done.**

## 5. Test strategy

### What we add
- `tests/test_audio_ring.c` — Phase 1, ~4 cases.
- `tests/test_audio_tick_rx.c` — Phase 2, ~6 cases.
- `tests/test_audio_tick_tx.c` — Phase 3, ~8 cases.
- `tests/test_audio_capture.c` — Phase 4, ~3 cases.

Expected additions: ~20 new test cases, bringing the suite from
283 to ~303. All driven by scripted `kerchunk_audio_input_t`
sequences; no PortAudio, no clock, no threads.

### What stays manual
- PortAudio actually calling our callbacks on real hardware.
- The audio thread actually meeting its 20ms deadline under load.
- Scrambler hooks in the vtable actually being called in the right
  order relative to RX/TX.
- The `clock_nanosleep` drift-free pacing.

These are integration concerns and each phase's on-radio smoke
test is how we catch them.

### Mock strategy
Use the existing test-stub pattern (`tests/test_stubs.c`): the
tick function takes state + inputs, returns outputs. No vtable,
no event bus in the tests themselves — we *inspect* the output's
`events[]` array instead of firing them. Queue/PTT state is
synthesized into the `kerchunk_audio_input_t`.

## 6. Risk & rollback

**Risk ranking (highest first):**

1. **Phase 3 (TX sub-tick).** Getting tail-cancellation or PTT
   release wrong means stuck PTT in production. Every commit in
   this phase gets a 30+ min on-radio soak before merge.
2. **Phase 2 (RX sub-tick).** Wrong decoder-reset edge means DTMF
   regresses to pre-fix behavior. Test with real radio
   sequences, not just synthetic input.
3. **Phase 0 (prep).** Moving globals into a struct is boring but
   one missed reference = crash. Compiler helps.
4. **Phases 1 & 4.** Small seams, contained blast radius.

**Rollback:** each phase is its own branch, merged fast-forward to
`main` only after on-radio validation. If a phase regresses, we
revert that phase's merge commit and go back. We do *not* pile
phases up on a long-lived branch.

**Stop condition:** if any phase takes > 2× its estimate or
surfaces a behavioral edge case we can't cleanly port, stop,
write it up, and decide whether to proceed. The existing code
works; there is no deadline on this work.

## 7. Definition of done

- [ ] All four items in ARCH-COR-DTMF.md §12 "Not yet covered"
  have at least one unit test each.
- [ ] `audio_thread_fn` in `main.c` is < 100 lines (down from
  407) and contains no `if` on behavioral state — only on I/O
  readiness.
- [ ] All file-scope audio-thread globals live in
  `kerchunk_audio_state_t`, except `g_tx_dtmf_active` (atomic,
  cross-thread by design).
- [ ] `make check` passes; suite grows to ~303 cases.
- [ ] 48 hours of on-radio runtime across the four phases with no
  new regression reports.
- [ ] §12 of ARCH-COR-DTMF.md updated to reflect new coverage.

## 8. Estimate

- Phase 0: 1 hour + deploy.
- Phase 1: 2 hours + deploy.
- Phase 2: 4 hours + deploy + 30 min soak.
- Phase 3: 6 hours + deploy + 60 min soak. (Hardest.)
- Phase 4: 2 hours + deploy.

**Total active work:** ~15 hours across five commits, plus
deploy-and-soak windows between each. Calendar time depends on how
aggressive we want to be about soaking.

## 9. Open questions

1. Does the RX catch-up burst belong inside `_tick_rx`, or stays
   in the shell as a pacing concern? Leaving it in the shell means
   the tick only ever sees one frame at a time, which is simpler
   to test. Decide at Phase 2.
2. Fold `_tick_rx` + `_tick_tx` into one `_tick` at Phase 4, or
   keep them split forever? Keeping them split lets tests focus;
   folding them makes the shell shorter. Decide at Phase 4 review.
3. Should we extract HID COS read at the same time? It's already
   a simple read-from-descriptor call; extraction probably doesn't
   pay back. **Proposal:** leave it.

---

*End of plan. Approve, revise, or shelve.*
