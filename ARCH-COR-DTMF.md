# COR/COS + DTMF: Current State and Re-Architecture Proposal

Draft — do not commit. Uncommitted working doc for review.

---

## 1. What we have today

### 1.1 Physical signal path

```
   Radio AF out ──────────────────→ RIM-Lite USB audio ──────→ PortAudio ──→ cap_ring
   Radio COS bit ────────────────→ CM108 HID GPIO ─────────→ /dev/rimlite  (read per tick)
```

Two channels, both going through the CM108AH:

- **Audio**: 48 kHz PCM samples over USB audio class. Arrives at PortAudio's input callback in 1024-sample frames; pushed into `g_cap_ring` (a 262144-sample SPSC ring).
- **COS**: a single HID bit in GPIO byte. The Retevis RT97L drives this bit LOW during both unsquelched voice and during DTMF tones (CTCSS detector loses lock on the DTMF 2-tone signal).

The two channels are asynchronous and their relationship is approximate at best — the COS bit tells us the radio *thinks* carrier is present, but has nothing to say about what's in the audio.

### 1.2 Software pipeline (current)

```
  ┌────────────────────────────────────────────────────────────────────────┐
  │ MAIN THREAD (20 ms tick, CLOCK_MONOTONIC)                              │
  │   • kerchunk_hid_read_cor()    → raw 0/1                               │
  │   • prev_cor, cor_drop_hold[_ticks=50 default]                         │
  │   • 1 → edge:  set_cor(1),  fire KERCHEVT_COR_ASSERT                   │
  │   • 0 → edge:  start 1000 ms hold timer  (NO event yet)                │
  │   • reassert-while-held: cancel timer   (NO event)                     │
  │   • timer expiry:  set_cor(0),  fire KERCHEVT_COR_DROP                 │
  │   • effect visible to others: kerchunk_core_set_cor(0/1) flips         │
  │     g_cor_active, which is what is_receiving() returns.                │
  └────────────────────────────────────────────────────────────────────────┘
                                  │ event bus (sync, main thread)
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────┐
  │  Twelve subscribers to COR_ASSERT/DROP:                                │
  │   mod_repeater — IDLE/RECEIVING/TAIL_WAIT/HANG_WAIT state machine      │
  │                  + another 150 ms cor_debounce_ms timer                │
  │   mod_recorder — start/stop WAV file per transmission                  │
  │   mod_dtmfcmd  — deferred *-command dispatch on DROP                   │
  │   mod_parrot   — record/playback trigger                               │
  │   mod_caller   — caller ID window                                      │
  │   mod_voicemail— user-identified greeting playback                     │
  │   mod_stats    — RX count + duration                                   │
  │   mod_cdr      — call detail record                                    │
  │   mod_asr      — transcription start/end                               │
  │   mod_poc      — PoC radio bridge                                      │
  │   mod_freeswitch — autopatch on/off                                    │
  │   mod_courtesy — courtesy tone on DROP                                 │
  └────────────────────────────────────────────────────────────────────────┘

  ┌────────────────────────────────────────────────────────────────────────┐
  │ AUDIO THREAD (20 ms tick, CLOCK_MONOTONIC)                             │
  │   • kerchunk_audio_capture() — partial: repeat-last; empty: repeat-last│
  │   • descrambler (noop by default)                                      │
  │   • KERCHEVT_AUDIO_FRAME dispatch to all audio taps                    │
  │   • [if relay_active && !g_relay_was_active] DTMF decoder reset        │
  │   • [if relay_active || g_relay_drain > 0] DTMF decoder process        │
  │     → fire KERCHEVT_DTMF_DIGIT / DTMF_END                              │
  │   • software relay (capture → playback ring + TX encoder)              │
  │   • queue drain (CWID, TTS, tones → playback ring)                     │
  │   • g_relay_drain = drain_ms on 1→0 transition                         │
  └────────────────────────────────────────────────────────────────────────┘

  ┌────────────────────────────────────────────────────────────────────────┐
  │ PORTAUDIO CALLBACK (hardware clock, ~21.3 ms / 1024 samples)           │
  │   cap_cb:    drop if paInputUnderflow; else ring_write(cap_ring)       │
  │   play_cb:   ring_read(play_ring); zero-fill on short read             │
  │   duplex_cb: same as cap+play, single-device variant                   │
  └────────────────────────────────────────────────────────────────────────┘
```

