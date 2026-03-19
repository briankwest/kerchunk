/* capture-processor.js — AudioWorklet for mic capture + resample to 8kHz
 *
 * Receives mic input at device sample rate, resamples down to 8kHz,
 * accumulates 160-sample frames, and posts each complete frame to main thread.
 *
 * Uses a simple moving-average low-pass filter before decimation to avoid
 * aliasing, then picks the nearest sample at each output position.
 */
const DST_RATE = 8000;
const FRAME_SIZE = 160;

class CaptureProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ratio = sampleRate / DST_RATE;       /* e.g. 6.0 for 48kHz */
    this.avgLen = Math.max(1, Math.round(this.ratio));  /* filter window */
    this.avgBuf = new Float32Array(this.avgLen);
    this.avgIdx = 0;
    this.avgSum = 0;
    this.phase = 0;                            /* fractional input counter */
    this.buf = new Int16Array(FRAME_SIZE);
    this.pos = 0;
  }
  process(inputs) {
    const inp = inputs[0] && inputs[0][0];
    if (!inp) return true;

    for (let i = 0; i < inp.length; i++) {
      /* Running average low-pass filter (box filter over ratio samples) */
      this.avgSum -= this.avgBuf[this.avgIdx];
      this.avgBuf[this.avgIdx] = inp[i];
      this.avgSum += inp[i];
      this.avgIdx = (this.avgIdx + 1) % this.avgLen;

      this.phase += DST_RATE;
      if (this.phase >= sampleRate) {
        this.phase -= sampleRate;
        const val = this.avgSum / this.avgLen;
        const s16 = Math.max(-32768, Math.min(32767, Math.round(val * 32767)));
        this.buf[this.pos++] = s16;
        if (this.pos >= FRAME_SIZE) {
          this.port.postMessage(this.buf.slice());
          this.pos = 0;
        }
      }
    }
    return true;
  }
}
registerProcessor('capture-processor', CaptureProcessor);
