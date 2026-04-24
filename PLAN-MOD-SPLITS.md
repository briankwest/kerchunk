# Plan: Pure-function splits for the three large modules

**Status:** Plan — not yet implemented. Author: pair, 2026-04-24.
**Branch:** TBD; one branch per module is fine.
**Goal:** Apply the kerchunk_tx_state extraction pattern to the
three modules that carry the most decision logic. After each
split, the .so retains its current public surface (event handlers,
configure, CLI), but the testable decision functions live in their
own pure TUs with white-box unit tests — no PortAudio, no curl,
no sockets.
**No backwards compatibility:** internal-only refactor, no module
rename, no event renames.

---

## 1. Why

The three biggest modules — `mod_web` (3098 lines), `mod_ai`
(1857 lines), `mod_freeswitch` (1533 lines) — carry the bulk of
the daemon's stateful decision logic and have almost no unit
tests because their decisions sit downstream of I/O. The
`kerchunk_tx_state` extraction (PLAN-STATE-MODEL.md) showed the
shape of the fix: lift the pure decision into its own struct +
function, leave the I/O shell behind.

After this work:

- `mod_web` keeps the HTTP wiring, owns SSE pubsub, and delegates
  payload composition to `web_sse.c`.
- `mod_ai` keeps the LLM HTTP shell and delegates the
  conversation-step decision (tool-call parsing, response routing,
  speak/queue/announce) to a pure `ai_conversation_step()`.
- `mod_freeswitch` keeps the ESL TCP loop and UDP audio bridge and
  delegates the parser + call-FSM transitions to pure
  `fs_esl_parse_block()` and `fs_call_fsm_step()` functions.

The driving rule is the same as for tx_state: **a function is in
scope if it makes a decision, out of scope if it does I/O.** A
pure decision is testable.

---

## 2. Scope and non-goals

**In scope:**

- Extract pure decision functions into new `src/` TUs with
  matching `include/` headers.
- White-box unit tests for each extracted function, modelled on
  `tests/test_tx_state.c`.
- Update Makefile.am for the new TUs (linked into both kerchunkd
  and test_kerchunk).

**Out of scope:**

- Changing the public module surface (no event renames, no CLI
  changes).
- Replacing libcurl / PortAudio / mongoose with anything else.
- mod_web's monolith stays one .so — we extract logic, not
  responsibilities.

---

## 3. Phase A — mod_web → web_sse extraction

### 3.1 What sits there now

`mod_web.c:2474-2620` contains `sse_publish_impl`,
`publish_status_snapshot`, `publish_bulletin_snapshot`, and the
SSE-publish snapshot cache. The cache is keyed by event-type
string and replayed to every new SSE subscriber on first connect.

That cache logic is the most testable piece of the module and the
most failure-prone (silent stale-cache bug = browser sees wrong
data after reconnect). It has zero direct unit coverage today.

### 3.2 Target shape

New TU: `src/kerchunk_sse_cache.c` + `include/kerchunk_sse_cache.h`.

```c
typedef struct kerchunk_sse_cache kerchunk_sse_cache_t;

kerchunk_sse_cache_t *kerchunk_sse_cache_create(int max_entries);
void kerchunk_sse_cache_destroy(kerchunk_sse_cache_t *c);

/* Returns the previous payload for `type`, or NULL.
 * The returned pointer is owned by the cache; copy if you need
 * to outlive the next put. */
const char *kerchunk_sse_cache_get(kerchunk_sse_cache_t *c,
                                    const char *type);

/* Stores a copy of `payload` (NUL-terminated, JSON). Replaces any
 * prior entry under the same type. Bounded by max_entries —
 * oldest entry evicted on overflow. */
void kerchunk_sse_cache_put(kerchunk_sse_cache_t *c,
                             const char *type,
                             const char *payload);

/* Iterate all cached entries for snapshot-burst on new SSE
 * connect. callback gets type+payload pairs. */
void kerchunk_sse_cache_each(
    kerchunk_sse_cache_t *c,
    void (*cb)(const char *type, const char *payload, void *ud),
    void *ud);
```

### 3.3 Tests

`tests/test_sse_cache.c`:

- put/get round-trip
- put-overwrite same key
- eviction at max_entries
- iterator visits every live entry exactly once
- thread-safety: 4-thread put + iterate (under a mutex; not
  lock-free)

### 3.4 Risk

Low. The cache is currently a small static array in
`mod_web.c:2486-2510` with explicit bounds. Lifting it out is a
straight code move + Makefile add.

