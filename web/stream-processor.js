/* stream-processor.js — AudioWorklet ring buffer + resampler
 *
 * Receives Float32 audio from main thread port, buffers in a 96k-sample
 * ring (12s at 8kHz), resamples to device rate, outputs to speakers.
 */
const SRC_RATE = 8000;
const RING = 96000;
const PRIME = 4000;

class StreamProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ring = new Float32Array(RING);
    this.w = 0;
    this.r = 0;
    this.frac = 0;
    this.step = SRC_RATE / sampleRate;
    this.primed = false;
    this.dryRuns = 0;
    this.dryThresh = Math.ceil(sampleRate * 5.0 / 128);
    this.statTick = 0;
    this.statInterval = Math.ceil(sampleRate / 128); // ~1 per second
    this.framesIn = 0;
    this.underruns = 0;
    this.samplesOut = 0;
    this.port.onmessage = (e) => {
      if (e.data instanceof Float32Array) {
        this.framesIn++;
        const d = e.data;
        for (let i = 0; i < d.length; i++) {
          const next = (this.w + 1) % RING;
          if (next === this.r) this.r = (this.r + 1) % RING;
          this.ring[this.w] = d[i];
          this.w = next;
        }
      }
    };
  }
  process(inputs, outputs) {
    const out = outputs[0][0];
    const avail = (this.w - this.r + RING) % RING;
    if (!this.primed) {
      if (avail < PRIME) { out.fill(0); return true; }
      this.primed = true;
      this.dryRuns = 0;
    }
    let got = 0;
    for (let i = 0; i < out.length; i++) {
      if (this.r !== this.w) {
        out[i] = this.ring[this.r];
        this.frac += this.step;
        while (this.frac >= 1.0) {
          this.frac -= 1.0;
          this.r = (this.r + 1) % RING;
        }
        got++;
      } else {
        out[i] = 0;
        this.underruns++;
      }
    }
    if (got === 0) {
      if (++this.dryRuns > this.dryThresh) this.primed = false;
    } else {
      this.dryRuns = 0;
    }
    this.samplesOut += out.length;
    if (++this.statTick >= this.statInterval) {
      this.port.postMessage({
        type: 'stats',
        avail: avail,
        primed: this.primed,
        framesIn: this.framesIn,
        underruns: this.underruns,
        samplesOut: this.samplesOut,
        ringSize: RING,
        step: this.step
      });
      this.statTick = 0;
      this.framesIn = 0;
      this.underruns = 0;
      this.samplesOut = 0;
    }
    return true;
  }
}
registerProcessor('stream-processor', StreamProcessor);
