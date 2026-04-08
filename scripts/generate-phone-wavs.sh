#!/bin/bash
#
# generate-phone-wavs.sh — Generate missing phone/ WAV files via Wyoming TTS
#
# Requires: wyoming-speak (from libwyoming) and a running Wyoming TTS server.
# The kerchunk audio queue auto-resamples, so native Piper rate (22050) is fine.
#
# Usage:
#   ./scripts/generate-phone-wavs.sh [sounds_dir] [host] [port]
#
# Defaults:
#   sounds_dir = ./sounds
#   host       = localhost
#   port       = 10200

set -euo pipefail

SOUNDS_DIR="${1:-./sounds}"
HOST="${2:-localhost}"
PORT="${3:-10200}"
PHONE_DIR="${SOUNDS_DIR}/phone"

if ! command -v wyoming-speak &>/dev/null; then
    echo "ERROR: wyoming-speak not found. Install libwyoming tools first." >&2
    exit 1
fi

# Verify server is reachable
if ! wyoming-describe "$HOST" "$PORT" &>/dev/null; then
    echo "ERROR: Cannot reach Wyoming TTS server at $HOST:$PORT" >&2
    exit 1
fi

mkdir -p "$PHONE_DIR"

# Phone prompts: filename → spoken text
declare -A PROMPTS=(
    [phone_access_denied]="Access denied."
    [phone_busy]="The line is busy."
    [phone_connected]="Call connected."
    [phone_dialing]="Dialing."
    [phone_disconnected]="Call disconnected."
    [phone_error]="Phone system error."
    [phone_no_answer]="No answer."
    [phone_not_available]="Phone service not available."
    [phone_ringing]="Ringing."
    [phone_timeout]="Call timed out."
)

GENERATED=0
SKIPPED=0

for name in "${!PROMPTS[@]}"; do
    outfile="${PHONE_DIR}/${name}.wav"
    if [ -f "$outfile" ]; then
        echo "SKIP: $outfile (already exists)"
        SKIPPED=$((SKIPPED + 1))
        continue
    fi
    text="${PROMPTS[$name]}"
    echo "GEN:  $outfile — \"$text\""
    if ! wyoming-speak "$HOST" "$PORT" "$text" "$outfile" 2>&1; then
        echo "ERROR: Failed to generate $outfile" >&2
        exit 1
    fi
    GENERATED=$((GENERATED + 1))
done

echo ""
echo "Done: $GENERATED generated, $SKIPPED skipped (already existed)"
echo "Phone WAVs are in: $PHONE_DIR/"
