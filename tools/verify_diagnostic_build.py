"""Mandatory diagnostic image proof, usable in immutable source archives.

The broker supplies source identity separately. This proof binds the measured
mode to the same BIN/ELF hashes recorded in its existing durable manifest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from record_verification import verify_mode, verify_measure_mode


def prove(build):
    mode = verify_mode(build, 'ON')
    measure = verify_measure_mode(build, 'ON')
    image = (build/'cardputer_pocketjs.bin').read_bytes()
    elf = (build/'cardputer_pocketjs.elf').read_bytes()
    elf_hash = hashlib.sha256(elf).digest()
    if (len(image) < 288 or image[0] != 0xE9
            or struct.unpack_from('<I', image, 32)[0] != 0xABCD5432
            or image[144:176].split(b'\0')[0] != b'v6.0.1'
            or image[176:208] != elf_hash):
        raise ValueError('diagnostic proof requires an IDF 6.0.1 BIN paired with the measured ELF')
    return dict(schema_version=1, build_mode=mode, build_measure=measure, hardware_tested=False,
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