### 1.3 Filter stack (layers that look at COR)

| Layer | Where | Knob | Default | Purpose |
|---|---|---|---|---|
| L0 | raw HID bit | `[hid] cor_polarity` | active_high | polarity invert |
| L1 | main.c drop hold | `[repeater] cor_drop_hold` | 1000 ms | absorb DTMF-induced COS drops |
| L2 | mod_repeater debounce | `[repeater] cor_debounce` | 150 ms | IDLE→RECEIVING delay (for require_id mode) |
| L3 | DTMF decoder gate | `relay_active \|\| g_relay_drain>0` | — | CPU save, avoid noise-floor false hits |
| L4 | DTMF decoder hysteresis | libplcode internal | 40 ms (2 × 20 ms block) | Goertzel SNR, twist, harmonic |
| L5 | mod_dtmfcmd cor_gate | ~~[dtmf] cor_gate_ms~~ | **removed** | was squelch-transient masking |
| L6 | mod_dtmfcmd cmd buffer | `inter_digit_timeout` | 3000 ms | pattern accumulation |

### 1.4 libplcode DTMF decoder defaults

```
hits_to_begin       = 2   (40 ms continuous tone before detection = ACTIVE)
misses_to_end       = 3   (60 ms silence before detection clears)
min_off_frames      = 2   (40 ms cooldown before same digit can re-trigger)
normal_twist_x      = 16  (12 dB row/col energy ratio allowed)
reverse_twist_x     = 4   (6 dB col/row allowed)
harmonic_reject     = on  (second harmonic must be <50% of fundamental)
```

Block size = 20 ms at 48 kHz = 960 samples per Goertzel block.

---

## 2. Why it's failing

### 2.1 The radio's behavior breaks our COS signal

RT97L + Retevis in general: COS bit goes LOW during any DTMF tone (CTCSS detector loses lock on the 2-tone mix). Our whole model — "COS=1 means user is transmitting voice/data" — breaks.

L1's 1-second drop-hold is essentially a workaround for this: pretend the radio's COS is still valid and hide the transient drops from the rest of the system. It works but has two costs:
- **1 s latency** between true unkey and `KERCHEVT_COR_DROP`, which means 1 s before any `#`-deferred command executes.
- **Blurs the line between "still transmitting"  and "DTMF tone in progress"** — the DTMF decoder has to rely on its own hysteresis to distinguish.

### 2.2 DTMF audio is itself truncated

Because COS dropping during DTMF typically coincides with the radio *muting* the audio output, the actual DTMF tone we capture is only ~40-60 ms long instead of the 100-200 ms the user pressed. This is right at the decoder's 2-block (40 ms) lock-on threshold — any audio-thread timing jitter during that window tips it over.

### 2.3 Audio pipeline adds its own gaps

Even with clean radio audio, our pipeline historically injected zero-frames under three conditions:
- ring partial read (< frame_samples available)       → **fixed**, repeat-last
- ring empty read (no samples available)              → **fixed**, repeat-last
- PortAudio `paInputUnderflow`                        → **fixed**, drop

So audio-side gaps should be gone in today's build. The remaining DTMF misses are either radio-side or decoder-threshold.

### 2.4 Too many things listening to COR drops

Twelve modules subscribe to `KERCHEVT_COR_DROP`. But KERCHEVT_COR_DROP, the way we generate it today, is **a best-effort proxy for "the user has finished"**, derived from a single radio bit that's noisy. We bubble it up because our modules' notion of "transmission" is keyed on COR. That design assumes the COR bit is honest.

It isn't, on this radio.

---

## 3. What "COR drop" should actually mean

Today: "raw HID bit = 0 for more than 1 s continuously".

What each consumer actually cares about:

| Module | What it really needs |
|---|---|
| mod_repeater | "user has stopped transmitting" — so it can start tail/hang timers |
| mod_recorder | same |
| mod_dtmfcmd  | same (executes deferred `#` command when user unkeys) |
| mod_parrot   | same (stops capture, starts playback) |
| mod_caller   | same (ends caller-ID window) |
| mod_voicemail| same |
| mod_stats    | same (records duration) |
| mod_cdr      | same (logs call end) |
| mod_asr      | same (sends "stop" to ASR stream) |
| mod_courtesy | same (plays courtesy tone) |
| mod_poc      | "RF user has stopped" — to release floor for PoC-originated audio |
| mod_freeswitch | same |

