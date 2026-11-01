"""Render a mesh-only AYNM with real C evaluation and procedural rasterization."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[3]
C_SOURCE = Path(__file__).with_suffix('.c')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def png565(source, target):
    values = struct.unpack('<' + 'H' * (240 * 135), source.read_bytes())
    rows = bytearray()
    for y in range(135):
        rows.append(0)
        for v in values[y * 240:(y + 1) * 240]:
            rows.extend((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    target.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 135, 8, 2, 0, 0, 0)) +
                       chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('model', type=Path)
    parser.add_argument('--camera', nargs=9, type=float, default=[0, 0, 400, 0, 0, 0, 1, 120, 67])
    parser.add_argument('--light', nargs=6, type=float, default=[-.5, -.4, .8, 1, 1, 1])
    parser.add_argument('--root-pose', nargs=9, type=float)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Refuse stale-success ambiguity. Use a new output directory per invocation.
    generated = ('metadata.json', 'pixels.u16le', 'coverage.u16le', 'visible-triangles.u16le', 'pixels.png', 'render-report.json')
    if any((output / name).exists() for name in generated):
        parser.error('output directory already contains probe outputs; choose a fresh directory')
    model = args.model.resolve()
    report = {'kind': 'REAL C MODEL / PROCEDURAL RASTER PROBE / NO DEVICE', 'passed': False, 'input': str(model)}
    result = 1
    try:
        cc = shutil.which(args.cc)
        if not cc:
            raise RuntimeError('gcc is required; run on Linux/WSL without ESP SDK')
        if not all(math.isfinite(x) for x in args.camera + args.light + (args.root_pose or [])):
            raise RuntimeError('camera/light/pose must be finite')
        inputs = [C_SOURCE, ROOT / 'main/ayatsuri/ayatsuri_model.c', ROOT / 'main/ayatsuri/ayatsuri_model.h',
                  ROOT / 'main/ayatsuri/ayatsuri_transform.h', ROOT / 'main/ayatsuri/ayatsuri_shade.h',
                  ROOT / 'main/ayatsuri/ayatsuri_visibility.c', ROOT / 'main/ayatsuri/ayatsuri_visibility.h',
                  ROOT / 'main/ui/kasane/ksn_procedural.c', ROOT / 'main/ui/kasane/ksn_procedural.h']
        before = {str(p.relative_to(ROOT)): sha(p) for p in inputs}
        input_hash = sha(model)
        flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-ffp-contract=off', '-fexcess-precision=standard']
        if args.asan:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        with tempfile.TemporaryDirectory(prefix='aynm-raster-probe-') as temp:
            binary = Path(temp) / 'render-native'
            command = [cc, *flags, '-I', str(ROOT / 'main'), str(C_SOURCE),
                       str(ROOT / 'main/ayatsuri/ayatsuri_model.c'),
                       str(ROOT / 'main/ayatsuri/ayatsuri_visibility.c'),
                       str(ROOT / 'main/ui/kasane/ksn_procedural.c'), '-lm', '-o', str(binary)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if built.returncode:
                raise RuntimeError('compile failed: ' + built.stderr.strip())
            root = args.root_pose or [0, 0, 0, 0, 0, 0, 1, 1, 1]
            command = [str(binary), str(model), str(output), *map(repr, args.camera), *map(repr, args.light),
                       '1' if args.root_pose else '0', *map(repr, root)]
            run = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if run.returncode:
                raise RuntimeError(f'render exited {run.returncode}: {run.stderr.strip()}')
            metadata = json.loads((output / 'metadata.json').read_text())
            if metadata['tagCoverageMismatch'] != 0:
                raise RuntimeError('native tag replay coverage differs')
            for name in ('pixels.u16le', 'coverage.u16le', 'visible-triangles.u16le'):
                if (output / name).stat().st_size != 240 * 135 * 2:
                    raise RuntimeError('unexpected pixel buffer length')
            png565(output / 'pixels.u16le', output / 'pixels.png')
            report.update(inputSha256=input_hash, requestedParameters=dict(camera=args.camera,light=args.light,rootPose=root,rootPoseApplied=args.root_pose is not None), compiler=cc,
                          compilerVersion=subprocess.check_output([cc, '--version'], text=True).splitlines()[0],
                          flags=flags, sourceSha256=before,
                          sourcesUnchanged=before == {str(p.relative_to(ROOT)): sha(p) for p in inputs},
                          inputUnchanged=input_hash == sha(model), arenaBytes=metadata['arenaBytes'],
                          packets=metadata['packetCount'], triangles=metadata['triangleCount'],
                          coverageMismatch=metadata['tagCoverageMismatch'],
                          outputs={name: sha(output / name) for name in generated if name != 'render-report.json'},
                          scope='Native color rendering and separate constant-ID replay only. No independent depth/color oracle, device capture, or performance proof.')
            report['passed'] = report['sourcesUnchanged'] and report['inputUnchanged']
            result = 0 if report['passed'] else 1
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError, KeyError) as error:
        report['error'] = str(error)
    text = json.dumps(report, indent=2) + '\n'
    (output / 'render-report.json').write_text(text)
    sys.stdout.write(text)
    return result


if __name__ == '__main__':
    raise SystemExit(main())
