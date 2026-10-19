"""Build-local runner mode proof; no Git metadata or source writes required."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from games.derby_runner_batch import instrument

ROOT = Path(__file__).resolve().parents[1]


def verify(build: Path, expected: str):
    cache = (build / 'CMakeCache.txt').read_bytes()
    modes = re.findall(rb'^POCKET_DERBY_RUNNER_BATCH:BOOL=(ON|OFF)\r?$', cache, re.M)
    if modes != [expected.encode()]:
        raise ValueError('runner batch cache mode mismatch or ambiguous')
    source = (ROOT / 'apps/derby/derby_pan.js').read_bytes()
    generated = instrument(source.decode()).encode()
    path = build / 'generated/derby_runner_batch/derby_pan.js'
    elf = (build / 'cardputer_pocketjs.elf').read_bytes()
    symbols = subprocess.check_output(['xtensa-esp-elf-nm', '--defined-only',
                                     str(build / 'cardputer_pocketjs.elf')], text=True)
    present = any(line.split()[-1] == 'js_runners' for line in symbols.splitlines() if line.split())
    enabled = expected == 'ON'
    if present != enabled:
        raise ValueError('runner batch native symbol mismatch')
    target, other = (generated, source) if enabled else (source, generated)
    if target + b'\0' not in elf or other + b'\0' in elf:
        raise ValueError('actual embedded PAN does not match requested mode')
    if enabled and (not path.exists() or path.read_bytes() != generated):
        raise ValueError('generated PAN mismatch')
    if not enabled and path.exists():
        raise ValueError('unexpected generated PAN in OFF build')
    image = (build / 'cardputer_pocketjs.bin').read_bytes()
    elf_hash = hashlib.sha256(elf).digest()
    if (len(image) < 288 or image[0] != 0xE9 or struct.unpack_from('<I', image, 32)[0] != 0xABCD5432
            or image[144:176].split(b'\0')[0] != b'v6.0.1' or image[176:208] != elf_hash):
        raise ValueError('actual IDF 6.0.1 BIN/ELF pair mismatch')
    return dict(runner_batch_mode=expected, native_symbol=present,
                cmake_cache_sha256=hashlib.sha256(cache).hexdigest(),
                embedded_pan_sha256=hashlib.sha256(target).hexdigest(), embedded_pan_bytes=len(target),
                bin_sha256=hashlib.sha256(image).hexdigest(), elf_sha256=elf_hash.hex())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--expect', choices=['ON', 'OFF'], required=True)
    args = parser.parse_args()
    proof = verify(args.build.resolve(), args.expect)
    (args.build / 'runner-batch-proof.json').write_text(json.dumps(proof, indent=2) + '\n')
    print('DERBY_RUNNER_BATCH_PROOF ' + json.dumps(proof, separators=(',', ':')))


if __name__ == '__main__':
    main()
