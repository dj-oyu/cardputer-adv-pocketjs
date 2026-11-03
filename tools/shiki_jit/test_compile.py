"""The JIT's compiler (ksn_shiki_jit_section) on the bench programs, read back.

Builds a host driver that registers every bench and check case of
main/ui/kasane/ksn_shiki_bench.c, compiles its VEACH sections with the same C
as the device, and writes the code; then disassembles it with
xtensa-esp32s3-elf-objdump and checks what the host can check without
running it: every byte decodes (no unknown instruction), the zero-overhead
loop's body starts 16-aligned and ends where the compiler said, and the
cases that should compile do. Device equivalence is the cycle bench's
`CB shiki check ... jit=` (shiki-jit.md sec.4). --show prints the code.

  python3 tools/shiki_jit/test_compile.py [--show]
"""
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import gen_encoding as G  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DRIVER = r'''
#include "ksn_shiki.h"
#include "ksn_shiki_bench.h"
#include "ksn_shiki_jit.h"
#include <stdio.h>
#include <stdlib.h>
static void one(const ksn_shiki_bench_case *c,ksn_shiki_plan *plan,FILE *bin){
    if(!ksn_shiki_register(c->code,c->len,NULL,0,c->region,c->regions,plan)){printf("%s REJECT\n",c->name);return;}
    static uint8_t mem[4096];
    ksn_jit_buf b={mem,0,sizeof mem,false,false};
    ksn_jit_section s[KSN_JIT_SECTIONS];
    const unsigned n=ksn_shiki_jit_plan(plan,s,KSN_JIT_SECTIONS,&b);
    long at=ftell(bin);
    fwrite(mem,1,b.len,bin);
    printf("%s sections=%u at=%ld len=%zu",c->name,n,at,b.len);
    for(unsigned k=0;k<n;k++)printf(" [pc=%u off=%u len=%u steps=%u ptr=%u shift=%u const=%u acc=%d q7=%d]",
                                     s[k].pc,s[k].off,s[k].len,s[k].steps,s[k].nptr,s[k].nshift,s[k].nconst,s[k].acc,s[k].q7);
    printf("\n");
    /* 16 B apart, as sections are in the device buffer */
    while(ftell(bin)%16)fputc(0,bin);
}
int main(int argc,char **argv){
    FILE *bin=fopen(argv[1],"wb");
    static int16_t work[KSN_SHIKI_BENCH_WORK] __attribute__((aligned(16)));
    ksn_shiki_plan *plan=calloc(1,KSN_SHIKI_PLAN_MAX);
    ksn_shiki_bench_case c;
    for(unsigned i=0;i<ksn_shiki_bench_count();i++)if(ksn_shiki_bench_build(i,work,&c))one(&c,plan,bin);
    for(unsigned i=0;i<ksn_shiki_bench_check_count();i++)if(ksn_shiki_bench_check_build(i,work,&c))one(&c,plan,bin);
    fclose(bin);
    return 0;
}
'''


def main():
    show = '--show' in sys.argv
    try:
        as_path = G.assembler(None)
    except SystemExit:
        print('test_compile: SKIP (no Xtensa toolchain)')
        return
    objdump, objcopy = as_path[:-2] + 'objdump', as_path[:-2] + 'objcopy'
    with tempfile.TemporaryDirectory() as tmp:
        drv = os.path.join(tmp, 'drv.c')
        open(drv, 'w').write(DRIVER)
        exe = os.path.join(tmp, 'drv')
        k = os.path.join(ROOT, 'main/ui/kasane')
        subprocess.run(['gcc', '-std=gnu11', '-O1', '-Wall', '-Werror', '-I', k, drv, os.path.join(k, 'ksn_shiki.c'),
                        os.path.join(k, 'ksn_shiki_bench.c'), os.path.join(k, 'ksn_shiki_jit.c'), '-lm', '-o', exe],
                       check=True)
        binp = os.path.join(tmp, 'code.bin')
        report = subprocess.run([exe, binp], capture_output=True, text=True, check=True).stdout
        code = open(binp, 'rb').read()
        spans = []
        for line in report.splitlines():
            if 'at=' not in line:
                continue
            at = int(re.search(r'at=(\d+)', line).group(1))
            for off, ln in re.findall(r'off=(\d+) len=(\d+)', line):
                spans.append((line.split(' sections')[0], at + int(off), int(ln)))
        src = []
        for k, (_, a, n) in enumerate(spans):
            src.append(f'.section .text.s{k}, "ax"')
            src += [f'.byte 0x{x:02x}' for x in code[a:a + n]]
        asm = os.path.join(tmp, 'code.S')
        open(asm, 'w').write('\n'.join(src) + '\n')
        obj = os.path.join(tmp, 'code.o')
        subprocess.run([as_path, asm, '-o', obj], check=True)
        dis = subprocess.run([objdump, '-d', obj], capture_output=True, text=True, check=True).stdout
    print(report, end='')
    bad = []
    # Each section: its instructions in order, its loopgtz and the loop's end.
    per, cur = {}, None
    for line in dis.splitlines():
        m = re.match(r'Disassembly of section \.text\.s(\d+):', line)
        if m:
            cur = int(m.group(1))
            per[cur] = []
            continue
        m = re.match(r'\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(.*)', line)
        if m and cur is not None:
            per[cur].append((int(m.group(1), 16), len(m.group(2)) // 2, m.group(3).strip()))
    for k, (name, a, n) in enumerate(spans):
        insns = per.get(k, [])
        if show:
            print(f'--- {name} (section {k}, {n} B)')
        loop, addrs, pos = None, set(), 0
        for addr, ln, text in insns:
            if addr != pos:
                bad.append(f'{name}: gap at {addr}')
            pos = addr + ln
            addrs.add(addr)
            if show:
                print(f'  {addr:4d}: {text}')
            if text.startswith('.byte') or 'unknown' in text or text.startswith('ill'):
                bad.append(f'{name}: {text} at {addr}')
            if text.startswith('loopgtz'):
                loop = (addr, int(text.split(',')[-1].strip(), 16))
        if pos != n:
            bad.append(f'{name}: decoded {pos} of {n} bytes')
        if not loop:
            bad.append(f'{name}: no loopgtz')
        else:
            if (a + loop[0] + 3) % 16:
                bad.append(f'{name}: loop body at {loop[0] + 3}, not 16-aligned')
            if loop[1] not in addrs:
                bad.append(f'{name}: loop end {loop[1]} is not an instruction')
    sections = len(spans)
    if 'affine128' not in report or not re.search(r'affine128 sections=1', report):
        bad.append('affine128 did not compile')
    for b in bad:
        print('FAIL', b)
    if bad:
        sys.exit(1)
    print(f'test_compile: {sections} sections decode, loops aligned')


if __name__ == '__main__':
    main()
