# mod_ai — AI Voice Assistant Framework for Kerchunk

## Overview

mod_ai orchestrates ASR → LLM → TTS to provide a voice-activated AI assistant
on the repeater. Users key up, speak a command or question, and the repeater
responds with synthesized speech. The LLM uses structured tool calling — not
prompt-and-pray — to interact with kerchunk's systems.

## Architecture

```
RF Inbound (COR)
     │
     ▼
mod_asr (Wyoming ASR) ──► transcript text
     │
     ▼
mod_ai (orchestrator)
     │
     ├── 1. Intent detection: is this directed at the AI?
     │      - Wake phrase: "kerchunk", "radio", "hey repeater", configurable
     │      - DTMF trigger: *99# activates AI for next transmission
     │      - Always-on mode (optional, for dedicated AI channels)
     │
     ├── 2. LLM inference (llama.cpp via HTTP API)
     │      - System prompt with available tools
     │      - User message = transcript
     │      - Tool calls returned as structured JSON
     │      - Multi-turn: tool results fed back, LLM generates final response
     │
     ├── 3. Tool execution
     │      - Tools map to kerchunk CLI commands + external APIs
     │      - Results formatted and fed back to LLM
     │
     └── 4. Response via TTS
            - LLM's final text response → g_core->tts_speak()
            - Queued to audio, plays over RF
```

## State Machine

Radio is turn-based — user keys up, talks, unkeys, we respond. This maps
to a clean state machine with 7 states:

```
                    ┌─────────────────────────────────────────┐
                    │                                         │
                    ▼                                         │
              ┌──────────┐                                    │
              │          │  COR assert                        │
              │   IDLE   │◄────── conversation timeout ───────┤
              │          │                                    │
              └────┬─────┘                                    │
                   │                                          │
                   │ COR assert                               │
                   ▼                                          │
              ┌──────────┐                                    │
              │          │                                    │
              │ LISTENING│  (mod_asr captures audio)          │
              │          │                                    │
              └────┬─────┘                                    │
                   │                                          │
                   │ COR drop → transcript arrives            │
                   ▼                                          │
              ┌──────────┐                                    │
              │  DETECT  │  Does transcript have wake phrase? │
              │          │  Is caller in active conversation? │
              │          │  Was DTMF *99# armed?              │
              └────┬─────┘                                    │
                   │                                          │
              no ──┤──► back to IDLE (ignore, not for us)     │
                   │                                          │
              yes  │ strip wake phrase from text              │
                   ▼                                          │
              ┌──────────┐                                    │
              │          │  POST /v1/chat/completions         │
              │ LLM_CALL │  with tools + message history      │
              │          │                                    │
              └────┬─────┘                                    │
                   │                                          │
                   ├── finish_reason = "stop"                 │
                   │   └──► go to SPEAKING                    │
                   │                                          │
                   ├── finish_reason = "tool_calls"           │
                   │   └──► go to TOOL_EXEC                   │
                   │                                          │
                   ▼                                          │
              ┌──────────┐                                    │
              │          │  Execute each tool call            │
              │TOOL_EXEC │  Collect results as JSON           │
              │          │  Append to message history         │
              └────┬─────┘                                    │
                   │                                          │
                   │ tool_rounds < max_tool_rounds?           │
                   │ yes → back to LLM_CALL with results      │
                   │ no  → force final response               │
                   │                                          │
                   ▼                                          │
              ┌──────────┐                                    │
              │          │  tts_speak(response_text)          │
              │ SPEAKING │  Audio queued → PTT → RF           │
              │          │                                    │
              └────┬─────┘                                    │
                   │                                          │
                   │ TTS complete                             │
                   ▼                                          │
              ┌──────────┐                                    │
              │          │  Waiting for follow-up             │
              │  READY   │  Same caller can continue          │
              │          │  conversation without wake phrase  │
              └────┬─────┘                                    │
                   │                                          │
                   ├── COR assert from same caller → LISTENING│
                   │   (no wake phrase needed, conversation   │
                   │    context preserved)                    │
                   │                                          │
                   ├── COR assert from different caller       │
                   │   → LISTENING (new conversation,         │
                   │     wake phrase required)                │
                   │                                          │
                   └── timeout ───────────────────────────────┘
                       conversation expires, back to IDLE
```

