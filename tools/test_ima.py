"""Decode the firmware's IMA ADPCM with the firmware's own C, on the host.

A wrong decoder does not crash and does not show up on the screen: it comes out
of the speaker as noise, and this board cannot hear itself -- board_capture sees
the framebuffer, not what reached the ES8311. So main/hal/ima_adpcm.h is a
header with no ESP dependencies, and this compiles those exact lines with the
host gcc (through WSL, as tools/uibudget does) and checks three things:

  1. the C decoder agrees sample for sample with an independent decoder written
     here from the IMA tables, over pseudorandom nibbles and over real audio;
  2. the round trip through the encoder keeps the signal -- an SNR floor, which
     is what catches a decoder that merely agrees with a broken reference;
  3. decoding from block k alone gives the same samples as decoding the whole
     clip and skipping to block k, which is the claim audio.player's seek and
     pause/resume rest on.

Run: python tools/test_ima.py
"""
import math
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
        45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
        209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
        796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
        2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
        7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
        20350, 22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "ima_adpcm.h"

// Reads "block count" then `count` payload bytes as hex, and prints the decoded
// samples one per line -- the same walk play_clip() does, without the I2S.
int main(int argc, char **argv) {
    unsigned block = (unsigned)atoi(argv[1]);
    unsigned frames = (unsigned)atoi(argv[2]);
    static uint8_t payload[1 << 16];
    unsigned n = 0, byte;
    while (n < sizeof payload && scanf("%2x", &byte) == 1) payload[n++] = (uint8_t)byte;
    ima_t s = {.data = payload, .bytes = n, .block = (uint16_t)block};
    for (unsigned i = 0; i < frames; i++) printf("%d\n", (int)ima_next(&s));
    return 0;
}
'''


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def encode(samples, block):
    """Samples to WAV-layout IMA blocks. The standard search: the nibble whose
    reconstruction is closest, which is what any encoder in the wild produces."""
    per_block = 1 + (block - 4) * 2
    out = bytearray()
    for start in range(0, len(samples), per_block):
        chunk = samples[start:start + per_block]
        if len(chunk) < per_block:
            chunk = list(chunk) + [chunk[-1]] * (per_block - len(chunk))
        predictor, index = chunk[0], 0
        if start:                      # continuity across blocks costs nothing
            index = out_index
        out += struct.pack('<hBB', predictor, index, 0)
        nibbles = []
        for want in chunk[1:]:
            step = STEP[index]
            delta = want - predictor
            code = 8 if delta < 0 else 0
            delta = abs(delta)
            if delta >= step:
                code |= 4
                delta -= step
            if delta >= step >> 1:
                code |= 2
                delta -= step >> 1
            if delta >= step >> 2:
                code |= 1
            diff = step >> 3
            if code & 1:
                diff += step >> 2
            if code & 2:
                diff += step >> 1
            if code & 4:
                diff += step
            predictor = clamp(predictor - diff if code & 8 else predictor + diff,
                              -32768, 32767)
            index = clamp(index + INDEX[code], 0, 88)
            nibbles.append(code)
        for i in range(0, len(nibbles), 2):
            out.append(nibbles[i] | (nibbles[i + 1] << 4))
        out_index = index
    return bytes(out)


def decode(payload, block, frames):
    """An independent walk of the same layout, written from the tables above."""
    out, at = [], 0
    while len(out) < frames:
        if at + 4 > len(payload):
            out.append(0)
            continue
        predictor, index, _ = struct.unpack_from('<hBB', payload, at)
        # A step index past 88 is not a sample the format can produce, so what
        # to do with one is the host's choice rather than the codec's. Both
        # sides start the block over at 0: a corrupt header then decodes quietly
        # instead of at the loudest step the table has.
        if not 0 <= index <= 88:
            index = 0
        at += 4
        out.append(predictor)
        for i in range((block - 4) * 2):
            if len(out) >= frames:
                return out[:frames]
            byte_at = at + i // 2
            if byte_at >= len(payload):
                out.append(0)
                continue
            byte = payload[byte_at]
            code = byte & 0xf if i % 2 == 0 else byte >> 4
            step = STEP[index]
            diff = step >> 3
            if code & 1:
                diff += step >> 2
            if code & 2:
                diff += step >> 1
            if code & 4:
                diff += step
            predictor = clamp(predictor - diff if code & 8 else predictor + diff,
                              -32768, 32767)
            index = clamp(index + INDEX[code], 0, 88)
            out.append(predictor)
        at += (block - 4)
    return out[:frames]


def wsl_path(path):
    drive = path.drive.rstrip(':').lower()
    return '/mnt/' + drive + str(path).replace('\\', '/')[2:]


def build(work):
    source = work / 'harness.c'
    source.write_text(HARNESS, encoding='utf-8')
    binary = work / 'harness'
    run = subprocess.run(
        ['wsl', '-e', 'bash', '-lc',
         f'gcc -O2 -std=gnu11 -I{wsl_path(ROOT / "main" / "hal")} '
         f'-Wall -Wextra -Wno-unused-function '
         f'-o {wsl_path(binary)} {wsl_path(source)}'],
        capture_output=True, text=True)
    if run.returncode:
        raise SystemExit('test_ima: the harness would not compile\n' + run.stderr)
    return binary


def run_c(binary, payload, block, frames):
    run = subprocess.run(
        ['wsl', '-e', wsl_path(binary), str(block), str(frames)],
        input=payload.hex(), capture_output=True, text=True)
    if run.returncode:
        raise SystemExit('test_ima: the harness failed\n' + run.stderr)
    return [int(line) for line in run.stdout.split()]


def snr(signal, decoded):
    noise = sum((a - b) ** 2 for a, b in zip(signal, decoded))
    power = sum(a * a for a in signal)
    return 10 * math.log10(power / noise) if noise else math.inf


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        binary = build(Path(tmp))

        # 1. Real audio: a 440 Hz tone at the host's own rate, at -6 dBFS.
        rate, seconds = 24000, 0.25
        tone = [int(16384 * math.sin(2 * math.pi * 440 * n / rate))
                for n in range(int(rate * seconds))]
        for block in (256, 512, 1024):
            payload = encode(tone, block)
            frames = len(tone)
            c = run_c(binary, payload, block, frames)
            py = decode(payload, block, frames)
            if c != py:
                bad = next(i for i, (a, b) in enumerate(zip(c, py)) if a != b)
                print(f'FAIL block={block}: C and reference part at sample {bad}'
                      f' ({c[bad]} vs {py[bad]})')
                failures += 1
                continue
            db = snr(tone, c)
            # IMA's own headroom is about 20 dB on a sine; anything near it says
            # the decoder is tracking the signal rather than agreeing with a
            # broken encoder.
            if db < 18:
                print(f'FAIL block={block}: round trip only {db:.1f} dB SNR')
                failures += 1
            else:
                print(f'  block={block}: {frames} samples, C == reference, '
                      f'{db:.1f} dB SNR')

        # 2. Nibble streams no encoder would produce, to walk the clamps.
        state = 12345
        payload = bytearray()
        for _ in range(4096):
            state = (state * 1103515245 + 12345) & 0x7fffffff
            payload.append((state >> 16) & 0xff)
        block = 256
        frames = (len(payload) // block) * (1 + (block - 4) * 2)
        c = run_c(binary, bytes(payload), block, frames)
        py = decode(bytes(payload), block, frames)
        if c != py:
            bad = next(i for i, (a, b) in enumerate(zip(c, py)) if a != b)
            print(f'FAIL random: C and reference part at sample {bad}')
            failures += 1
        else:
            print(f'  random nibbles: {frames} samples, C == reference')

        # 3. What seek and resume claim: a block decodes the same alone as it
        # does in the middle of the clip.
        payload = encode(tone, 256)
        per_block = 1 + (256 - 4) * 2
        whole = run_c(binary, payload, 256, len(tone))
        for index in (1, 3, 7):
            tail = run_c(binary, payload[index * 256:], 256, per_block)
            want = whole[index * per_block:index * per_block + per_block]
            if tail != want:
                print(f'FAIL resume: block {index} decodes differently alone')
                failures += 1
                break
        else:
            print('  resume: blocks 1, 3 and 7 decode the same alone as in place')

    print('IMA_ADPCM ok' if not failures else f'IMA_ADPCM {failures} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
