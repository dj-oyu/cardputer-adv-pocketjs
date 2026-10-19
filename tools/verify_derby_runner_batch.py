"""Build-local runner mode proof; no Git metadata or source writes required."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from games.derby_runner_batch import instrument
from derby_elf import object_symbol

ROOT = Path(__file__).resolve().parents[1]


def verify(build: Path, expected: str, expected_clip=None, head=None):
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
    extra={}
    if head is not None:
        if not re.fullmatch('[0-9a-f]{40}',head) or image[48:80].split(b'\0')[0]!=head[:12].encode():
            raise ValueError('embedded project version does not match the expected source head')
        description=json.loads((build/'project_description.json').read_text())
        if description['project_version']!=head[:12]:raise ValueError('build project version mismatch')
        extra['source_head']=head
    if expected_clip is not None:
        modes=re.findall(rb'^POCKET_DERBY_CLIP_NORM:BOOL=(ON|OFF)\r?$',cache,re.M)
        if modes!=[expected_clip.encode()]:raise ValueError('clip cache mode mismatch or ambiguous')
        native=(ROOT/'main/pocket/pocket_derby.c').read_text()
        body=re.search(r'static struct \{(.*?)\} view;',native,re.S)
        if body is None:raise ValueError('clip state source layout missing')
        # Compile a layout-only object; it is never linked into the firmware.
        probe=build/'clip-mode-layout.c';layout=build/'clip-mode-layout.o'
        probe.write_text('#include <stdint.h>\n#include <stdbool.h>\nstruct clip_layout {'+body[1]+'};\nconst uint32_t clip_layout_size=sizeof(struct clip_layout);\n')
        flags=['-DPOCKET_DERBY_CLIP_NORM=1'] if expected_clip=='ON' else []
        subprocess.run(['xtensa-esp-elf-gcc','-std=c11',*flags,'-c',str(probe),'-o',str(layout)],check=True)
        size,data=object_symbol(layout.read_bytes(),'clip_layout_size')
        if size!=4 or data is None:raise ValueError('invalid target layout proof')
        expected_size=struct.unpack('<I',data)[0]
        actual_size,_=object_symbol(elf,'view','pocket_derby.c')
        if actual_size!=expected_size:raise ValueError('actual ELF clip state does not match selected mode')
        extra.update(clip_norm_mode=expected_clip,elf_clip_state_bytes=actual_size,
                     native_source_sha256=hashlib.sha256(native.encode()).hexdigest())
    return dict(**extra,runner_batch_mode=expected, native_symbol=present,
                cmake_cache_sha256=hashlib.sha256(cache).hexdigest(),
                embedded_pan_sha256=hashlib.sha256(target).hexdigest(), embedded_pan_bytes=len(target),
                bin_sha256=hashlib.sha256(image).hexdigest(), elf_sha256=elf_hash.hex())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--expect', choices=['ON', 'OFF'], required=True)
    parser.add_argument('--expect-clip', choices=['ON','OFF'])
    parser.add_argument('--head', help='exact source SHA expected in the embedded project version')
    args = parser.parse_args()
    proof = verify(args.build.resolve(), args.expect, args.expect_clip, args.head)
    (args.build / 'runner-batch-proof.json').write_text(json.dumps(proof, indent=2) + '\n')
    print('DERBY_RUNNER_BATCH_PROOF ' + json.dumps(proof, separators=(',', ':')))


if __name__ == '__main__':
    main()