### State descriptions

**IDLE** — No active AI conversation. Every transmission is checked for a
wake phrase. Most transmissions are ignored (normal repeater traffic).

**LISTENING** — COR is active, mod_asr is capturing audio. mod_ai is just
waiting for the transcript. Nothing happens here — the audio thread and
ASR handle this.

**DETECT** — Transcript arrived. Three questions:
1. Does it start with the wake phrase? → strip it, proceed
2. Is this caller already in an active conversation? → proceed (no wake
   phrase needed for follow-ups)
3. Was `*99#` DTMF armed for this caller? → proceed, disarm
4. None of the above → ignore, back to IDLE

**LLM_CALL** — HTTP POST to llama.cpp with the full message history and
tool definitions. This is blocking on the worker thread (not the audio
thread). Takes 1-3 seconds on CPU.

**TOOL_EXEC** — LLM said "I need to call get_weather()". We execute the
tool, get the result JSON, append it to the message history, and go back
to LLM_CALL. This can loop up to `max_tool_rounds` times (default 3)
to prevent infinite loops.

**SPEAKING** — LLM produced a final text response. We call `tts_speak()`
and wait for the audio to play. The response is in the conversation
history now.

**READY** — Response has been spoken. We're waiting for a potential
follow-up from the same caller. If they key up again within the
conversation timeout (default 5 minutes), they don't need the wake
phrase — the conversation continues. If a different caller keys up,
they need their own wake phrase. If nobody talks to us for 5 minutes,
we drop back to IDLE.

### Tool call round-trip example

```
User: "kerchunk what's the weather and time?"

Round 1:
  → LLM receives: [system, user:"what's the weather and time?"]
  ← LLM returns: tool_calls: [get_weather(), get_time()]
  
  Execute get_weather() → {"temp":67,"condition":"sunny","wind":"S 7mph"}
  Execute get_time()    → {"time":"11:42 AM","date":"Monday April 7"}

Round 2:
  → LLM receives: [system, user, assistant(tool_calls), tool(weather), tool(time)]
  ← LLM returns: "It's 11:42 AM on Monday. Currently sunny and 67 degrees
                   with light south winds."

  → tts_speak("It's 11:42 AM on Monday...")
```

### Multi-turn conversation example

```
User: "kerchunk what's the temperature?"
  AI: "It's currently 67 degrees and sunny."     [conversation created]

User: "and the wind?"                            [same caller, no wake phrase]
  AI: "Wind is from the south at 7 miles per hour." [conversation continues]

User: "send a page to brian saying test"         [still same conversation]
  AI: [calls send_page tool]
  AI: "Page sent to Brian."

... 5 minutes of silence ...                     [conversation expires]

User: "what time is it"                          [no wake phrase → ignored]
User: "kerchunk what time is it"                 [wake phrase → new conversation]
  AI: "The time is 12:15 PM Central."
```

### Threading model

All AI work happens on a dedicated worker thread. The audio thread and
main loop are never blocked:

```
Audio thread (20ms tick):
  → captures frames, runs decoders, dispatches taps
  → never touches mod_ai

Main loop (20ms tick):
  → COR polling, timers, events
  → fires KERCHEVT_ANNOUNCEMENT when ASR transcript ready

AI worker thread:
  → receives transcript via queue (same pattern as mod_tts)
  → runs wake detection, HTTP calls, tool execution
  → calls tts_speak() when done (non-blocking, goes to TTS worker)
```

## Tool Calling Protocol

We use llama.cpp's OpenAI-compatible `/v1/chat/completions` endpoint with
`tool_choice: "auto"` and structured tool definitions. The LLM decides
which tools to call based on the user's request.

### Request format (to llama.cpp):

