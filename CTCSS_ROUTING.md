# CTCSS/DCS Tone-Based Action Routing

## Concept

Key up on a specific CTCSS or DCS tone and the repeater automatically triggers an action — no DTMF commands needed. Each tone acts like a dedicated "service channel" within the same frequency.

```
User keys up with CTCSS 141.3 Hz
  -> mod_caller: no user with that tone (or identifies them if assigned)
  -> mod_route: matches route.2 -> fires weather announcement
```

Works alongside caller identification — the same tone can both identify a user AND trigger a route. Or use tones not assigned to any user as dedicated service tones.

## Configuration

```ini
[route]
enabled = on            ; global enable/disable

[route.1]
ctcss = 1318            ; 131.8 Hz -> auto voicemail for user 2
action = voicemail
target = 2              ; target user ID

[route.2]
ctcss = 1413            ; 141.3 Hz -> weather
action = weather

[route.3]
dcs = 47                ; DCS 047 -> parrot echo
action = parrot

[route.4]
ctcss = 1622            ; 162.2 Hz -> speak a message
action = tts
text = Welcome to the repeater.

[route.5]
ctcss = 1710            ; 171.0 Hz -> play a WAV
action = announce
wav = sounds/system/system_ready.wav

[route.6]
ctcss = 2035            ; 203.5 Hz -> selective calling for group 2
action = group
target = 2              ; target group ID
```

## Supported Actions

| Action | What happens | Reuses event |
|--------|-------------|-------------|
| `voicemail` | Records to target user's mailbox | CUSTOM+1 |
| `weather` | Current weather announcement | CUSTOM+8 |
| `forecast` | Forecast announcement | CUSTOM+9 |
| `time` | Time check | CUSTOM+10 |
| `parrot` | Arms echo mode | CUSTOM+13 |
| `nws` | NWS alert check | CUSTOM+14 |
| `tts` | Speaks configured text | Direct g_core->tts_speak() |
| `announce` | Plays a WAV file | Direct g_core->queue_audio_file() |
| `group` | Selective calling (TX with group tone) | CUSTOM+16/17 |

## How It Works

### Event Flow

```
User keys radio with CTCSS 141.3 Hz
  -> Hardware detects carrier -> KERCHEVT_COR_ASSERT
  -> mod_repeater starts debounce timer
  -> Audio thread runs CTCSS decoder on captured frames
  -> CTCSS decoder locks -> KERCHEVT_CTCSS_DETECT (active=1, freq_x10=1413)
     -> mod_caller.on_ctcss_detect: no user with 1413 -> no ID (or identifies if assigned)
     -> mod_route.on_ctcss_detect: matches route.2 -> execute_route(weather)
        -> fires KERCHEVT_CUSTOM + 8
        -> mod_weather handles it: fetches + announces weather
  -> mod_repeater debounce expires -> RECEIVING state
  -> User unkeys -> KERCHEVT_COR_DROP
     -> mod_route resets fired-this-COR flag
```

### Debounce

Each route fires once per COR cycle. CTCSS/DCS detection events fire continuously while the tone is present, but the module tracks a "fired this COR" flag per route that resets on COR drop.

### Voicemail Routing

For voicemail recording, the timing works naturally:
1. User keys up with the voicemail tone
2. mod_route detects tone, fires CUSTOM+1 (voicemail record) with target user ID
3. mod_voicemail starts recording (registers audio tap)
4. User speaks their message
5. User unkeys (COR drop) -> recording stops and saves

### Selective Calling (Group Routing)

When `action = group` fires:
1. mod_route fires KERCHEVT_CUSTOM+16 (group select) with group ID
2. mod_txcode subscribes to this event, looks up the target group's TX CTCSS/DCS tone
3. mod_txcode switches the TX encoder to that group's tone
4. Only radios programmed with the matching tone squelch open — other radios stay muted
5. On COR drop, mod_route fires KERCHEVT_CUSTOM+17 (group clear)
6. mod_txcode reverts to the normal TX encoder (caller-based or repeater default)

This is standard analog selective calling — not encrypted, but effective for talkgroup-like separation on a single frequency.

### Emergency Mode

All tone routes are suppressed during emergency mode, except `action = group` (selective calling is a routing function, not an announcement).

## Implementation Details

### New Module: mod_route.c

- Subscribes to: `KERCHEVT_CTCSS_DETECT`, `KERCHEVT_DCS_DETECT`, `KERCHEVT_COR_DROP`
- Static route table: up to 32 entries
- `route_configure()`: iterates `[route.N]` sections (N = 1..32)
- `on_ctcss_detect()`: scans routes for matching freq_x10, fires action if not already fired this COR
- `on_dcs_detect()`: scans routes for matching dcs_code
- `on_cor_drop()`: resets all fired flags
- `execute_route()`: dispatches action (fires custom events or calls TTS/queue directly)
- CLI command `route`: shows route table and active state

### Interaction with Caller ID

Tone routing and caller identification are independent. Both subscribe to the same CTCSS/DCS detection events. The event bus delivers to all subscribers, so:

- **Tone assigned to user AND route**: both identify the user AND trigger the action
- **Tone assigned to user only**: normal caller ID, no route action
- **Tone assigned to route only**: action fires, caller stays anonymous
- **Tone not assigned to anything**: ignored by both

### Custom Event Offsets

Currently allocated: 0-15. New allocations for mod_route:

| Offset | Name | Purpose |
|--------|------|---------|
| 16 | ROUTE_EVT_GROUP_SELECT | Selective calling start |
| 17 | ROUTE_EVT_GROUP_CLEAR | Selective calling end |

### Files to Create

| File | Description |
|------|-------------|
| `modules/mod_route.c` | New module: route table, event handlers, action dispatch, CLI |
| `tests/test_integ_route.c` | Integration tests (11 test cases) |

### Files to Modify

| File | Change |
|------|--------|
| `modules/mod_txcode.c` | Add group select/clear event handlers (CUSTOM+16/17) |
| `Makefile` | Add mod_route.c to MOD_SRC, test to TEST_SRC |
| `tests/test_integration.c` | Wire in test_integ_route |
| `kerchunk.conf.example` | Add [route] and [route.N] documentation |
| `CLAUDE.md` | Document tone routing and new event offsets |
| `README.md` | Add mod_route section |

### Module Load Order

mod_route must load AFTER all action modules (so their event subscriptions are active when routes fire). Recommended position:

```ini
load = ...,mod_parrot,mod_cdr,mod_tts,mod_nws,mod_route,mod_stats,mod_web
```

### Test Cases

1. CTCSS tone fires weather action
2. Same tone on same COR cycle doesn't re-fire (debounce)
3. COR drop resets debounce, next COR fires again
4. DCS route match
5. Unmatched tone is ignored
6. Disabled module ignores all tones
7. Voicemail route passes target as argument
8. Group select fires CUSTOM+16 with correct group ID
9. COR drop fires CUSTOM+17 (group clear)
10. Emergency mode suppresses routes (except group)
11. Multiple routes can fire in same COR cycle (different tones)

## Future Enhancements

- **Scheduled routes**: fire an action at specific times (cron-style)
- **Conditional routes**: only fire if caller has specific access level
- **Chain routes**: one route triggers another (e.g., weather + time together)
- **Rate limiting**: prevent abuse by limiting how often a route can fire
- **Route priorities**: when multiple tones match, fire highest priority only
