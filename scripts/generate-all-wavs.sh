#!/bin/bash
#
# generate-all-wavs.sh — Regenerate ALL static WAV files via Piper TTS
#
# Requires: wyoming-speak (from libwyoming) and a running Wyoming TTS server.
# The kerchunk audio queue auto-resamples, so native Piper rate (22050) is fine.
#
# Usage:
#   ./scripts/generate-all-wavs.sh [sounds_dir] [host] [port]
#
# Options:
#   --force    Overwrite existing files (default: skip existing)
#
# Defaults:
#   sounds_dir = ./sounds
#   host       = localhost
#   port       = 10200

set -euo pipefail

FORCE=0
if [[ "${1:-}" == "--force" ]]; then
    FORCE=1
    shift
fi

SOUNDS_DIR="${1:-./sounds}"
HOST="${2:-localhost}"
PORT="${3:-10200}"

if ! command -v wyoming-speak &>/dev/null; then
    echo "ERROR: wyoming-speak not found. Install libwyoming tools first." >&2
    exit 1
fi

if ! wyoming-describe "$HOST" "$PORT" &>/dev/null; then
    echo "ERROR: Cannot reach Wyoming TTS server at $HOST:$PORT" >&2
    exit 1
fi

GENERATED=0
SKIPPED=0
ERRORS=0

generate() {
    local subdir="$1"
    local name="$2"
    local text="$3"
    local dir="${SOUNDS_DIR}/${subdir}"
    local outfile="${dir}/${name}.wav"

    mkdir -p "$dir"

    if [ "$FORCE" -eq 0 ] && [ -f "$outfile" ]; then
        SKIPPED=$((SKIPPED + 1))
        return
    fi

    if wyoming-speak "$HOST" "$PORT" "$text" "$outfile" 2>/dev/null; then
        echo "  ${subdir}/${name}.wav — \"$text\""
        GENERATED=$((GENERATED + 1))
    else
        echo "  ERROR: ${subdir}/${name}.wav" >&2
        ERRORS=$((ERRORS + 1))
    fi
}

# ── caller/ ──────────────────────────────────────────────────────
echo "=== caller ==="
generate caller caller_identified  "Caller identified."
generate caller caller_login_ok    "Login successful."
generate caller caller_login_fail  "Login failed."

# ── numbers/ ─────────────────────────────────────────────────────
echo "=== numbers ==="
generate numbers num_0   "Zero."
generate numbers num_1   "One."
generate numbers num_2   "Two."
generate numbers num_3   "Three."
generate numbers num_4   "Four."
generate numbers num_5   "Five."
generate numbers num_6   "Six."
generate numbers num_7   "Seven."
generate numbers num_8   "Eight."
generate numbers num_9   "Nine."
generate numbers num_10  "Ten."
generate numbers num_20  "Twenty."
generate numbers num_30  "Thirty."
generate numbers num_40  "Forty."
generate numbers num_50  "Fifty."
generate numbers num_60  "Sixty."
generate numbers num_70  "Seventy."
generate numbers num_80  "Eighty."
generate numbers num_90  "Ninety."
generate numbers num_100 "One hundred."

# ── phone/ ───────────────────────────────────────────────────────
echo "=== phone ==="
generate phone phone_access_denied  "Access denied."
generate phone phone_busy           "The line is busy."
generate phone phone_connected      "Call connected."
generate phone phone_dialing        "Dialing."
generate phone phone_disconnected   "Call disconnected."
generate phone phone_error          "Phone system error."
generate phone phone_no_answer      "No answer."
generate phone phone_not_available  "Phone service not available."
generate phone phone_ringing        "Ringing."
generate phone phone_timeout        "Call timed out."

# ── system/ ──────────────────────────────────────────────────────
echo "=== system ==="
generate system system_access_denied  "Access denied."
generate system system_emergency_on   "Emergency mode activated."
generate system system_emergency_off  "Emergency mode deactivated."
generate system system_error          "System error."
generate system system_ok             "OK."
generate system system_ready          "System ready."
generate system system_shutdown       "System shutting down."
generate system system_timeout        "Timeout."

# ── time/ ────────────────────────────────────────────────────────
echo "=== time ==="
generate time tm_the_time_is  "The time is"
generate time tm_oclock       "o'clock"
generate time tm_oh           "oh"
generate time tm_am           "AM"
generate time tm_pm           "PM"
generate time tm_eastern      "Eastern"
generate time tm_central      "Central"
generate time tm_mountain     "Mountain"
generate time tm_pacific      "Pacific"
generate time tm_alaska       "Alaska"
generate time tm_hawaii       "Hawaii"

# ── voicemail/ ───────────────────────────────────────────────────
echo "=== voicemail ==="
generate voicemail vm_greeting      "Please leave a message after the tone."
generate voicemail vm_recorded      "Message recorded."
generate voicemail vm_deleted       "Message deleted."
generate voicemail vm_no_messages   "No messages."
generate voicemail vm_one_message   "You have one message."
generate voicemail vm_messages      "messages."
generate voicemail vm_mailbox_full  "Mailbox full."
generate voicemail vm_end           "End of messages."

# ── weather/ conditions ──────────────────────────────────────────
echo "=== weather ==="
generate weather wx_current_weather  "Current weather."
generate weather wx_forecast         "Today's forecast."
generate weather wx_sunny            "Sunny."
generate weather wx_clear            "Clear."
generate weather wx_partly_cloudy    "Partly cloudy."
generate weather wx_overcast         "Overcast."
generate weather wx_cloudy           "Cloudy."
generate weather wx_thunderstorm     "Thunderstorm."
generate weather wx_rainy            "Rainy."
generate weather wx_snowy            "Snowy."
generate weather wx_foggy            "Foggy."

# ── weather/ measurements ────────────────────────────────────────
generate weather wx_temperature      "Temperature"
generate weather wx_degrees          "degrees"
generate weather wx_wind             "Wind"
generate weather wx_miles_per_hour   "miles per hour"
generate weather wx_percent          "percent"
generate weather wx_minus            "minus"
generate weather wx_humidity         "Humidity"
generate weather wx_high             "High"
generate weather wx_low              "Low"
generate weather wx_chance_of_rain   "Chance of rain"
generate weather wx_chance_of_snow   "Chance of snow"

# ── weather/ wind directions ─────────────────────────────────────
generate weather wx_dir_n    "North"
generate weather wx_dir_nne  "North northeast"
generate weather wx_dir_ne   "Northeast"
generate weather wx_dir_ene  "East northeast"
generate weather wx_dir_e    "East"
generate weather wx_dir_ese  "East southeast"
generate weather wx_dir_se   "Southeast"
generate weather wx_dir_sse  "South southeast"
generate weather wx_dir_s    "South"
generate weather wx_dir_ssw  "South southwest"
generate weather wx_dir_sw   "Southwest"
generate weather wx_dir_wsw  "West southwest"
generate weather wx_dir_w    "West"
generate weather wx_dir_wnw  "West northwest"
generate weather wx_dir_nw   "Northwest"
generate weather wx_dir_nnw  "North northwest"

# ── Summary ──────────────────────────────────────────────────────
echo ""
echo "Done: $GENERATED generated, $SKIPPED skipped, $ERRORS errors"
echo "Files in: $SOUNDS_DIR/"
