#!/bin/bash
#
# benchmark-asr.sh — Benchmark all ASR models via wyoming-server
#
set -euo pipefail

PORT=10298
RUNS=3
TESTWAV="/tmp/asr_test_16k.wav"

# Generate test audio if missing
if [ ! -f "$TESTWAV" ]; then
  wyoming-speak localhost 10200 "The current temperature is seventy two degrees with partly cloudy skies and winds from the northwest at ten miles per hour." /tmp/asr_test.wav 2>/dev/null
  sox /tmp/asr_test.wav -r 16000 "$TESTWAV" 2>/dev/null
fi

AUDIO_DUR=$(soxi -D "$TESTWAV" 2>/dev/null)

echo "================================================================="
echo "ASR Model Benchmark — $(date)"
echo "Platform: $(uname -m), $(nproc) cores"
echo "Test audio: ${AUDIO_DUR}s, 16kHz mono"
echo "Runs per model: $RUNS"
echo "================================================================="
printf "\n%-40s %8s %8s %8s %s\n" "Model" "Size" "RTF" "Time(s)" "Transcript"
echo "-----------------------------------------------------------------------------------------"

declare -A MODELS
MODELS["whisper-tiny.en (batch)"]="whisper|/usr/share/wyoming/models/whisper-tiny.en|off"
MODELS["whisper-base.en (batch)"]="whisper|/usr/share/wyoming/models/whisper-base.en|off"
MODELS["zipformer-en (streaming)"]="zipformer|/usr/share/wyoming/models/zipformer-en|on"
MODELS["parakeet-110m-ctc (batch)"]="nemo_ctc|/tmp/sherpa-onnx-nemo-parakeet_tdt_ctc_110m-en-36000-int8|off"
MODELS["parakeet-110m-ctc (stream)"]="nemo_ctc|/tmp/sherpa-onnx-nemo-parakeet_tdt_ctc_110m-en-36000-int8|on"
MODELS["parakeet-0.6b-tdt (batch)"]="zipformer|/tmp/sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8|off"
MODELS["parakeet-0.6b-tdt (stream)"]="zipformer|/tmp/sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8|on"

# Ordered keys for consistent output
ORDER=(
  "whisper-tiny.en (batch)"
  "whisper-base.en (batch)"
  "zipformer-en (streaming)"
  "parakeet-110m-ctc (batch)"
  "parakeet-110m-ctc (stream)"
  "parakeet-0.6b-tdt (batch)"
  "parakeet-0.6b-tdt (stream)"
)

for name in "${ORDER[@]}"; do
  IFS='|' read -r mtype mdir streaming <<< "${MODELS[$name]}"

  # Get model size
  msize=$(du -sm "$mdir" 2>/dev/null | cut -f1)

  # Write temp config
  cat > /tmp/bench_asr.conf << CONF
[server]
host = 127.0.0.1
port = $PORT
[asr]
engine = sherpa
model_dir = $mdir
model_type = $mtype
language = en
streaming = $streaming
[log]
level = error
CONF

  # Start server
  wyoming-server -c /tmp/bench_asr.conf &
  SRV_PID=$!
  sleep 2

  if ! kill -0 $SRV_PID 2>/dev/null; then
    printf "%-40s %6sMB %8s %8s FAILED TO START\n" "$name" "$msize" "-" "-"
    continue
  fi

  total_ms=0
  transcript=""

  for ((r=1; r<=RUNS; r++)); do
    start=$(date +%s%N)
    result=$(/tmp/test_asr "$TESTWAV" 127.0.0.1 $PORT 2>/dev/null)
    end=$(date +%s%N)
    ms=$(( (end - start) / 1000000 ))
    total_ms=$((total_ms + ms))
    if [ -z "$transcript" ]; then
      transcript=$(echo "$result" | sed 's/Transcript: //')
    fi
  done

  kill $SRV_PID 2>/dev/null
  wait $SRV_PID 2>/dev/null || true

  avg_ms=$((total_ms / RUNS))
  avg_s=$(awk "BEGIN{printf \"%.2f\", $avg_ms / 1000}")
  rtf=$(awk "BEGIN{printf \"%.3f\", $avg_ms / ($AUDIO_DUR * 1000)}")

  # Truncate transcript for display
  short="${transcript:0:60}"

  printf "%-40s %6sMB %8s %7ss %s\n" "$name" "$msize" "$rtf" "$avg_s" "$short"
done

echo "-----------------------------------------------------------------------------------------"
echo "RTF = processing_time / audio_duration (lower is better, <1.0 = real-time)"
echo ""
