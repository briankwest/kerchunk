# Plan: CDR browser tab in the admin UI

**Status:** Plan — not yet implemented. Author: pair, 2026-04-25.
**Branch:** `cdr-ui`.
**Goal:** Give operators a searchable, playable view of the CDR
history that mod_cdr already writes to disk. Date picker, caller /
method filters, inline WAV playback, no new persistence — the
CSVs and recordings on disk are the source of truth.
**No backwards compatibility constraints:** new endpoints, new page,
no changes to existing CDR writer or recording layout.

---

## 1. Why

Today the dashboard's "Today" panel shows live counters
(`today_calls`, `today_seconds`, in-call state) but nothing else.
The historical CSVs at `<cdr_dir>/YYYY-MM-DD.csv` and the WAV
recordings at `<recordings_dir>/<timestamp>_RX_<user>.wav` are
authoritative records that an operator might want to:

- Review what happened during a particular hour
- Find a transmission by a specific user (search/audit)
- Listen to a recording without SSH-ing into the box
- Spot-check audio quality after tuning the radio
- Pull a CSV for paperwork (FCC 95.1705 cooperative use record)

The data exists; the only gap is a way to browse it. mod_web
already serves auth-gated admin pages, so the new tab fits the
existing pattern.

This is a small feature — no architecture changes, no new
in-process state, no new background work. It's strictly a
read-only view over disk.

---

## 2. Scope and non-goals

**In scope:**

- Three new admin API endpoints (list days, fetch day's records,
  stream a recording) with realpath sandboxing on the recording
  endpoint
- New `web/admin/cdr.html` page with date picker, filters, sortable
  table, and inline `<audio>` playback
- Nav button on the main dashboard that links to the new page
- Doc updates (README, USAGE, ARCHITECTURE)

**Out of scope:**

- Multi-day search (one day at a time in v1; backend stays simple)
- Server-side filter/pagination (CSVs are small enough to ship the
  whole day to the client)
- Auto-prune of old CSVs or recordings (separate `[cdr] max_age_days`
  feature if ever wanted)
- Editing or annotating CDR records (read-only)
- Mobile-optimized layout (admin pages are desktop-first)

---

## 3. Backend — three endpoints

### 3.1 `GET /admin/api/cdr/days`

List of days that have a CDR file.

```json
{
  "days": ["2026-04-23", "2026-04-24", "2026-04-25"],
  "today": "2026-04-25"
}
```

Implementation: `opendir(g_cdr_dir)`, filter entries matching
`YYYY-MM-DD.csv`, strip `.csv`, sort ascending, JSON-encode.
`today` is `localtime()`-based YYYY-MM-DD so the UI can default
to it without a second roundtrip.

### 3.2 `GET /admin/api/cdr?date=2026-04-25`

That day's records.

```json
{
  "date": "2026-04-25",
  "count": 142,
  "records": [
    {"ts":1777068600,"time":"19:30:00","user_id":1,"user_name":"Brian",
     "method":"CTCSS","duration_s":4.2,"emergency":false,
     "avg_rms":1234,"peak_rms":5678,
     "recording":"recordings/20260425_193000_RX_brian.wav"}
  ]
}
```

Implementation:
- Open `<cdr_dir>/<date>.csv` (validate `<date>` matches `YYYY-MM-DD`
  pattern — defense-in-depth even though we sandbox to cdr_dir)
