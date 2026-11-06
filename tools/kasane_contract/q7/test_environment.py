"""Test raw register bookkeeping, not Xtensa instruction/silicon behavior.

Replace only the two inline-assembly blocks with independent register latches.
The model deliberately retains all written bits, so it reproduces the packed
FSR|FCR contamination that ordinary host fenv tests cannot detect. Production
get/set comparison and caller/numeric snapshots are compiled unchanged.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
source = (ROOT / 'main/ui/kasane/ksn_mesh_q7_environment.h').read_text()
read = '''__asm__ volatile ("rur.fcr %0\\n\\trur.fsr %1"
        : "=&a"(fcr), "=&a"(fsr) : : "memory");'''
write = '''__asm__ volatile ("wur.fcr %0\\n\\twur.fsr %1"
        : : "a"(fcr), "a"(fsr) : "memory");'''
assert source.count(read) == source.count(write) == 1
source = source.replace(read, 'fcr=model_fcr;fsr=model_fsr;')
source = source.replace(write, 'model_fcr=fcr^corrupt_fcr;model_fsr=fsr^corrupt_fsr;')
prefix = '''#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define __XTENSA__ 1
static uint32_t model_fcr,model_fsr,corrupt_fcr,corrupt_fsr;
'''
suffix = r'''
static void pair(uint32_t fcr,uint32_t fsr){
    model_fcr=fcr;model_fsr=fsr;
    ksn_q7_environment caller,numeric;
    assert(ksn_q7_environment_get(&caller)==0);
    assert(caller.fcr==fcr && caller.fsr==fsr);
    model_fcr=~fcr;model_fsr=~fsr;
    assert(ksn_q7_environment_get(&numeric)==0);
    assert(ksn_q7_environment_set(&caller)==0);
    assert(model_fcr==fcr && model_fsr==fsr);
    assert(ksn_q7_environment_set(&numeric)==0);
    assert(model_fcr==~fcr && model_fsr==~fsr);
    assert(ksn_q7_environment_set(&caller)==0);
}
int main(void){
    /* Exactly the observed distinct pair: packed get/set loses identity. */
    uint32_t old_fcr=0,old_fsr=128,packed=old_fcr|old_fsr;
    assert(packed!=old_fcr && packed==old_fsr);
    pair(old_fcr,old_fsr);
    /* Every rounding/enable/flag combination in the nominal low 12 bits. */
    for(uint32_t fcr=0;fcr<128;fcr++)
        for(uint32_t fsr=0;fsr<4096;fsr+=128)pair(fcr,fsr);
    /* Preserve arbitrary raw readback, not only architected field masks. */
    for(unsigned bit=0;bit<32;bit++){
        pair(UINT32_C(1)<<bit,0);pair(0,UINT32_C(1)<<bit);
        pair(UINT32_C(1)<<bit,~(UINT32_C(1)<<bit));
    }
    uint32_t random=12345;
    for(unsigned i=0;i<10000;i++){
        random=random*UINT32_C(1664525)+UINT32_C(1013904223);
        uint32_t fcr=random;
        random=random*UINT32_C(1664525)+UINT32_C(1013904223);
        pair(fcr,random);
    }
    ksn_q7_environment saved={0,128};
    for(unsigned bit=0;bit<32;bit++){
        corrupt_fcr=UINT32_C(1)<<bit;
        assert(ksn_q7_environment_set(&saved)==-1);
        corrupt_fcr=0;corrupt_fsr=UINT32_C(1)<<bit;
        assert(ksn_q7_environment_set(&saved)==-1);
        corrupt_fsr=0;
    }
    assert(ksn_q7_environment_set(&saved)==0);
    puts("Q7_ENVIRONMENT_MODEL_PASS independent full raw pair; packed contamination reproduced; every-bit restore corruption rejected; not silicon execution");
}
'''
with tempfile.TemporaryDirectory(prefix='q7-environment-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(prefix + source + suffix)
    binary = Path(directory) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall',
                    '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, env=dict(os.environ,
        ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1'))

# Exercise the real begin/step state machine with the same register-latch model.
# Native host floating math still runs under the host environment: this checks
# raw caller/numeric bookkeeping, not Xtensa FP execution or task preemption.
integration = r'''
#include "ksn_mesh_q7_preflight.h"
#include "ksn_mesh_q7_environment.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
uint32_t model_fcr,model_fsr,corrupt_fcr,corrupt_fsr;
extern bool __real_ksn_mesh_q7_render(const ksn_mesh_q7_snapshot *,uint16_t *,
    unsigned,unsigned,float *,uint8_t *,unsigned,ksn_mesh_q7_stats *);
static unsigned renders;
bool __wrap_ksn_mesh_q7_render(const ksn_mesh_q7_snapshot *snapshot,uint16_t *rgb,
    unsigned y,unsigned height,float *depth,uint8_t *owner,unsigned rows,
    ksn_mesh_q7_stats *stats){
    assert(model_fcr==0); /* Numeric stream, never this call's caller control. */
    if(renders)assert(model_fsr&256); /* Prior numeric status must persist. */
    model_fsr|=256;renders++;
    return __real_ksn_mesh_q7_render(snapshot,rgb,y,height,depth,owner,rows,stats);
}
static bool cancel(void *unused){
    (void)unused;
    assert(model_fcr==0 && model_fsr==128);
    model_fcr=3;model_fsr=2048;
    return false;
}
int main(void){
    size_t bytes=ksn_mesh_q7_preflight_bytes();
    void *memory=malloc(bytes);assert(memory);
    ksn_mesh_q7_preflight_report report;
    model_fcr=0;model_fsr=128;
    assert(ksn_mesh_q7_preflight_begin(memory,bytes,&report));
    assert(model_fcr==0 && model_fsr==128);
    for(unsigned step=0;step<3376;step++){
        uint32_t caller_fcr=(step%3)+1,caller_fsr=UINT32_C(1)<<(step%32);
        model_fcr=caller_fcr;model_fsr=caller_fsr;
        ksn_mesh_q7_preflight_step_result result=
            ksn_mesh_q7_preflight_step(memory,bytes,&report);
        assert(result==(step==3375?KSN_MESH_Q7_PREFLIGHT_DONE:KSN_MESH_Q7_PREFLIGHT_MORE));
        assert(model_fcr==caller_fcr && model_fsr==caller_fsr);
    }
    assert(renders==6752 && report.checkpoints==3376 && report.pixels==810240);
    assert(report.environment_restored && !report.invariant_failures && !report.mismatch_count);
    model_fcr=0;model_fsr=128;renders=0;
    assert(!ksn_mesh_q7_preflight_run_cooperative(memory,bytes,&report,cancel,NULL));
    assert(report.cancelled && report.checkpoints==1 && renders==2);
    assert(model_fcr==0 && model_fsr==128);
    free(memory);
    puts("Q7_ENVIRONMENT_INTEGRATION_MODEL_PASS full3376 steps; changing caller raw pair; numeric status persists; cancellation restores caller; host math not target simulation");
}
'''
with tempfile.TemporaryDirectory(prefix='q7-environment-integration-') as directory:
    directory = Path(directory)
    (directory / 'ksn_mesh_q7_environment.h').write_text(
        '#include <stdint.h>\nextern uint32_t model_fcr,model_fsr,corrupt_fcr,corrupt_fsr;\n' + source)
    (directory / 'preflight.c').write_text(
        (ROOT / 'main/ui/kasane/ksn_mesh_q7_preflight.c').read_text())
    (directory / 'test.c').write_text(integration)
    sources = ['main/ayatsuri/compact_opaque.c', 'main/ui/kasane/ksn_mesh_depth.c',
               'main/ui/kasane/ksn_shiki_frame.c', 'main/ui/kasane/ksn_mesh_prepared.c',
               'main/ui/kasane/ksn_mesh_q7_certified.c']
    binary = directory / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall',
        '-Wextra', '-Werror', '-D__XTENSA__=1', '-fno-fast-math', '-frounding-math',
        '-ffp-contract=off', '-fsanitize=address,undefined',
        '-I'+str(ROOT/'main'), '-I'+str(ROOT/'main/ui/kasane'),
        str(directory/'test.c'), str(directory/'preflight.c'),
        *[str(ROOT/item) for item in sources], '-Wl,--wrap=ksn_mesh_q7_render',
        '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, env=dict(os.environ,
        ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1'))
