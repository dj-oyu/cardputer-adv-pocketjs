"""Convert a WAV into the one shape this host plays, for embedding in assets:/.

The firmware has no resampler and no downmixer -- docs/common-api.md 9.1 and
9.2 both explain why, and both come back to one I2S controller having one sample
rate and the DAC's being 24000. So audio.player refuses anything that is not
24 kHz mono, and the conversion has to happen here, on a machine that has room
for it.

Why this file exists at all: streaming playback had nothing in-tree to play.
`app:/` caps a file at 24,576 bytes, which is the same number the old in-RAM
buffer used, so nothing stored there can demonstrate that the cap is gone.
`assets:/` is flash-mapped firmware content with no such cap, so a clip put
there is the unambiguous case -- the old player would have refused it at open()
with LIMIT_EXCEEDED, and this one plays it.

PCM16 rather than IMA ADPCM, deliberately, and it is the reason the output is
large. ADPCM would compress the same audio to a quarter of the size -- which
would put it back UNDER 24,576 bytes and demonstrate nothing. PCM16 also feeds
the ring at 48,000 bytes a second against ADPCM's 12,000, so it is the worst
case for the producer and the honest thing to test with.

The anti-aliasing is not decoration. 44,100 to 24,000 is a decimation, and
everything above 12 kHz in the source folds back into the audible band if it is
not filtered out first -- as hiss and as tones that were never played. A linear
interpolator alone does almost nothing about that. So: a windowed-sinc low-pass
at 11 kHz, then linear interpolation.

    python tools/make_wav_asset.py IN.wav OUT.wav [--repeat N]

--repeat concatenates the clip N times. One pass of the sample material is about
a second, which is over twice the old cap but only two ring-fills; three passes
is about three seconds and roughly 35 refills, which is a sustained feed rather
than a burst. Clicks are also the easiest thing to hear an underrun in: a gap in
a click train is obvious where a gap in a chord is not.
"""
import argparse
import math
import struct
import sys
from pathlib import Path

OUT_RATE = 24000


def read_wav(path):
    """The subset of RIFF this needs: PCM16, any rate, mono or stereo."""
    raw = Path(path).read_bytes()
    if raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
        raise SystemExit(f'{path}: not a WAV file')
    at, fmt, data = 12, None, None
    while at + 8 <= len(raw):
        cid = raw[at:at + 4]
        size = struct.unpack('<I', raw[at + 4:at + 8])[0]
        if size > len(raw) - at - 8:
            raise SystemExit(f'{path}: chunk {cid!r} runs past the end')
        if cid == b'fmt ' and size >= 16:
            fmt = struct.unpack('<HHIIHH', raw[at + 8:at + 24])
        elif cid == b'data':
            data = raw[at + 8:at + 8 + size]
        at += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise SystemExit(f'{path}: no fmt or no data chunk')
    tag, channels, rate, _, _, bits = fmt
    if tag != 1 or bits != 16:
        raise SystemExit(f'{path}: only PCM16 in, got format {tag} {bits}-bit')
    n = len(data) // (2 * channels)
    samples = struct.unpack(f'<{n * channels}h', data[:n * channels * 2])
    if channels == 1:
        mono = list(samples)
    else:
        # Average rather than take one side: a stereo source with a signal
        # only in the right channel would otherwise convert to silence.
        mono = [sum(samples[i * channels:(i + 1) * channels]) // channels
                for i in range(n)]
    return mono, rate


def lowpass(samples, rate, cutoff, taps=63):
    """Windowed-sinc FIR, applied so nothing above `cutoff` survives the
    decimation to fold back into the band. Hamming window, odd tap count so the
    delay is a whole sample and the output lines up with the input."""
    if cutoff >= rate / 2:
        return samples
    half = taps // 2
    fc = cutoff / rate
    kernel = []
    for i in range(taps):
        k = i - half
        s = 2 * fc if k == 0 else math.sin(2 * math.pi * fc * k) / (math.pi * k)
        kernel.append(s * (0.54 - 0.46 * math.cos(2 * math.pi * i / (taps - 1))))
    gain = sum(kernel)
    kernel = [k / gain for k in kernel]
    n = len(samples)
    out = [0] * n
    for i in range(n):
        acc = 0.0
        for j, k in enumerate(kernel):
            at = i + j - half
            if 0 <= at < n:
                acc += samples[at] * k
        out[i] = acc
    return out


def resample(samples, rate_in, rate_out):
    """Linear interpolation, after the filter above has made it safe."""
    if rate_in == rate_out:
        return [int(round(s)) for s in samples]
    n_out = int(len(samples) * rate_out / rate_in)
    step = rate_in / rate_out
    out = []
    for i in range(n_out):
        pos = i * step
        a = int(pos)
        frac = pos - a
        lo = samples[a]
        hi = samples[a + 1] if a + 1 < len(samples) else lo
        out.append(int(round(lo + (hi - lo) * frac)))
    return out


def write_wav(path, samples):
    body = struct.pack(f'<{len(samples)}h',
                       *[max(-32768, min(32767, s)) for s in samples])
    header = (b'RIFF' + struct.pack('<I', 36 + len(body)) + b'WAVE'
              + b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, OUT_RATE,
                                      OUT_RATE * 2, 2, 16)
              + b'data' + struct.pack('<I', len(body)))
    Path(path).write_bytes(header + body)
    return len(header) + len(body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source')
    ap.add_argument('out')
    ap.add_argument('--repeat', type=int, default=1)
    args = ap.parse_args()

    mono, rate = read_wav(args.source)
    filtered = lowpass(mono, rate, min(11000, OUT_RATE / 2 - 1000))
    out = resample(filtered, rate, OUT_RATE) * args.repeat
    size = write_wav(args.out, out)
    ms = len(out) * 1000 // OUT_RATE
    peak = max(abs(s) for s in out) if out else 0
    print(f'WAV_ASSET in={rate}Hz frames={len(mono)} '
          f'out=24000Hz frames={len(out)} durationMs={ms} '
          f'bytes={size} peak={peak}')
    # The number the whole exercise turns on: the old player refused anything
    # over 24,576 bytes, so say plainly whether this file would have been.
    print(f'WAV_ASSET vs the old 24576-byte cap: '
          f'{"OVER by " + str(size - 24576) if size > 24576 else "UNDER -- proves nothing"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