```json
{
  "model": "local",
  "messages": [
    {
      "role": "system",
      "content": "You are a radio repeater assistant. Respond concisely for voice output. Use tools to get real-time data. Keep responses under 3 sentences."
    },
    {
      "role": "user",
      "content": "what's the weather like?"
    }
  ],
  "tools": [
    {
      "type": "function",
      "function": {
        "name": "get_weather",
        "description": "Get current weather conditions at the repeater site",
        "parameters": { "type": "object", "properties": {} }
      }
    },
    {
      "type": "function",
      "function": {
        "name": "get_time",
        "description": "Get current date and time",
        "parameters": { "type": "object", "properties": {} }
      }
    },
    ...
  ],
  "tool_choice": "auto",
  "max_tokens": 200,
  "temperature": 0.3
}
```

### Response with tool call:

```json
{
  "choices": [{
    "message": {
      "role": "assistant",
      "content": null,
      "tool_calls": [{
        "id": "call_1",
        "type": "function",
        "function": {
          "name": "get_weather",
          "arguments": "{}"
        }
      }]
    },
    "finish_reason": "tool_calls"
  }]
}
```

### Tool result fed back:

```json
{
  "messages": [
    ... (previous messages) ...,
    {
      "role": "assistant",
      "content": null,
      "tool_calls": [{"id": "call_1", "function": {"name": "get_weather", "arguments": "{}"}}]
    },
    {
      "role": "tool",
      "tool_call_id": "call_1",
      "content": "{\"temperature\":67,\"condition\":\"sunny\",\"wind\":\"S at 7mph\",\"humidity\":45}"
    }
  ]
}
```

### Final response:

```json
{
  "choices": [{
    "message": {
      "role": "assistant",
      "content": "It's currently sunny and 67 degrees with light winds from the south at 7 miles per hour."
    },
    "finish_reason": "stop"
  }]
}
```

## Tool Registry

Tools are registered at module load time. Each tool maps to either a
kerchunk internal command or an external data source.

### Built-in tools (from kerchunk modules):

| Tool | Source | Description |
|------|--------|-------------|
| `get_weather` | mod_weather cache | Current conditions (temp, wind, humidity, condition) |
| `get_forecast` | mod_weather cache | Multi-day forecast |
| `get_time` | system clock | Current date, time, day of week |
| `get_repeater_status` | CLI `status` | RX/TX state, uptime, COR active |
| `get_stats` | mod_stats RRD | Activity stats: RX count, TX count, top users |
| `get_nws_alerts` | mod_nws cache | Active weather alerts |
| `get_user_info` | user DB | Caller info (if identified) |
| `send_page` | mod_pocsag/flex | Send a pager message |
| `set_emergency` | mod_emergency | Activate/deactivate emergency mode |
| `get_asr_history` | mod_asr | Recent transcripts |
| `play_tone` | mod_tones | Generate a tone burst |
| `run_command` | CLI dispatch | Execute any kerchunk CLI command (admin only) |

### Tool definition struct:

```c
typedef struct {
    const char *name;           /* "get_weather" */
    const char *description;    /* "Get current weather conditions" */
    const char *parameters_json;/* JSON Schema for parameters */
    /* Callback: receives parsed args JSON, returns result JSON string */
    char *(*execute)(const char *args_json, void *userdata);
    void  *userdata;
    int    admin_only;          /* 1 = requires authenticated caller */
} ai_tool_t;

#define MAX_AI_TOOLS 32
```

### Tool execution flow:

```c
/* 1. LLM returns tool_calls array */
/* 2. For each tool call: */
for (int i = 0; i < num_tool_calls; i++) {
    ai_tool_t *tool = find_tool(tool_calls[i].name);
    if (!tool) { /* unknown tool — feed error back */ continue; }
    if (tool->admin_only && !caller_is_admin) {
        /* feed "access denied" back to LLM */
        continue;
    }
    char *result = tool->execute(tool_calls[i].arguments, tool->userdata);
    /* Add tool result to messages array */
    add_tool_result(messages, tool_calls[i].id, result);
    free(result);
}
/* 3. Send updated messages back to LLM for final response */
```

## Multi-Turn Conversation

