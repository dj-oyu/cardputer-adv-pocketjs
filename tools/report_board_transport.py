"""Report actual display-storage symbols from a freshly built target ELF.

Named static storage only: not runtime free/contiguous heap, stack or timing.
The existing image/source verification remains authoritative for build mode.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess

REQUIRED = {'shared': 3840, 'tx_buf': 3840, 'window_x': 2, 'window_cols': 2}
BASE_HEAD = 'd41aadc8a9de106d8290ae5e398288fa29fb7a6d'
BASE_NAMED_BYTES = 3840 + 7680 + 4


def parse_symbols(text):
    symbols = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) != 4 or not re.fullmatch(r'[0-9a-fA-F]+', fields[1]):
            continue
        name = fields[3]
        if name in (*REQUIRED, 'tx_front', 'tx_pending'):
            if name in symbols:
                raise ValueError('duplicate display symbol: ' + name)
            symbols[name] = {'bytes': int(fields[1], 16), 'section_type': fields[2]}
    return symbols


def report(symbols, source_head, elf_sha256):
    if not re.fullmatch(r'[0-9a-f]{40}', source_head):
        raise ValueError('invalid exact source SHA')
    if not re.fullmatch(r'[0-9a-f]{64}', elf_sha256):
        raise ValueError('invalid ELF digest')
    for name, size in REQUIRED.items():
        actual = symbols.get(name)
        if actual is None or actual['bytes'] != size or actual['section_type'].lower() not in ('b', 'd'):
            raise ValueError('unexpected display storage: ' + name)
    if 'tx_front' in symbols:
        raise ValueError('two-buffer selector still linked')
    named = sum(symbols[name]['bytes'] for name in REQUIRED)
    return dict(schema_version=1, source_head=source_head, elf_sha256=elf_sha256,
                symbols=symbols, named_storage_bytes=named,
                comparison_base=BASE_HEAD, base_named_storage_bytes=BASE_NAMED_BYTES,
                named_storage_delta_bytes=named-BASE_NAMED_BYTES,
                scope='Actual named static symbols only; no runtime heap, largest block, guest headroom, stack or elapsed-time acceptance')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    elf = args.build / 'cardputer_pocketjs.elf'
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    expected = os.environ.get('SOURCE_HEAD', head)
    if head != expected:
        raise ValueError('build checkout does not match expected source head')
    symbols = parse_symbols(subprocess.check_output(
        ['xtensa-esp32s3-elf-nm', '-S', '--defined-only', str(elf)], text=True))
    result = report(symbols, head, hashlib.sha256(elf.read_bytes()).hexdigest())
    (args.build / 'board-transport.json').write_text(json.dumps(result, indent=2)+'\n')
    print('BOARD_TRANSPORT_STATIC '+json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