Everyone wants the same logical thing: **"user has finished this transmission"**. They don't care about the physical COS bit at all — they trust us to tell them truthfully.

Right now, "user has finished" is computed as `COS bit low continuously for 1 s`. That's wrong in two directions:
- **False positives**: never happens (the hold always catches reassert).
- **False negatives**: the 1 s hold is too slow; we tell consumers 1 s after the truth.

And our DTMF pipeline is *also* trying to reason about the same "is user present" question, in parallel, from the same radio bit.

---

## 4. Proposed re-architecture

### 4.1 Core idea

**Stop equating COS bit with user-present.** The COS bit is one noisy input. The actual ground truth is a derived signal: **"someone is transmitting RF audio at us right now"**. Compute that signal from multiple inputs, then fire exactly one clean event to everyone.

Inputs available:
1. **COS bit** (from HID) — fast but lies during DTMF
2. **Audio RMS** (from the capture stream we already have) — independent of the COS bit
3. **DTMF decoder activity** (derived) — if the decoder is actively locked on, *someone* is transmitting

Derive:

```
user_transmitting  =  cos_bit
                  OR  audio_rms > noise_floor            for ≥ N ms
                  OR  dtmf_decoder_active
```

All three inputs are AM-redundant to each other. COS alone is wrong with Retevis. Audio RMS alone catches DTMF (because the tone IS audio) without needing the COS bit to behave. Decoder-active is a mid-detection sanity check.

The "stop transmitting" event fires when **all three** have been false for a short consistent window (say 300 ms, much less than the current 1 s).

### 4.2 Proposed architecture

```
  ┌──────────────────────────────────────────────────────────────────────┐
  │  TX-ACTIVITY DETECTOR  (new — one place, clearly named)              │
  │                                                                      │
  │   Inputs  — 20 ms-tick fusion:                                       │
  │     • cos_bit         (from kerchunk_hid_read_cor)                   │
  │     • audio_rms       (running RMS from AUDIO_FRAME tap)             │
  │     • dtmf_active     (libplcode .detected state)                    │
  │                                                                      │
  │   State:                                                             │
  │     • present          bool — last published edge state              │
  │     • silent_ticks     counter                                       │
  │                                                                      │
  │   Rules:                                                             │
  │     present := cos_bit                                               │
  │              OR audio_rms > g_noise_floor for ≥ 40 ms                │
  │              OR dtmf_active                                          │
  │                                                                      │
  │     on !present: start silent_ticks countdown (default 300 ms)       │
  │     on reassert during silence: zero the countdown                   │
  │     on silent_ticks expiry: fire KERCHEVT_TX_END, set present=false  │
  │                                                                      │
  │     on false→true transition: fire KERCHEVT_TX_BEGIN, present=true   │
  └──────────────────────────────────────────────────────────────────────┘
```

This is just a cleaner name for what we have — but with two real differences:

1. **It fuses three independent signals**, so it's robust to any single one lying.
2. **Consumers consume a semantic event** (`TX_BEGIN` / `TX_END`), not a mechanical one (`COR_ASSERT` / `COR_DROP`). The raw COS bit can still be exposed via a low-level API for debug, but modules stop depending on it.

Rename-pass:
- `KERCHEVT_COR_ASSERT` → `KERCHEVT_TX_BEGIN`
- `KERCHEVT_COR_DROP`   → `KERCHEVT_TX_END`
- `is_receiving()`      → same name, but returns the fused state
- Retain `KERCHEVT_COR_*` as a deprecated alias for one release so modules migrate

### 4.3 What this gets us