The AI supports multi-turn within a single COR session (user keys up
multiple times). Conversation state is maintained per-caller (by caller ID)
with a configurable timeout.

```c
typedef struct {
    int      caller_id;
    time_t   last_active;
    /* Rolling message history (last N messages) */
    char    *messages_json;     /* JSON array of messages */
    int      message_count;
} ai_conversation_t;

#define MAX_CONVERSATIONS 8
#define MAX_MESSAGES_PER_CONV 20
#define CONVERSATION_TIMEOUT_S 300  /* 5 min idle = reset */
```

## Wake Phrase Detection

Not every transmission should trigger the AI. Detection methods:

1. **Wake phrase** — First word(s) of transcript match: "kerchunk", "hey radio",
   "computer", etc. Configurable via `[ai] wake_phrase`.

2. **DTMF activation** — `*99#` arms the AI for the next COR cycle from that
   caller. After one response, returns to normal mode.

3. **Always-on** — Every transmission goes through the AI. For dedicated AI
   channels or testing.

4. **Keyword in transcript** — Scan for "question" or "?" in the ASR output.

```c
typedef enum {
    AI_TRIGGER_WAKE_PHRASE,
    AI_TRIGGER_DTMF,
    AI_TRIGGER_ALWAYS,
} ai_trigger_mode_t;
```

## llama.cpp Integration

mod_ai talks to llama.cpp via its OpenAI-compatible HTTP API (`/v1/chat/completions`).
This keeps the LLM process separate from kerchunk — different process, can run on
different hardware, easy to swap models.

### Server setup:

```bash
# Start llama.cpp server
llama-server \
  --model /path/to/model.gguf \
  --host 127.0.0.1 \
  --port 8081 \
  --n-predict 200 \
  --ctx-size 2048 \
  --threads 4 \
  --chat-template chatml
```

### Config:

```ini
[ai]
enabled = on
llm_host = 127.0.0.1
llm_port = 8081
llm_model = local              ; model name for API (llama.cpp ignores this)
max_tokens = 200               ; keep responses short for voice
temperature = 0.3              ; low temperature for factual responses
system_prompt = You are a radio repeater assistant for station WRDP519. \
                Respond concisely — your output will be spoken over radio. \
                Keep responses under 3 sentences. Use tools for real-time data.
trigger = wake_phrase           ; wake_phrase | dtmf | always
wake_phrase = kerchunk          ; wake phrase to listen for
dtmf_code = 99                 ; DTMF code for *99# activation
conversation_timeout = 5m      ; reset conversation after idle
max_tool_rounds = 3            ; max tool call → response rounds per request
```

## Module Structure (mod_ai.c)

```
mod_ai.c
├── Tool registry (register, lookup, execute)
├── LLM client (HTTP POST to llama.cpp, parse JSON response)
├── Conversation manager (per-caller state, timeout cleanup)
├── Wake phrase detector (string prefix match on transcript)
├── Event handlers:
│   ├── KERCHEVT_ANNOUNCEMENT (source="asr") — trigger on transcript
│   ├── KERCHEVT_CALLER_IDENTIFIED — track caller for conversation
│   ├── KERCHEVT_CALLER_CLEARED — cleanup
│   └── KERCHEVT_CUSTOM+N — DTMF activation from mod_dtmfcmd
├── Orchestration loop:
│   1. Receive transcript
│   2. Check wake/trigger
│   3. Build messages (system + conversation history + user)
│   4. POST to llama.cpp with tools
│   5. If tool_calls: execute tools, feed results back, goto 4
│   6. Speak final response via tts_speak()
└── CLI: ai status, ai history, ai tools, ai ask <text>
```

## Dependencies

| Dependency | Purpose | Required? |
|-----------|---------|-----------|
| libcurl | HTTP client for llama.cpp API | Yes |
| cJSON (bundled in kerchunk) | Parse LLM JSON responses | Yes (already available) |
| mod_asr | Provides transcripts | Yes (must be loaded) |
| mod_tts | Speaks responses | Yes (must be loaded) |
| llama.cpp server | LLM inference | External process |

