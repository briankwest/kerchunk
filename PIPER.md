# Piper TTS Setup Guide

Piper is a free, offline neural text-to-speech engine. It runs locally on ARM64 and x86_64 with no cloud API, no internet connection, and no per-request cost. Typical synthesis latency is 1-3 seconds on ARM64.

## Quick Start

### 1. Install Piper Binary

Download the pre-built binary for your architecture from [GitHub Releases](https://github.com/rhasspy/piper/releases):

```bash
# ARM64 (Raspberry Pi, Orange Pi, etc.)
cd /tmp
curl -sL https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_aarch64.tar.gz | tar xz
sudo cp piper/piper /usr/bin/
sudo cp piper/lib*.so* /usr/lib/
sudo cp -r piper/espeak-ng-data /usr/share/
sudo ldconfig

# x86_64
curl -sL https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz | tar xz
sudo cp piper/piper /usr/bin/
sudo cp piper/lib*.so* /usr/lib/
sudo cp -r piper/espeak-ng-data /usr/share/
sudo ldconfig
```

Verify:
```bash
echo "Hello world" | piper --model /path/to/model.onnx --output_raw | wc -c
```

### 2. Download a Voice Model

Models are `.onnx` files paired with `.onnx.json` config files. Both files must be in the same directory.

```bash
sudo mkdir -p /usr/share/piper/models
cd /usr/share/piper/models
```

**Recommended voices** (all US English):

| Voice | Quality | Size | Description |
|-------|---------|------|-------------|
| `en_US-lessac-high` | High | 109 MB | Clear female, best quality |
| `en_US-ryan-high` | High | 109 MB | Clear male, best quality |
| `en_US-ljspeech-high` | High | 109 MB | Female, classic LJSpeech |
| `en_US-hfc_female-medium` | Medium | 61 MB | Female, good balance of speed/quality |
| `en_US-hfc_male-medium` | Medium | 61 MB | Male, good balance |
| `en_US-amy-medium` | Medium | 61 MB | Female |
| `en_US-joe-medium` | Medium | 61 MB | Male |

Download a voice (example: lessac high quality):
```bash
# Download model + config (both required)
MODEL=en_US-lessac-high
sudo curl -sL "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/lessac/high/${MODEL}.onnx" \
    -o ${MODEL}.onnx
sudo curl -sL "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/lessac/high/${MODEL}.onnx.json" \
    -o ${MODEL}.onnx.json
```

For other voices, adjust the URL path. The pattern is:
```
https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/{lang}/{locale}/{name}/{quality}/{model_name}.onnx
```

**British English voices:**

| Voice | Quality | Description |
|-------|---------|-------------|
| `en_GB-cori-high` | High | British female |
| `en_GB-alan-medium` | Medium | British male |
| `en_GB-alba-medium` | Medium | Scottish female |
| `en_GB-northern_english_male-medium` | Medium | Northern English male |

```bash
# Example: British female
MODEL=en_GB-cori-high
sudo curl -sL "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_GB/cori/high/${MODEL}.onnx" \
    -o ${MODEL}.onnx
sudo curl -sL "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_GB/cori/high/${MODEL}.onnx.json" \
    -o ${MODEL}.onnx.json
```

### 3. Configure kerchunk

In `kerchunk.conf`, set the `[tts]` section:

```ini
[tts]
engine = piper
piper_binary = /usr/bin/piper
piper_model = /usr/share/piper/models/en_US-lessac-high.onnx
piper_speaker = 0
```

Reload config:
```bash
sudo kill -HUP $(pgrep kerchunkd)
```

### 4. Test

```bash
kerchunk tts say "This is a test of piper text to speech"
kerchunk tts status
```

## Voice Quality Comparison

| Quality | Sample Rate | Model Size | Latency (ARM64) | Use Case |
|---------|------------|------------|-----------------|----------|
| `low` | 16000 Hz | ~15 MB | <1 sec | Low-resource devices |
| `medium` | 22050 Hz | ~60 MB | 1-2 sec | Good balance for repeaters |
| `high` | 22050 Hz | ~110 MB | 2-4 sec | Best quality, recommended |

For repeater use, **high quality is recommended** — the extra 1-2 seconds of synthesis time is negligible since audio is cached after first synthesis. Subsequent plays of the same text are instant.

## Caching

Piper responses are cached as WAV files in `<sounds_dir>/cache/tts/`. Identical text is synthesized once; subsequent requests serve from cache instantly.

- Cache is engine-aware — switching between Piper and ElevenLabs produces separate cache entries
- Clear cache: `kerchunk tts cache-clear`
- Cache location: shown in `kerchunk tts status`

## Multi-Speaker Models

Some models support multiple speakers (e.g., `en_US-libritts-high` has 900+ speakers). Set the speaker index:

```ini
piper_speaker = 42
```

Check the `.onnx.json` file for available speaker IDs:
```bash
python3 -c "import json; d=json.load(open('/usr/share/piper/models/YOUR_MODEL.onnx.json')); print(d.get('num_speakers', 1))"
```

## Switching Between Engines

You can switch between Piper (local) and ElevenLabs (cloud) without restarting:

```ini
[tts]
engine = piper          ; or: elevenlabs
; Piper settings (used when engine = piper)
piper_binary = /usr/bin/piper
piper_model = /usr/share/piper/models/en_US-lessac-high.onnx
; ElevenLabs settings (used when engine = elevenlabs)
api_key = sk_your_key_here
voice_id = 21m00Tcm4TlvDq8ikWAM
model = eleven_turbo_v2_5
```

Reload: `sudo kill -HUP $(pgrep kerchunkd)`

## Troubleshooting

**"Piper binary not executable"** — Check path and permissions:
```bash
ls -la /usr/bin/piper
which piper
```

**"Piper model not readable"** — Check model path:
```bash
ls -la /usr/share/piper/models/*.onnx
```

**No audio output** — Check the log for piper errors:
```bash
tail -f /var/log/kerchunk/events.log | grep tts
```

Test piper directly:
```bash
echo "test" | piper --model /usr/share/piper/models/YOUR_MODEL.onnx --output_raw | wc -c
```
Should output a non-zero byte count.

**Shared library errors** — Run `sudo ldconfig` after copying piper libraries:
```bash
sudo ldconfig
ldd /usr/bin/piper | grep "not found"
```

## Browse All Voices

Full voice catalog: https://huggingface.co/rhasspy/piper-voices/tree/v1.0.0

Languages available: Arabic, Catalan, Czech, Danish, German, Greek, English, Spanish, Finnish, French, Hungarian, Icelandic, Italian, Georgian, Kazakh, Luxembourgish, Nepali, Dutch, Norwegian, Polish, Portuguese, Romanian, Russian, Serbian, Swahili, Swedish, Turkish, Ukrainian, Vietnamese, Chinese.