- **Retevis DTMF-suppresses-COS problem vanishes**. Decoder is active → `dtmf_active` is true → fused state stays `present`. No more "hold for 1 s to mask DTMF drops".
- **Low-latency unkey**. The 1 s hold becomes 300 ms of "all three signals quiet", fired as soon as the actual audio goes quiet. Commands dispatch ~700 ms faster.
- **Fewer tuning knobs on the user's plate**. `cor_drop_hold` + `cor_debounce` + `cor_gate_ms` + `relay_drain` collapse into a single `tx_end_silence` (default 300 ms) and a single `audio_noise_floor` (default RMS 30, tunable per radio).
- **Robustness to radios we don't know about**. The fused model doesn't require any specific radio behavior — as long as at least one of the three channels is honest, we're correct.

### 4.4 Implementation shape

- New file: `src/kerchunk_txactivity.c` + `include/kerchunk_txactivity.h`
- Initialized from `src/main.c`, ticked from the audio thread (so audio_rms and dtmf_active are sampled on the same clock as the audio data they came from)
- Fires `KERCHEVT_TX_BEGIN` / `TX_END` on the existing event bus
- Raw COS bit still readable for diagnostics via `kerchunk_hid_read_cor()` (unchanged)
- Old `KERCHEVT_COR_*` emitted in parallel (as aliases) during migration, deleted after all modules move

### 4.5 Knobs to expose

```ini
[txactivity]
; How long all three channels must be quiet before TX_END fires
end_silence_ms      = 300

; RMS threshold above which audio counts as "present"
audio_noise_floor   = 30    ; int16 RMS, well above sound card noise (~12-15)

; Hysteresis on audio channel so brief gaps don't flap
audio_hold_ms       = 80    ; ≈ 4 frames

; Whether to trust cos_bit at all. Set to off if radio is totally unreliable.
trust_cos_bit       = on
```

---

## 5. libplcode DTMF decoder review

While we're at it, candidate improvements in libplcode:

### 5.1 Relax `hits_to_begin`

Default is 2 blocks (40 ms). For tight-squelch Retevis audio that gives 40-60 ms of clean tone, we're right on the edge. Dropping to 1 block (20 ms) would catch shorter tones at modest false-positive cost. The decoder already does SNR + twist + harmonic rejection per block, so a single-block detection isn't blindly trusting one sample.

Option: expose as config knob in kerchunk (`[dtmf] hits_to_begin = 1`), default unchanged in libplcode.

### 5.2 Lower `min_off_frames`

Default is 2 blocks (40 ms) of silence before the same digit can re-trigger. For `*101#` where the user presses `1`, `0`, `1` in rapid succession, the second `1` needs ≥40 ms silence after the first before it can fire. If the user presses fast and/or the radio briefly holds carrier between tones, the second `1` gets rejected.

Option: drop to 1 block default. Or expose as a config knob.

### 5.3 Twist threshold may be too tight for FM-radio-filtered audio

FM radios have an audio response curve that's not flat — high frequencies (DTMF column tones) get slightly de-emphasized vs low frequencies (DTMF row tones). A perfectly-generated DTMF tone from a handheld, after going through the transmit radio's speech processing, the RF link, the receive radio's audio filter, and the sound card, can easily have >12 dB twist. Retevis specifically is known to pre-emphasize voice audio, which could exaggerate this further.

Option: loosen `normal_twist_x` from 16 (12 dB) → 8 (9 dB)? Need to actually measure on captured recordings.

### 5.4 Block size vs. radio timing

Our 20 ms block is standard but not fundamental. For radios that give ≤40 ms of clean DTMF, shifting to a 10 ms block with 3-hit hysteresis (30 ms) would catch more.

Cost: 2× the Goertzel work, more sensitive to short noise pulses.

### 5.5 Consider passing audio quality info

The decoder currently binary-decides detected/not. It could also report per-digit *confidence* (ratio of detected frames to required frames, or SNR margin). mod_dtmfcmd could use confidence to break ties when two digits fire close together.

### 5.6 Test harness proposal

We should be able to feed RX WAV files captured from real transmissions into the decoder offline (no kerchunkd, no threads) and measure hit rate. Put the harness in `libplcode/tools/decode_dtmf_wav` — eat a WAV, print detected digits with timestamps and confidences.

This is also the tool we'd use to regression-test any decoder changes.

---

## 6. Migration plan

**Phase 0 — diagnostic (1 hour)**
Build `decode_dtmf_wav` harness. Run it against the recordings in `/var/lib/kerchunk/recordings/*RX*.wav` that we know collapsed `*101#` to `*1#`. This tells us empirically: can libplcode *ever* get it right from the captured audio, or is the decoder the bottleneck?