## CLI Commands

```
ai                    Show AI status (enabled, LLM connection, trigger mode)
ai tools              List registered tools and their descriptions
ai history            Show recent AI interactions (query → response)
ai ask <text>         Manually trigger AI with text input (bypasses wake phrase)
ai reset              Clear all conversation state
```

## Event Flow (complete)

```
User keys up: "kerchunk what time is it"
  │
  ├── mod_asr captures audio, sends to Wyoming ASR
  ├── ASR returns: "kerchunk what time is it"
  ├── mod_asr fires KERCHEVT_ANNOUNCEMENT source="asr"
  │
  ├── mod_ai receives announcement
  ├── Wake phrase "kerchunk" detected, strip it: "what time is it"
  ├── Look up caller conversation (or create new)
  ├── Build messages:
  │     [system prompt]
  │     [user: "what time is it"]
  │
  ├── POST to llama.cpp /v1/chat/completions with tools
  ├── LLM returns: tool_calls: [get_time()]
  │
  ├── Execute get_time → {"time":"11:42 AM","date":"Monday April 7","tz":"Central"}
  ├── Feed tool result back to LLM
  │
  ├── POST again with tool result
  ├── LLM returns: "The time is 11:42 AM Central on Monday, April 7th."
  │
  ├── g_core->tts_speak("The time is 11:42 AM Central on Monday, April 7th.")
  ├── Audio queued → PTT → spoken over RF
  │
  └── Conversation state saved for multi-turn follow-up

User keys up again: "and what about the weather?"
  │
  ├── mod_asr → "and what about the weather?"
  ├── mod_ai: no wake phrase, but active conversation for this caller
  ├── Append to existing conversation
  ├── LLM gets full context, calls get_weather tool
  ├── Response: "Currently sunny and 67 degrees with light south winds."
  └── Spoken over RF
```

## Implementation Order

1. **Tool registry** — struct, register, lookup, execute
2. **LLM HTTP client** — POST /v1/chat/completions, parse response, handle tool_calls
3. **Built-in tools** — get_weather, get_time, get_repeater_status, get_stats
4. **Wake phrase detection** — simple string prefix match
5. **Orchestration loop** — ASR event → wake check → LLM → tools → TTS
6. **Conversation manager** — per-caller state, timeout, multi-turn
7. **DTMF trigger** — *99# via mod_dtmfcmd integration
8. **Admin tools** — send_page, set_emergency, run_command (access controlled)
9. **CLI commands** — ai status/tools/history/ask/reset
10. **Config + example** — [ai] section in kerchunk.conf

## Security

- Tools marked `admin_only` require the caller to be identified as admin
  (via CTCSS, DTMF login, or OTP) before execution
- `run_command` tool is always admin-only and logs all invocations
- `set_emergency` tool is admin-only
- `send_page` tool is admin-only
- LLM responses are sanitized before TTS (strip special chars, limit length)
- Conversation timeout prevents stale context from being used
- Rate limiting: max 1 AI request per COR cycle, configurable cooldown between requests

## Model Recommendations

| Model | Size | VRAM/RAM | Tokens/s (CPU) | Tool Calling | Notes |
|-------|------|----------|----------------|--------------|-------|
| Phi-3 mini 3.8B Q4 | 2.3GB | 3GB | 8-12 t/s | Good | Best for CPU |
| Llama 3.2 3B Q4 | 2.0GB | 3GB | 10-15 t/s | Good | Fast, good quality |
| Mistral 7B Q4 | 4.1GB | 5GB | 5-8 t/s | Excellent | Best tool calling |
| Qwen 2.5 7B Q4 | 4.5GB | 5GB | 5-8 t/s | Excellent | Best multilingual |
| Functionary 7B Q4 | 4.1GB | 5GB | 5-8 t/s | **Best** | Purpose-built for tools |

For your 12-core ARM CPU: **Llama 3.2 3B Q4** gives the best speed/quality tradeoff.
For a GPU setup: **Functionary 7B** or **Mistral 7B** for superior tool calling.
