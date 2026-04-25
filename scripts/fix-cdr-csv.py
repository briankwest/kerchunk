#!/usr/bin/env python3
"""
fix-cdr-csv.py — repair legacy mod_cdr CSV files in place.

Older mod_cdr writes had two problems:

  1. Announcement rows used a 9-column shape instead of the 11-column
     voice-row shape, breaking any reader that expects fixed columns.
  2. NEITHER row type quoted operator-influenced fields, so any
     announcement description or user_name with an embedded comma
     spilled over into adjacent columns. TTS-spelled callsigns
     ("WRDP519 repeater, 462.55 megahertz, PL 136.5 hertz") are the
     usual culprit.

The fixed writer (post-2026-04-25) emits 11 RFC 4180-compliant
columns for both row types and quotes anything containing a comma /
quote / newline.

This tool walks one or more CDR directories and rewrites every
YYYY-MM-DD.csv in place with the new format. Original files are
saved alongside as YYYY-MM-DD.csv.bak so you can compare or roll
back.

Usage:
    sudo ./scripts/fix-cdr-csv.py /var/lib/kerchunk/cdr
    sudo ./scripts/fix-cdr-csv.py --dry-run /var/lib/kerchunk/cdr
    sudo ./scripts/fix-cdr-csv.py --no-backup /var/lib/kerchunk/cdr/2026-04-24.csv

Run it as the same user that owns the CDR files (typically the
kerchunk system user, so prefix with sudo when fixing the live
deployment).
"""

import argparse
import csv
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

CSV_HEADER = [
    "timestamp", "date", "time", "user_id", "user_name",
    "method", "duration_s", "emergency",
    "avg_rms", "peak_rms", "recording",
]

DATE_FILE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}\.csv$")


def is_int(s):
    if s == "" or s is None:
        return False
    if s[0] in "+-":
        s = s[1:]
    return s.isdigit()


def is_float(s):
    if s is None:
        return False
    try:
        float(s)
        return True
    except (TypeError, ValueError):
        return False


def looks_like_voice_row(cols):
    """Already-clean 11-column voice row — keep as-is."""
    if len(cols) != 11:
        return False
    return (is_int(cols[0])              # timestamp
        and is_int(cols[3])              # user_id
        and is_float(cols[6])            # duration_s
        and cols[7] in ("0", "1")        # emergency
        and is_int(cols[8])              # avg_rms
        and is_int(cols[9]))             # peak_rms


def looks_like_legacy_announcement(cols):
    """Old 9-column announcement OR a comma-spilled variant.

    Pattern: cols[3] == "0" AND cols[4] == "system". Other shape
    indicators are unreliable because the description spillage
    creates a variable column count."""
    return (len(cols) >= 9
        and is_int(cols[0])
        and cols[3] == "0"
        and cols[4] == "system")


def reconstruct_announcement(cols):
    """Glue the comma-split description back together.

    Layout was:
      ts, date, time, "0", "system", source, "0.0", "0", description...

    Description sat in cols[8..end] and was naïvely comma-split, so
    rejoin with ", " (the original separator was "," — the leading
    space in subsequent fields is from the natural-language source
    text). Returns 11 fields in the new clean layout, with empty
    avg_rms/peak_rms/recording columns and the description placed
    in the recording slot (matching the new writer)."""
    ts, date, time_, _user_id, _user_name = cols[:5]
    source = cols[5] if len(cols) > 5 else ""
    duration = cols[6] if len(cols) > 6 else "0.0"
    emergency = cols[7] if len(cols) > 7 else "0"
    description = ",".join(cols[8:]) if len(cols) > 8 else ""
    description = description.strip()
    return [
        ts, date, time_, "0", "system",
        source, "0.0", "0", "0", "0", description,
    ]


def fix_file(path: Path, *, backup: bool, dry_run: bool):
    """Returns (kept, fixed, dropped, total) counts."""
    raw = path.read_text(encoding="utf-8", errors="replace")
    lines = raw.splitlines()
    if not lines:
        return (0, 0, 0, 0)

    out_rows = [CSV_HEADER]
    kept = fixed = dropped = 0

    # Skip header if present.
    start = 1 if lines[0].lower().startswith("timestamp,") else 0

    for line in lines[start:]:
        if not line.strip():
            continue
        # Naïve split — exactly the broken behavior we're cleaning up
        # after, so we walk it the same way the old broken reader did.
        cols = line.split(",")

        if looks_like_voice_row(cols):
            out_rows.append(cols)
            kept += 1
            continue

        if looks_like_legacy_announcement(cols):
            out_rows.append(reconstruct_announcement(cols))
            fixed += 1
            continue

        # Anything else: a row we don't understand. Drop, but loud.
        dropped += 1

    total = kept + fixed + dropped

    if dry_run:
        return (kept, fixed, dropped, total)

    # Write atomically into a temp file in the same dir, then replace.
    if backup:
        bak = path.with_suffix(path.suffix + ".bak")
        if not bak.exists():
            shutil.copy2(path, bak)

    fd, tmp_name = tempfile.mkstemp(
        dir=path.parent, prefix=path.name + ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", newline="", encoding="utf-8") as fp:
            w = csv.writer(fp, quoting=csv.QUOTE_MINIMAL)
            for row in out_rows:
                w.writerow(row)
        # Preserve ownership/perms — important when fixing live files
        # owned by the kerchunk system user.
        st = path.stat()
        os.chown(tmp_name, st.st_uid, st.st_gid)
        os.chmod(tmp_name, st.st_mode)
        os.replace(tmp_name, path)
    except Exception:
        os.unlink(tmp_name)
        raise

    return (kept, fixed, dropped, total)


def collect_targets(paths):
    """Expand directories to their YYYY-MM-DD.csv children, leave
    explicit file args alone."""
    out = []
    for p in paths:
        path = Path(p)
        if path.is_dir():
            out.extend(sorted(
                f for f in path.iterdir()
                if f.is_file() and DATE_FILE_RE.match(f.name)))
        elif path.is_file():
            out.append(path)
        else:
            print(f"warning: skipping {p}: not a file or directory",
                  file=sys.stderr)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+",
        help="CDR directory or specific YYYY-MM-DD.csv file(s)")
    ap.add_argument("--dry-run", action="store_true",
        help="report what would change, don't write")
    ap.add_argument("--no-backup", action="store_true",
        help="skip the .bak file (default: keep one)")
    args = ap.parse_args()

    targets = collect_targets(args.paths)
    if not targets:
        print("no CDR files found", file=sys.stderr)
        return 2

    grand = [0, 0, 0, 0]
    for path in targets:
        try:
            kept, fixed, dropped, total = fix_file(
                path, backup=not args.no_backup, dry_run=args.dry_run)
        except OSError as e:
            print(f"  {path}: ERROR — {e}", file=sys.stderr)
            continue

        verb = "would update" if args.dry_run else "updated"
        if fixed == 0 and dropped == 0:
            verb = "ok"
        print(f"  {path}: {verb} — kept={kept} fixed={fixed} "
              f"dropped={dropped} total={total}")
        for i, n in enumerate((kept, fixed, dropped, total)):
            grand[i] += n

    print()
    print(f"total: kept={grand[0]} fixed={grand[1]} "
          f"dropped={grand[2]} ({grand[3]} rows across "
          f"{len(targets)} file{'s' if len(targets) != 1 else ''})")
    if args.dry_run:
        print("dry-run — no files modified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