**Phase 1 — TX-activity detector in parallel (4 hours)**
Implement `kerchunk_txactivity.c`. Wire it up. Fire `TX_BEGIN`/`TX_END` *in parallel with existing* `COR_ASSERT`/`COR_DROP`. Log both so we can correlate. No module changes yet.

**Phase 2 — migrate consumers one module at a time (1 day)**
Swap subscriptions from `COR_*` to `TX_*` in each of the twelve modules. Leave cor_drop_hold at its current 1000 ms as fallback. Delete the three redundant filter layers (cor_drop_hold, cor_debounce on open repeater, cor_gate_ms already gone).

**Phase 3 — libplcode tuning (half day)**
Based on phase 0 results: single-block hysteresis, lower min_off_frames, relaxed twist. Version bump libplcode. Add config knobs in kerchunk.

**Phase 4 — cleanup (2 hours)**
Delete the old `KERCHEVT_COR_*` events entirely (they've been aliased during phase 2-3). Consolidate docs and example config.

---

## 7. Open questions / things to decide before implementing

1. Do we really want to *remove* the COS-only behavior, or keep it as an opt-in (`[txactivity] trust_cos_bit = only`) for radios where it's honest?
2. Should the TX activity detector live in the audio thread, the main thread, or a new thread of its own? Leaning: main thread, ticked at 20 ms, pulling audio_rms/dtmf_active from atomic snapshots the audio thread writes.
3. Do we keep software_relay's `relay_active` / `g_relay_drain` concept, or fold that into the new TX activity state?
4. Should the decoder changes (#5.1-5.4) be libplcode defaults or kerchunk-specific opts? If libplcode is a general-purpose library we don't want to destabilize its other users.
5. What's the test plan? The `decode_dtmf_wav` harness is phase 0, but we'll also want an end-to-end reproducibility test — maybe a canned WAV + canned HID trace that we replay through kerchunkd.

---

## 8. Summary of the point

We've been patching a worn-out shirt. The real problem is the shirt's design: one noisy binary signal is being asked to represent a fuzzy concept ("is the user transmitting"), and twelve modules + three filter layers + one DSP are all trying to compensate for that signal's dishonesty in their own way. Replace that single-signal source-of-truth with a small sensor-fusion component, rename the events to what they actually mean, and the whole stack simplifies.

---

## 9. Phase 0 results (update)

Built + ran libplcode's existing `decode_dtmf_wav` tool against three
real RX recordings captured on the live system:

| Recording | What user pressed | Live decoder | Offline decoder | Agree? |
|---|---|---|---|---|
| Retevis attempt 1 | `*101#` | `*1#` (`*` / `1` / `#` at 0.9 / 3.8 / 6.0 s) | `*1#` (same timing) | ✓ |
| Retevis attempt 2 | `*101#` | `*0#` (`*` / `0` / `#` at 0.3 / 1.4 / 2.4 s) | `*0#` (same timing) | ✓ |
| Different radio | `*93#` | `*93#` (`*` / `9` / `3` / `#` at 1.1 / 2.4 / 3.3 / 4.0 s) | `*93#` (same timing) | ✓ |

**Offline and live decoders produce identical output for the same
audio.** libplcode is correctly identifying every DTMF tone present
in the captured audio — it is NOT the bottleneck.

**The Retevis RT97L is dropping tones from its audio output.** When
the user presses `*101#`, only one of the three middle digits (`1`,
`0`, `1`) physically makes it to the audio stream. The 2-3 second
gap visible between `*` and the next detected digit is the window
where multiple button presses happen but only one gets through.

No amount of decoder tuning or pipeline fixing can recover digits
the radio never captured. §5's libplcode tuning suggestions are
therefore **withdrawn** — they'd help marginal-SNR cases but do
nothing for missing-tone cases, which is what we actually have.

## 10. Revised recommendation

Phase 0 changes what Phase 1-3 are *worth*:

| Phase | Was | Now |
|---|---|---|
| 0 (diagnostic) | proposed | **done** — radio is the bottleneck |
| 1 (TX-activity detector) | fixes DTMF reliability | **hygiene-only** — still worth doing for latency + clarity, but won't improve `*101#` |
| 2 (migrate consumers) | needed for Phase 1 rename | **simplified** — keep existing event names, just change how they're derived |
| 3 (libplcode tuning) | possible fix | **dropped** — decoder is already correct |
| 4 (cleanup) | follows 2 | same, smaller scope |

### Simplified Phase 1

The original plan proposed renaming `KERCHEVT_COR_*` → `KERCHEVT_TX_*`
and migrating twelve module subscriptions. Given Phase 0 findings,
a smaller change captures the architectural value:

- **Keep event names as-is** (`KERCHEVT_COR_ASSERT` / `KERCHEVT_COR_DROP`).
  No module changes.
- **Change how main.c derives them** from a single HID bit + 1 s hold
  to the fused signal described in §4 (cos_bit OR audio_rms OR
  dtmf_active, with a short end-silence window).
- **Remove `cor_drop_hold`** from `[repeater]` — obsolete once
  fused, since the DTMF-induced COS drops are absorbed by
  `dtmf_active = true` rather than by a time-based mask.
- **New config** `[txactivity]` section with the fused-detector
  knobs.

This gets us the latency win (~700 ms earlier command dispatch on
unkey), the architectural clarity, and lets us retire one filter
layer — without churning every module subscriber.

### What this re-architecture does NOT fix

- DTMF reliability on radios that mute audio mid-tone. That's a
  hardware limitation. Mitigations remain:
  - Use a radio that doesn't mute DTMF (most non-Chinese HTs)
  - Restrict DTMF commands to two-tone patterns the radio can pass
  - Use web dashboard + Wyoming ASR for multi-digit commands

## 11. Phase 1 implementation details

### 11.1 Atomic snapshots the audio thread writes

Add two `atomic_int` globals exposed from the audio thread via
getters:

```c
/* Running-RMS of the most recent ~20 ms capture frame. int16 domain. */
static _Atomic int32_t g_tx_audio_rms;

/* 1 when libplcode's DTMF decoder is currently in detected state. */
static _Atomic int g_tx_dtmf_active;
```

Written per-tick by the audio thread; read per-tick by the main thread.

### 11.2 Main-thread fused detector

Replaces the current `prev_cor / cor_drop_hold` logic in `main.c`'s
main-thread loop:

```
per 20 ms tick:
  cos     = kerchunk_hid_read_cor()         (0 or 1)
  rms     = atomic_load(&g_tx_audio_rms)    (int16 RMS)
  dtmf    = atomic_load(&g_tx_dtmf_active)  (0 or 1)

  audio_present = (rms >= audio_noise_floor)  ; simple threshold
  present_now   = cos || audio_present || dtmf

  if present_now:
      silent_ticks = 0
      if !published_present:
          published_present = 1
          set_cor(1)
          fire KERCHEVT_COR_ASSERT
  else:
      silent_ticks += 1
      if published_present && silent_ticks >= end_silence_ticks:
          published_present = 0
          set_cor(0)
          fire KERCHEVT_COR_DROP
```

Three inputs OR'd — any one channel can assert presence. All three
must be false (continuously, for `end_silence_ticks`) before we
declare the transmission over.

### 11.3 Config

```ini
[txactivity]
; ms of combined-silence before KERCHEVT_COR_DROP fires.
; Replaces [repeater] cor_drop_hold.
end_silence_ms    = 300

; int16 RMS above which captured audio counts as "present".
; Typical mic-noise floor on CM108AH is ~12-15; 30 is comfortably above.
audio_noise_floor = 30

; Whether the raw HID COS bit contributes to the fused state.
; Set to 0 for radios that lie about COS (Retevis during DTMF). The
; audio_present + dtmf_active channels cover the same ground.
trust_cos_bit     = 1
```

Back-compat: if `[txactivity]` section absent, read legacy
`[repeater] cor_drop_hold` and use it for `end_silence_ms`.

### 11.4 What gets deleted

- `prev_cor`, `cor_drop_hold`, `cor_drop_hold_ms`, `cor_drop_hold_ticks` locals in main.c
- The COR: drop seen / reassert / hold timer expired log lines (replaced by the new detector's simpler logging)
- `[repeater] cor_drop_hold` config key (documented as deprecated alias)