- Skip header row
- Parse each line with comma-split. Tolerant: drop lines that don't
  have at least 11 columns (last-line race with mod_cdr's writer)
- JSON-escape the user_name and recording fields
- Stream into a response buffer; cap response at ~2 MB to be safe

The whole day's CSV is returned in a single response; client-side
filtering is fast and avoids per-keystroke server round-trips. A
busy 200-call day at ~150 bytes per JSON record is ~30 KB.

### 3.3 `GET /admin/api/recording?path=<rel>`

Stream a WAV file from the recordings dir.

Implementation:
- Realpath both the request `path` and `[recording] directory`
- Reject if resolved path doesn't begin with `<recordings_real>/`
- Reject if it doesn't end in `.wav`
- Use `mg_http_serve_file` for streaming + Range request support
  (browsers seek through long recordings via Range)
- Sets `Content-Type: audio/wav`
- Optional `?download=1` query → adds
  `Content-Disposition: attachment; filename="..."`

This mirrors the same containment pattern as the recently-hardened
`cmd_play` (`resolve_under_sounds_dir` in src/main.c).

---

## 4. Frontend — `/admin/cdr.html`

Single column, ~600 px wide, matching the existing admin styling.

```
┌─ CDR Browser ────────────────────────────────┐
│ Date [2026-04-25 ▼]  ◀ ▶          (242 rec) │
│ Filter: [______] User  [______] Method       │
│ Show: ☑ voice  ☐ announcements  ☑ emergency │
├──────────────────────────────────────────────┤
│ Time     │ User   │ Method│ Dur  │       │   │
│──────────┼────────┼───────┼──────┼───────┼───│
│ 19:30:00 │ Brian  │ CTCSS │ 4.2s │ ▶     │ ⬇│
│ 19:31:12 │ unknown│ DTMF  │ 2.1s │ ▶     │ ⬇│
│ 19:32:00 │ system │ cwid  │  —   │       │   │
│ ...                                          │
│                                              │
│ [<audio> element appears below table         │
│  when ▶ is clicked]                          │
└──────────────────────────────────────────────┘
```

### Behavior

- **Date picker:** HTML `<input type="date">` populated from
  `/admin/api/cdr/days`. `min` / `max` attributes set to first /
  last available day. Defaults to `today`. Prev / next arrows
  step through available days only.
- **Live filters:** typing in User or Method box re-renders the
  table client-side via substring match (case-insensitive).
- **Show toggles:**
  - voice: rows where `user_name != "system"`
  - announcements: rows where `user_name == "system"`
  - emergency: rows where `emergency == true` (highlighted red
    regardless of toggle)
  Default: voice on, announcements off, emergency on.
- **Sortable columns:** click header to re-sort; default time desc.
- **▶ button:** disabled when `recording` is empty. Click loads
  `/admin/api/recording?path=<recording>` into a single shared
  `<audio controls>` element below the table. Replays the row's
  audio. Only one plays at a time.
- **⬇ button:** same URL with `?download=1` so the browser saves
  the WAV instead of streaming.
- **Row count badge:** shows "(N records)" matching the active
  filters.

### Implementation notes

- Pure vanilla JS, no framework — matches the existing admin
  pages.
- Single shared `<audio>` so we don't end up with N concurrent
  players if the user clicks several rows.
- Reuse `fmtDuration()` and the basic table CSS from
  `/admin/index.html`.

---

## 5. Nav integration

Add a "CDR" button to the admin header in `/admin/index.html`
alongside the existing "Users" button. One line of HTML; matches
the link pattern at line 157.

---

## 6. Files touched

| File | Change | Approx LOC |
|---|---|---|
| `modules/mod_web.c` | 3 new admin endpoint handlers + route lines | ~150 |
| `web/admin/cdr.html` | New page (table, filters, audio) | ~280 |
| `web/admin/index.html` | One nav button | ~3 |
| `README.md` | Document the page + endpoints | ~15 |
| `USAGE.md` | Same | ~15 |
| `ARCHITECTURE.md` | Add to API route list | ~3 |

Total: ~470 lines across 6 files.

---

## 7. Risks and edge cases

1. **Concurrent CSV write.** mod_cdr appends to today's CSV while
   a reader fetches it. Race: reader sees partial last line. Easy
   mitigation — column-count check drops malformed lines.

2. **Large recording files.** A 5-minute RX at 48 kHz mono is
   ~28 MB. `mg_http_serve_file` already handles Range requests for
   browser seeking, so no full-file buffering. The audio element
   will start playing as soon as enough is buffered.

3. **Path traversal.** The `?path=` parameter is operator-supplied
   (well, populated from `recording` column which mod_recorder writes,
   but treat as untrusted). Realpath both sides + prefix check is
   the same hardening already in place for `cmd_play`.

4. **CSV format drift.** If someone adds a column to mod_cdr's CSV
   later, the parser should ignore extras. Already the design —
   we read the first 11 columns and ignore the tail.

5. **Time-zone confusion.** The CSV has both UTC epoch (column 1)
   and local-time strings (columns 2+3). Frontend should display
   local time (matches operator expectations) but the JSON should
   include `ts` so client can re-render in any TZ. Use the local
   strings as authoritative for display; `ts` for sort.

6. **Days-list staleness.** `/admin/api/cdr/days` lists files at
   the moment of request. After midnight a new file appears. Refresh
   the days list when the user opens the picker (cheap), or just
   reload the page when needed.

7. **No file → 404.** If a user manipulates the URL with a date
   that has no CSV, return `{"date": "...", "records": []}` rather
   than 404 — simpler frontend handling.

8. **Auth on the recording endpoint.** Recordings can include
   sensitive content. The endpoint sits under `/admin/api/` so it
   inherits Basic Auth from mod_web. Don't accidentally place it
   under `/api/` (public).

---

## 8. Sequencing

1. **Backend endpoints first** — implement and exercise via curl.
   Verify the days list, day fetch, and recording stream all work
   with auth + sandboxing. (1 session)

2. **Frontend table + filters** — render records, no audio yet.
   Date picker + live filters + sort. Confirm the JSON contract
   feels right. (1 session)

3. **Audio playback + download** — wire the ▶ and ⬇ buttons. Test
   with a real recording. Verify Range/seek works in browser. (small)

4. **Nav button + polish** — emergency-row highlighting, "today"
   defaulting, row-count badge, doc sweep. (small)

Everything is independently shippable; there's no commit that
takes the dashboard to a non-working state.

---

## 9. Future extensions (not v1)

- Multi-day search via `?from=YYYY-MM-DD&to=YYYY-MM-DD`. Concatenated
  records, capped at N days. Client-side filter still applies.
- Aggregate view: daily totals over a date range (calls per day
  bar chart).
- Per-user statistics: total airtime per user over a date range.
- Export filtered subset as CSV (dump the current filtered view).
- Auto-prune: `[cdr] max_age_days = 90` deletes old CSVs and the
  matching recordings. (Different feature; touches mod_cdr only.)
- ASR integration: if a recording has a stored transcript (from
  mod_asr), surface it in a tooltip / expandable row.

These are deferred until someone actually asks for them.
