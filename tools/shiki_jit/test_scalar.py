"""The scalar JIT (docs/kasane/shiki-jit.md sec.11) on the host.

1. test_scalar.c: random programs compiled and run on a small emulator of
   the emitted Xtensa instructions, against the interpreter.
2. The shipped programs (DERBY's and MEGADEMO's @shiki functions, as the
   firmware's built-in table): each compiled, disassembled with
   xtensa-esp32s3-elf-objdump, every byte from the entry decoded without a
   gap, every l32r's target (objdump's reading) inside the literal pool and
   where the emulator reads it. Prints each app's code size: what the
   budget (ksn_shiki_jit_budget) has to hold.

  python3 tools/shiki_jit/test_scalar.py [TRIALS]
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import gen_encoding as G  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
K = os.path.join(ROOT, 'main/ui/kasane')
DRIVER = r'''
#include "ksn_shiki.h"
#include "ksn_shiki_jit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv){
    (void)argc;
    FILE *bin=fopen(argv[1],"wb");
    unsigned count;
    const ksn_shiki_rom_entry *rom=ksn_shiki_rom_table(&count);
    ksn_shiki_plan *plan=calloc(1,KSN_SHIKI_PLAN_MAX);
    for(unsigned e=0;e<count;e++){
        const uint8_t *c=rom[e].code;
        const unsigned nin=c[7],narg=c[8],ndata=c[9];
        const uint8_t *t=c+12+nin+narg;
        /* Regions as the declarations ask, 64 records of zeros; arguments 1. */
        ksn_shiki_region reg[4];
        for(unsigned k=0;k<ndata;k++){
            reg[k]=(ksn_shiki_region){calloc((size_t)t[4*k+2]*64u,4),64,t[4*k],t[4*k+1],t[4*k+2]};
        }
        double args[8]={1,1,1,1,1,1,1,1};
        if(!ksn_shiki_register(c,rom[e].len,args,narg,reg,ndata,plan)){printf("%s REJECT\n",rom[e].name);continue;}
        static uint8_t mem[8192];
        ksn_jit_buf b={mem,0,sizeof mem,false};
        size_t entry=0;
        const bool ok=ksn_shiki_jit_scalar(plan,&b,&entry);
        while(ftell(bin)%16)fputc(0,bin);
        const long at=ftell(bin);
        fwrite(mem,1,b.len,bin);
        printf("%s ok=%d n=%u at=%ld entry=%zu len=%zu\n",rom[e].name,ok,plan->count,at,entry,b.len);
        for(unsigned k=0;k<ndata;k++)free(reg[k].data);
    }
    fclose(bin);
    return 0;
}
'''


def main():
    trials = sys.argv[1] if len(sys.argv) > 1 else '5000'
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'fuzz')
        subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', K,
                        os.path.join(K, 'ksn_shiki.c'), os.path.join(K, 'ksn_shiki_bench.c'),
                        os.path.join(K, 'ksn_shiki_jit.c'), os.path.join(ROOT, 'tools/shiki_jit/test_scalar.c'), '-lm', '-o', exe], check=True)
        r = subprocess.run([exe, trials], capture_output=True, text=True)
        print(r.stdout, end='')
        if r.returncode:
            print(r.stderr, end='')
            sys.exit('test_scalar: the emulated code differs from the interpreter')

        try:
            as_path = G.assembler(None)
        except SystemExit:
            print('test_scalar: shipped programs SKIP (no Xtensa toolchain)')
            return
        if not shutil.which('node'):
            print('test_scalar: shipped programs SKIP (no node)')
            return
        rom = os.path.join(tmp, 'rom.c')
        subprocess.run(['node', os.path.join(ROOT, 'tools/kasane_ir/emit_shiki_rom.mjs'), rom,
                        'derby=' + os.path.join(ROOT, 'apps/derby/derby_prog.js'),
                        'kasane=' + os.path.join(ROOT, 'apps/kasane/megademo_prog.js')],
                       check=True, stdout=subprocess.DEVNULL)
        drv = os.path.join(tmp, 'drv.c')
        open(drv, 'w').write(DRIVER)
        exe = os.path.join(tmp, 'drv')
        subprocess.run(['gcc', '-std=gnu11', '-O1', '-Wall', '-Werror', '-I', K, drv, rom,
                        os.path.join(K, 'ksn_shiki.c'), os.path.join(K, 'ksn_shiki_jit.c'), '-lm', '-o', exe],
                       check=True)
        binp = os.path.join(tmp, 'code.bin')
        report = subprocess.run([exe, binp], capture_output=True, text=True, check=True).stdout
        code = open(binp, 'rb').read()
        plans = []
        for line in report.splitlines():
            m = re.match(r'(\S+) ok=(\d) n=(\d+) at=(\d+) entry=(\d+) len=(\d+)', line)
            if not m:
                print(line)
                continue
            plans.append((m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6))))
        src = []
        for k, (_, ok, n, at, entry, ln) in enumerate(plans):
            src.append(f'.section .text.p{k}, "ax"')
            src += [f'.byte 0x{x:02x}' for x in code[at:at + ln]]
        asm, obj = os.path.join(tmp, 'code.S'), os.path.join(tmp, 'code.o')
        open(asm, 'w').write('\n'.join(src) + '\n')
        subprocess.run([as_path, asm, '-o', obj], check=True)
        objdump = as_path[:-2] + 'objdump'
        bad, size = [], {}
        for k, (name, ok, n, at, entry, ln) in enumerate(plans):
            if not ok:
                bad.append(f'{name}: did not compile')
                continue
            app = name.split('.')[0]
            size[app] = (size.get(app, (0, 0, 0))[0] + ln + 20, size.get(app, (0, 0, 0))[1] + n,
                         size.get(app, (0, 0, 0))[2] + 1)
            dis = subprocess.run([objdump, '-d', f'--section=.text.p{k}', f'--start-address={entry}', obj],
                                 capture_output=True, text=True, check=True).stdout
            pos = entry
            blob = code[at:at + ln]
            for line in dis.splitlines():
                m = re.match(r'\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(.*)', line)
                if not m:
                    continue
                addr, nb, text = int(m.group(1), 16), len(m.group(2)) // 2, m.group(3).strip()
                if addr != pos:
                    bad.append(f'{name}: gap at {addr}')
                pos = addr + nb
                if text.startswith('.byte') or 'unknown' in text or text.startswith('ill'):
                    bad.append(f'{name}: {text} at {addr}')
                if text.startswith('l32r'):
                    target = int(re.search(r',\s*0x([0-9a-f]+)', text).group(1), 16)
                    imm = blob[addr + 1] | blob[addr + 2] << 8
                    emu = ((addr + 3) & ~3) + 4 * imm - 262144      # test_scalar.c's reading
                    if target != emu or target % 4 or not 0 <= target < entry:
                        bad.append(f'{name}: l32r at {addr} reads {target} (emulator {emu}, pool 0..{entry})')
            if pos != ln:
                bad.append(f'{name}: decoded to {pos} of {ln} bytes')
    for app, (b, n, p) in sorted(size.items()):
        print(f'{app}: {p} programs, {n} instructions, {b} B of code with alignment ({b / n:.1f} B per instruction)')
    for b in bad:
        print('FAIL', b)
    if bad:
        sys.exit(1)
    print(f'test_scalar: {len(plans)} shipped programs compile and decode')


if __name__ == '__main__':
    main()