### 3.5 Stretch — payload composers

If Phase A goes well, follow up with `web_status_payload.c`
exporting:

```c
int web_status_payload_render(char *buf, size_t max,
                               const web_status_inputs_t *in);
```

with `web_status_inputs_t` carrying every field the snapshot reads
(rx state, tx state, cor, ptt, emergency + expiry, queue depth,
audio listeners, sample rate, bitrate, registration). Lets
`tests/test_web_status_payload.c` pin the wire format in the same
way `tests/test_tx_state.c` pins the FSM.

---

## 4. Phase B — mod_ai conversation step

### 4.1 What sits there now

`mod_ai.c` carries:

- libcurl LLM client + Ollama / OpenAI provider switching
- system-prompt assembly
- tool-call parsing from streamed JSON
- conversation-history ring with token budget
- in-flight guard, standby cue wiring, ASR noise-marker filter
- TTS speak vs sound_file fallback decision

The recent loop/repeat fixes (in-flight guard, content:""
normalisation, stat-before-queue) all clustered around the
conversation-step path. That code wants tests.

### 4.2 Target shape

New TU: `src/kerchunk_ai_step.c` + `include/kerchunk_ai_step.h`.

```c
typedef struct {
    /* User input */
    const char  *user_text;        /* ASR transcript */
    const char  *caller_name;
    const char  *caller_callsign;

    /* Daemon state */
    int          in_flight;        /* ai already speaking? */
    int          emergency_active;

    /* History */
    const ai_history_t *history;   /* ring + token budget */

    /* Provider config */
    ai_provider_t provider;        /* OPENAI | OLLAMA */
    const char    *model;
} ai_step_inputs_t;

typedef enum {
    AI_ACTION_SKIP,                /* drop input — guard, noise */
    AI_ACTION_SPEAK,               /* TTS the response */
    AI_ACTION_PLAY_FILE,           /* play sound_file fallback */
    AI_ACTION_TOOL_CALL,           /* dispatch tool */
} ai_action_t;

typedef struct {
    ai_action_t  action;
    const char  *speak_text;       /* if action == SPEAK */
    const char  *play_file;        /* if action == PLAY_FILE */
    const char  *tool_name;        /* if action == TOOL_CALL */
    const char  *tool_args_json;
    const char  *append_history;   /* assistant turn to append */
} ai_step_result_t;

void ai_step_compute(const ai_step_inputs_t *in,
                      const char *llm_response_json,
                      ai_step_result_t *out);
```

`ai_step_compute` is pure — given the LLM response JSON and the
input bag, it returns what the shell should do next. The shell
(mod_ai.c) keeps the libcurl call, the TTS speak, the file queue,
and the history mutation.

### 4.3 Tests

`tests/test_ai_step.c`:

- in-flight guard short-circuits (returns SKIP)
- ASR noise marker (e.g. `[BLANK_AUDIO]`) returns SKIP
- empty content + tool_calls → TOOL_CALL with parsed args
- valid content + no tool_calls → SPEAK
- emergency_active gates certain tool calls
- history append shape: assistant turn includes content:"" not null
  (the Ollama HTTP 400 fix)
- malformed JSON → SKIP (no crash)

Use libplcode-style fixture strings — actual LLM responses
captured from prior debugging sessions are perfect inputs.

### 4.4 Stretch — tool-call parser

`src/kerchunk_ai_tools.c` with:

```c
int ai_parse_tool_calls(const char *response_json,
                         ai_tool_call_t *out, int max);
```

Self-contained JSON walker with its own tests
(`tests/test_ai_tools.c`). Today this lives inline in mod_ai.c
and shares state with the response router.

### 4.5 Risk

Medium. mod_ai has more moving parts than tx_state. Plan to land
phase B in two PRs: extract the step decision first (most value),
then the tool-call parser as follow-up. Keep PLAN-MOD-AI.md (the
existing AI module plan) as the design source.

---

## 5. Phase C — mod_freeswitch parser + call FSM

### 5.1 What sits there now

`mod_freeswitch.c`:

- ESL TCP socket, recv buffer, send queue
- ESL block parser (auth/request, command/reply, event headers)
- Call FSM: IDLE → DIALING → RINGING → CONNECTED → IDLE
- UDP audio bridge with jitter buffer + VAD
- Circuit breaker on ESL reconnect
- Dial whitelist enforcement

### 5.2 Target shape

Two extractions, each with its own tests:

