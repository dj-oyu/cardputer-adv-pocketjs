"""Compile the real seed provider in comparison and normal modes."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'main/pocket/random_seed.c'
sys.path.insert(0, str(ROOT / 'tools/games'))
from derby_seeds import race_seed
from elf_symbol_value import symbol_u32
TEST = r'''
#include <assert.h>
#include <stdint.h>
#include "random_stream.h"
static unsigned calls;
uint32_t esp_random(void) { return UINT32_C(0x12345670) + ++calls; }
int main(void) {
#ifdef POCKET_DERBY_COMPARE_FIXED_HW
    assert(pocket_random_seed() == UINT32_C(0x2545F491));
    assert(pocket_random_seed() == UINT32_C(0x2545F491));
    assert(calls == 0);
#else
    assert(pocket_random_seed() == UINT32_C(0x12345671));
    assert(pocket_random_seed() == UINT32_C(0x12345672));
    assert(calls == 2);
#endif
}
'''

with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    (work / 'esp_random.h').write_text('#include <stdint.h>\nuint32_t esp_random(void);\n')
    (work / 'test.c').write_text(TEST)
    for fixed in (False, True):
        exe = work / ('fixed' if fixed else 'normal')
        command = ['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-DESP_PLATFORM', '-I', str(work), '-I', str(ROOT / 'main/pocket')]
        if fixed:
            command.append('-DPOCKET_DERBY_COMPARE_FIXED_HW=1')
        subprocess.run([*command, str(SOURCE), str(work / 'test.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        if fixed:
            assert symbol_u32(exe.read_bytes(), 'pocket_derby_comparison_hw_seed') == 0x2545F491
        else:
            try:
                symbol_u32(exe.read_bytes(), 'pocket_derby_comparison_hw_seed')
            except ValueError:
                pass
            else:
                raise AssertionError('normal ELF unexpectedly contains diagnostic seed')

assert race_seed(0x2545F491, 19, 1000001) == 0x9112A7AB
print('PASS normal hardware seed and diagnostic fixed seed are isolated')
