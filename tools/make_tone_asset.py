"""A steady tone, as the material that can catch an underrun ANYWHERE.

    python tools/make_tone_asset.py OUT.wav [--seconds 30] [--hz 541.7]

Then encode it the same way the click train is encoded, so the two are
comparable end to end:

    wsl -e bash -lc "cd /mnt/c/... && bash tools/make_opus_asset.sh OUT.wav OUT.pok"

WHY A TONE AND NOT ONLY THE CLICK TRAIN, which is the real content of this file.

`apps/chime/chime.wav` is a click train, chosen because a gap in a train of
events is obvious where a gap in a chord is not. That reasoning is right about
the events and wrong about the gaps between them: **a click train is mostly
silence, and inserting silence into silence is not merely hard to hear, it is
inaudible in principle.** Only an underrun that lands ON a click can be
perceived at all. With short transients over eight seconds that is a few percent
of the timeline, so "listen for a break in the train" was checking a small
fraction of the run while sounding like it checked all of it.

A sustained tone has no silent moments, so every underrun lands on signal.

AND THE UNDERRUN POLICY MAKES IT STRICTLY BETTER. `stretch` inserts silence
WITHOUT ADVANCING THE SOURCE (sound.h), so when the tone resumes it resumes at
the phase it was interrupted at, not the phase it would have reached. That is a
step discontinuity in the waveform -- a pop -- and the ear is far better at
pops than at missing events. The property that made `stretch` the right policy
is the property a tone exposes and a click train hides.

CHOOSING THE FREQUENCY, WHICH IS NOT ARBITRARY AND NEARLY WENT WRONG.

An underrun is exactly 128 frames (sound.c's block), which at 24 kHz is 5.333 ms
= 1/187.5 s. **A tone whose frequency is a multiple of 187.5 Hz resumes exactly
in phase after an underrun, so it produces NO discontinuity and NO pop.** The
silence is still there, but at 5.3 ms a pure gap with no phase step is close to
inaudible -- the very defect we are listening for would be silent.

That rules out some obvious picks. 1000 Hz is 5.333 x 187.5, so three
consecutive underruns are perfectly in phase. 375 Hz and 562.5 Hz are exact
multiples and are silent for EVERY count. 656.25 Hz maximises the step for one
underrun and is silent for every even number of them.

So the frequency is chosen to keep the phase offset far from a whole cycle for
every underrun count 1..8. Searching 400-1200 Hz for the best worst case:

    541.7 Hz   f/187.5 = 2.8891   worst step over n=1..8 = 0.111 cycles
    634.0 Hz                      0.051
    440.0 Hz                      0.040
    1000.0 Hz                     0.000   <- silent at n=3, 6

541.7 Hz it is: mid-band where the ear is sensitive, and the offsets walk
0.89, 0.78, 0.67, 0.56, 0.45, 0.33, 0.22, 0.11 -- never near zero.

The tone is STEADY rather than swept or beating. A beat has amplitude nulls,
which reintroduces exactly the blind spots the click train had. A slow sweep
would let a listener say roughly where a gap fell, but that is not needed: the
player already logs `positionMs` beside `underruns`, so the instrument supplies
the position and the ear only has to supply "there was a pop". Steady is the
more sensitive detector, so steady is what this writes.

The fades are not decoration. A tone that starts at full amplitude on a nonzero
sample IS a click, and it would be the first thing a listener hears -- a false
positive at t=0 on every run. 20 ms raised-cosine at each end costs nothing and
removes it.
"""
import argparse
import math
import struct
from pathlib import Path

RATE = 24000        # the only rate this host plays; see docs/common-api.md 9.2
BLOCK = 128         # sound.c's block, and so the length of one underrun
DEFAULT_HZ = 541.7  # see the header: chosen against multiples of RATE/BLOCK
FADE_MS = 20


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out', type=Path)
    ap.add_argument('--seconds', type=float, default=30.0)
    ap.add_argument('--hz', type=float, default=DEFAULT_HZ)
    ap.add_argument('--gain', type=float, default=0.5)
    args = ap.parse_args()

    commensurate = RATE / BLOCK
    ratio = args.hz / commensurate
    near = abs(ratio - round(ratio))
    if near < 0.05:
        print(f'refusing {args.hz} Hz: it is {ratio:.4f} x {commensurate} Hz, so an '
              f'underrun would resume nearly in phase and make no pop. '
              f'Pick a frequency away from a multiple of {commensurate} Hz.')
        return 1

    n = int(args.seconds * RATE)
    fade = int(FADE_MS / 1000 * RATE)
    peak = int(32767 * args.gain)
    pcm = bytearray()
    for i in range(n):
        s = math.sin(2 * math.pi * args.hz * i / RATE)
        if i < fade:
            s *= 0.5 - 0.5 * math.cos(math.pi * i / fade)
        elif i >= n - fade:
            s *= 0.5 - 0.5 * math.cos(math.pi * (n - 1 - i) / fade)
        pcm += struct.pack('<h', int(round(s * peak)))

    header = (b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' +
              struct.pack('<IHHIIHH', 16, 1, 1, RATE, RATE * 2, 2, 16) +
              b'data' + struct.pack('<I', len(pcm)))
    args.out.write_bytes(header + pcm)

    worst = min(min((ratio * k) % 1.0, 1 - (ratio * k) % 1.0) for k in range(1, 9))
    print(f'{args.out}: {args.seconds:g} s of {args.hz:g} Hz at {RATE} Hz mono, '
          f'{len(pcm) + 44} bytes of PCM16')
    print(f'  f/{commensurate:g} = {ratio:.4f}; worst phase step over 1..8 '
          f'underruns = {worst:.3f} cycles (0 would be silent)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
