#!/bin/bash
#
# benchmark-voices.sh — Benchmark all installed Piper voices
#
# For each voice, starts wyoming-server with that model, synthesizes
# a test phrase 5 times, measures wall-clock time and audio duration
# to compute Real-Time Factor (RTF). Lower RTF = faster.
#
# Requires: wyoming-server, wyoming-speak, soxi (from sox)
#
set -euo pipefail

VOICE_DIR="/usr/share/wyoming/voices"
PORT=10299
PHRASES=(
  "The current temperature is seventy two degrees with partly cloudy skies."
  "Emergency mode activated."
  "The time is three forty five PM Eastern."
)
RUNS=5
TMPWAV="/tmp/bench_voice.wav"

echo "================================================================="
echo "Piper Voice Benchmark — $(date)"
echo "Platform: $(uname -m), $(nproc) cores"
echo "Phrases: ${#PHRASES[@]}, Runs per phrase: $RUNS"
echo "================================================================="
printf "\n%-30s %6s %8s %8s %8s %s\n" "Voice" "Quality" "Size(MB)" "RTF" "Time(s)" "Rate"
echo "-----------------------------------------------------------------------"

for model_dir in "$VOICE_DIR"/*/; do
  voice=$(basename "$model_dir")
  model="${model_dir}model.onnx"
  [ -f "$model" ] || continue

  # Extract quality from voice name (last segment after -)
  quality="${voice##*-}"
  model_size=$(du -sm "$model" | cut -f1)

  # Start a temporary server on the bench port
  wyoming-server -c /dev/stdin <<EOF &
[server]
host = 127.0.0.1
port = $PORT
[tts]
engine = piper-native
model = $model
[log]
level = error
EOF
  SERVER_PID=$!
  sleep 1

  # Verify server is up
  if ! kill -0 $SERVER_PID 2>/dev/null; then
    printf "%-30s %6s %8s %8s %8s FAILED TO START\n" "$voice" "$quality" "${model_size}" "-" "-"
    continue
  fi

  total_synth=0
  total_audio=0

  for phrase in "${PHRASES[@]}"; do
    for ((r=1; r<=RUNS; r++)); do
      rm -f "$TMPWAV"
      start=$(date +%s%N)
      wyoming-speak 127.0.0.1 $PORT "$phrase" "$TMPWAV" 2>/dev/null
      end=$(date +%s%N)

      if [ -f "$TMPWAV" ]; then
        synth_ms=$(( (end - start) / 1000000 ))
        audio_ms=$(soxi -D "$TMPWAV" 2>/dev/null | awk '{printf "%.0f", $1 * 1000}')
        total_synth=$((total_synth + synth_ms))
        total_audio=$((total_audio + audio_ms))
      fi
    done
  done

  kill $SERVER_PID 2>/dev/null
  wait $SERVER_PID 2>/dev/null || true

  if [ "$total_audio" -gt 0 ]; then
    rtf=$(awk "BEGIN{printf \"%.3f\", $total_synth / $total_audio}")
    avg_time=$(awk "BEGIN{printf \"%.2f\", $total_synth / (${#PHRASES[@]} * $RUNS) / 1000}")
    rate=$(soxi -r "$TMPWAV" 2>/dev/null || echo "?")
    printf "%-30s %6s %6sMB %8s %7ss %s Hz\n" "$voice" "$quality" "${model_size}" "$rtf" "$avg_time" "$rate"
  else
    printf "%-30s %6s %6sMB %8s %8s NO AUDIO\n" "$voice" "$quality" "${model_size}" "-" "-"
  fi

  rm -f "$TMPWAV"
done

echo "-----------------------------------------------------------------------"
echo "RTF = synthesis_time / audio_duration (lower is better, <1.0 = real-time)"
echo ""
