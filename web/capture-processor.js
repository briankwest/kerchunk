/* capture-processor.js — AudioWorklet for mic capture + resample
 *
 * Receives mic input at device sample rate, resamples to the server's
 * sample rate, accumulates frames, and posts each complete frame to
 * main thread as Int16Array.
 *
 * processorOptions: { sampleRate: 48000, frameSize: 960 }
 */
class CaptureProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const dstRate = (options.processorOptions && options.processorOptions.sampleRate) || 48000;
    const frameSize = (options.processorOptions && options.processorOptions.frameSize) || 960;

    this.ratio = sampleRate / dstRate;
    this.avgLen = Math.max(1, Math.round(this.ratio));
    this.avgBuf = new Float32Array(this.avgLen);
    this.avgIdx = 0;
    this.avgSum = 0;
    this.phase = 0;
    this.dstRate = dstRate;
    this.buf = new Int16Array(frameSize);
    this.frameSize = frameSize;
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

      this.phase += this.dstRate;
      if (this.phase >= sampleRate) {
        this.phase -= sampleRate;
        const val = this.avgSum / this.avgLen;
        const s16 = Math.max(-32768, Math.min(32767, Math.round(val * 32767)));
        this.buf[this.pos++] = s16;
        if (this.pos >= this.frameSize) {
          this.port.postMessage(this.buf.slice());
          this.pos = 0;
        }
      }
    }
    return true;
  }
}
registerProcessor('capture-processor', CaptureProcessor);