**`src/kerchunk_fs_esl.c`** + `include/kerchunk_fs_esl.h`:

```c
typedef enum {
    FS_ESL_BLOCK_AUTH_REQUEST,
    FS_ESL_BLOCK_COMMAND_REPLY_OK,
    FS_ESL_BLOCK_COMMAND_REPLY_ERR,
    FS_ESL_BLOCK_EVENT,
    FS_ESL_BLOCK_UNKNOWN,
} fs_esl_block_kind_t;

typedef struct {
    fs_esl_block_kind_t kind;
    char event_name[64];           /* if kind == EVENT */
    char unique_id[64];
    char body[1024];               /* trailing free-form text */
} fs_esl_block_t;

int fs_esl_parse_block(const char *raw, size_t len,
                        fs_esl_block_t *out);
```

Pure parser. Test fixtures are real ESL captures.

**`src/kerchunk_fs_call.c`** + `include/kerchunk_fs_call.h`:

```c
typedef enum {
    FS_CALL_IDLE, FS_CALL_DIALING, FS_CALL_RINGING, FS_CALL_CONNECTED,
} fs_call_state_t;

typedef struct {
    fs_call_state_t  current;
    int              esl_authed;
    /* …timer remaining, dial whitelist match, etc. */
} fs_call_inputs_t;

typedef enum {
    FS_CALL_NONE,
    FS_CALL_PLAY_RINGING_SOUND,
    FS_CALL_SETUP_AUDIO,
    FS_CALL_TEAR_DOWN,
} fs_call_action_t;

fs_call_state_t fs_call_fsm_step(
    const fs_call_inputs_t *in,
    const fs_esl_block_t   *evt,
    fs_call_action_t       *action_out);
```

Pure FSM transition with single side-effect intent
(`action_out`). Shell executes the action.

### 5.3 Tests

`tests/test_fs_esl.c`:
- auth/request frame
- command/reply +OK / -ERR
- CHANNEL_PROGRESS / CHANNEL_ANSWER / CHANNEL_HANGUP
- malformed block (missing newline) → BLOCK_UNKNOWN, no crash

`tests/test_fs_call.c`:
- IDLE + dial → DIALING
- DIALING + CHANNEL_PROGRESS → RINGING + PLAY_RINGING_SOUND
- RINGING + CHANNEL_ANSWER → CONNECTED + SETUP_AUDIO
- any state + CHANNEL_HANGUP → IDLE + TEAR_DOWN
- esl_authed=0 + dial → NONE (rejected)

### 5.4 Risk

Medium-low. Parser is straightforward. FSM has edge cases around
the CHANNEL_ANSWER + CHANNEL_PROGRESS race (we've seen ANSWER
arrive before PROGRESS in fast SIP setups). Cover that in tests.

---

## 6. Sequencing and dependencies

Phases are independent and can land in any order. Suggested
sequence for cumulative payoff:

1. **Phase A (web_sse cache)** — smallest, biggest leverage.
   Every other module that adopts snapshot publishing benefits
   from the testable cache.
2. **Phase C (fs_esl + fs_call)** — medium size, well-bounded
   scope. ESL block parsing is the most "clearly pure" piece in
   the codebase outside libplcode.
3. **Phase B (ai_step)** — most complex, biggest payoff for
   correctness. Land last so we can borrow patterns from A and C.

Each phase ships with:
- New `src/kerchunk_*.c` + `include/kerchunk_*.h`
- Makefile.am addition (linked into kerchunkd_SOURCES and
  test_kerchunk_SOURCES)
- New `tests/test_*.c` with ≥6 cases
- The original module's `.c` reduced by 100-400 lines, calling
  the new helpers
- `make check` green at every commit

---

## 7. Estimated sizes (reference)

| Phase | New TU LOC | Test LOC | Module .c reduction |
|-------|-----------:|---------:|--------------------:|
| A — web_sse_cache | ~120 | ~150 | ~80 |
| A stretch — web_status_payload | ~80 | ~100 | ~50 |
| B — ai_step | ~250 | ~250 | ~300 |
| B stretch — ai_tools parser | ~150 | ~120 | ~150 |
| C — fs_esl parser | ~150 | ~180 | ~120 |
| C — fs_call FSM | ~80 | ~150 | ~80 |
| **Total** | ~830 | ~950 | ~780 |

Net effect: roughly the same line count overall, but with ≥1700
lines now under unit-test coverage and the modules' shell logic
clearly separated from their decision logic.
