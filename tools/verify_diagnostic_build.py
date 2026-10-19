"""Mandatory diagnostic image proof, usable in immutable source archives.

The broker supplies source identity separately. This proof binds the measured
mode to the same BIN/ELF hashes recorded in its existing durable manifest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from record_verification import verify_mode, verify_measure_mode, verify_fixed_mode, fixed_expected, verify_detail_mode, detail_mode
from derby_compare_chunks import FIXED_SAVED_RACE
from elf_symbol_value import symbol_u32

ROOT = Path(__file__).resolve().parents[1]
FIXED_HW = 0x2545F491


def prove(build):
    mode = verify_mode(build, 'ON')
    measure = verify_measure_mode(build, 'ON')
    workload = verify_fixed_mode(build)
    detail = verify_detail_mode(build)
    if workload['mode'] != 'ON':
        raise ValueError('finite diagnostic source requires fixed workload ON')
    cache = (build/'CMakeCache.txt').read_text()
    fixed = [line.split('=', 1)[1] for line in cache.splitlines()
             if line.startswith('POCKET_DERBY_COMPARE_FIXED_HW:BOOL=')]
    if fixed != ['ON']:
        raise ValueError('diagnostic comparison requires fixed hardware seed')
    image = (build/'cardputer_pocketjs.bin').read_bytes()
    elf = (build/'cardputer_pocketjs.elf').read_bytes()
    source_view = (ROOT/'apps/derby/derby_view.js').read_bytes()
    expected_view = fixed_expected(detail_mode(build)=='ON')['derby_view.js']
    view_path = build/'generated/derby_fixed_workload/derby_view.js'
    if (not view_path.is_file() or view_path.read_bytes() != expected_view
            or expected_view+b'\0' not in elf or source_view+b'\0' in elf):
        raise ValueError('diagnostic comparison requires exact generated view in ELF')
    if symbol_u32(elf, 'pocket_derby_comparison_hw_seed') != FIXED_HW:
        raise ValueError('diagnostic ELF does not contain the provider hardware seed')
    elf_hash = hashlib.sha256(elf).digest()
    if (len(image) < 288 or image[0] != 0xE9
            or struct.unpack_from('<I', image, 32)[0] != 0xABCD5432
            or image[144:176].split(b'\0')[0] != b'v6.0.1'
            or image[176:208] != elf_hash):
        raise ValueError('diagnostic proof requires an IDF 6.0.1 BIN paired with the measured ELF')
    return dict(schema_version=1, build_mode=mode, build_measure=measure, fixed_workload=workload, detail_measurement=detail,
                comparison_seed=dict(mode='fixed', hardware_seed=f'{FIXED_HW:08X}', saved_race=FIXED_SAVED_RACE,
                                     embedded_view_sha256=hashlib.sha256(expected_view).hexdigest(),
                                     elf_seed_symbol_verified=True, elf_embedded_view_verified=True),
                hardware_tested=False,
                artifacts={
                    'cardputer_pocketjs.bin': dict(bytes=len(image), sha256=hashlib.sha256(image).hexdigest()),
                    'cardputer_pocketjs.elf': dict(bytes=len(elf), sha256=elf_hash.hex())})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    report = prove(args.build.resolve())
    text = json.dumps(report, sort_keys=True)
    (args.build/'diagnostic-mode-proof.json').write_text(text+'\n')
    print('DERBY_DIAGNOSTIC_BUILD_PROOF '+text, flush=True)


if __name__ == '__main__':
    main()
