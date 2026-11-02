"""The C encoder (ksn_jit_emit, main/ui/kasane/ksn_shiki_jit.c) against the assembler.

Builds a host driver around ksn_shiki_jit.c, has it encode random operand
sets for every form of the generated table, assembles the same lines with
xtensa-esp32s3-elf-as and compares byte for byte. Also checks that
ksn_shiki_jit_enc.h is what gen_encoding.py makes now (a stale table fails).
Host only; skipped (exit 0, says so) where the Xtensa toolchain is absent.

  python3 tools/shiki_jit/test_encoder.py
"""
import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import gen_encoding as G  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DRIVER = r'''
#include "ksn_shiki_jit.h"
#include "ksn_shiki_jit_enc.h"
#include <stdio.h>
#include <stdlib.h>
int main(void){
    int form,o0,o1,o2;
    while(scanf("%d %d %d %d",&form,&o0,&o1,&o2)==4){
        uint8_t mem[8]; ksn_jit_buf b={mem,0,sizeof mem,false};
        ksn_jit_emit(&b,(unsigned)form,o0,o1,o2);
        for(size_t k=0;k<b.len;k++)printf("%02x",mem[k]);
        printf("\n");
    }
    return 0;
}
'''


def main():
    try:
        as_path = G.assembler(None)
    except SystemExit:
        print('test_encoder: SKIP (no xtensa-esp32s3-elf-as)')
        return
    with tempfile.TemporaryDirectory() as tmp:
        fresh = os.path.join(tmp, 'enc.h')
        G.emit_header(G.build(as_path), fresh)
        kept = os.path.join(ROOT, 'main/ui/kasane/ksn_shiki_jit_enc.h')
        if open(fresh).read() != open(kept).read():
            sys.exit('test_encoder: ksn_shiki_jit_enc.h is stale; run tools/shiki_jit/gen_encoding.py')
        drv = os.path.join(tmp, 'drv.c')
        open(drv, 'w').write(DRIVER)
        exe = os.path.join(tmp, 'drv')
        subprocess.run(['gcc', '-O1', '-Wall', '-Werror', '-I', os.path.join(ROOT, 'main/ui/kasane'), drv,
                        os.path.join(ROOT, 'main/ui/kasane/ksn_shiki_jit.c'), '-o', exe], check=True)
        rnd = random.Random(1)
        cases, lines = [], []
        for idx, (name, syn, ops) in enumerate(G.FORMS):
            for _ in range(64 if ops else 1):
                vals = [rnd.randint(lo, hi) for _, lo, hi, _ in ops]
                cases.append((idx, (vals + [0, 0, 0])[:3]))
                lines.append(syn.format(*[(v * sc if k == 'i' else v) for v, (k, _, _, sc) in zip(vals, ops)]))
        feed = ''.join(f'{i} {v[0]} {v[1]} {v[2]}\n' for i, v in cases)
        got = subprocess.run([exe], input=feed, capture_output=True, text=True, check=True).stdout.split()
        want = [b.hex() for b, _ in G.assemble_many(as_path, lines)]
    bad = [(l, g, w) for l, g, w in zip(lines, got, want) if g != w]
    for l, g, w in bad[:10]:
        print(f'MISMATCH {l}: encoder {g} assembler {w}')
    if bad or len(got) != len(want):
        sys.exit(f'test_encoder: {len(bad)} of {len(want)} differ')
    print(f'test_encoder: {len(want)} encodings, all the same as the assembler')


if __name__ == '__main__':
    main()
