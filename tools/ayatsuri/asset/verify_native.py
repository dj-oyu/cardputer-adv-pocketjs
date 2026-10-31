"""Minimal real-C AYNM reader/arena probe; no renderer, QuickJS, SDK or device."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
# Including the real implementation exposes its private node-state sizeof to
# this measurement-only translation unit; the production API is unchanged.
PROBE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ayatsuri/ayatsuri_model.c"
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    FILE *file=fopen(argv[1],"rb");if(!file)return 3;
    if(fseek(file,0,SEEK_END)){fclose(file);return 3;}
    long length=ftell(file);if(length<0||length>AYATSURI_MODEL_MAX_BYTES){fclose(file);return 3;}
    rewind(file);size_t n=(size_t)length;
    uint8_t *data=malloc(n?n:1),*copy=malloc(n?n:1);
    if(!data||!copy){fclose(file);free(data);free(copy);return 4;}
    if(fread(data,1,n,file)!=n){fclose(file);free(data);free(copy);return 3;}
    fclose(file);memcpy(copy,data,n);
    ayatsuri_model model;
    ayatsuri_model_status status=ayatsuri_model_open(data,n,&model);
    if(status!=AYATSURI_MODEL_OK){
        printf("{\"valid\":false,\"readerStatus\":%d,\"coldBytes\":%zu}\n",(int)status,n);
        free(copy);free(data);return 1;
    }
    size_t truncated=0;int atomic=1;
    for(size_t k=0;k<n;k++){
        ayatsuri_model out,sentinel;memset(&sentinel,0xa5,sizeof sentinel);out=sentinel;
        if(ayatsuri_model_open(data,k,&out)==AYATSURI_MODEL_OK)break;
        if(memcmp(&out,&sentinel,sizeof out))atomic=0;
        truncated++;
    }
    copy[0]^=0xff;
    ayatsuri_model out,sentinel;memset(&sentinel,0xa5,sizeof sentinel);out=sentinel;
    int bad_magic=ayatsuri_model_open(copy,n,&out)!=AYATSURI_MODEL_OK&&!memcmp(&out,&sentinel,sizeof out);
    memcpy(copy,data,n);ayatsuri_model reopened;
    int roundtrip=ayatsuri_model_open(copy,n,&reopened)==AYATSURI_MODEL_OK;
    size_t arena_bytes=ayatsuri_model_instance_bytes(&model);
    void *allocation=malloc(arena_bytes+7u);
    if(!allocation){free(copy);free(data);return 4;}
    void *arena=(void *)(((uintptr_t)allocation+7u)&~(uintptr_t)7u);
    ayatsuri_model_instance instance;int owner=0;
    int initialized=ayatsuri_model_instance_init(&model,arena,arena_bytes,&owner,1,&instance)==AYATSURI_MODEL_OK;
    unsigned kinds[4]={0};int poses=initialized;
    if(initialized)for(uint16_t id=0;id<model.node_count;id++){
        uint8_t kind=255;float pose[9];
        if(ayatsuri_model_instance_node_kind(&instance,&owner,1,id,&kind)!=AYATSURI_MODEL_OK||kind>3){poses=0;break;}
        kinds[kind]++;
        if(ayatsuri_model_instance_read_pose(&instance,&owner,1,id,pose)!=AYATSURI_MODEL_OK){poses=0;break;}
        for(unsigned k=0;k<9;k++)if(pose[k]!=mf(model.data+model.section[1].offset+id*36u+k*4u))poses=0;
    }
    int disposed=initialized&&ayatsuri_model_instance_dispose(&instance,&owner,1)==AYATSURI_MODEL_OK;
    roundtrip=roundtrip&&reopened.node_count==model.node_count&&reopened.packet_count==model.packet_count&&
        reopened.slot_bytes==model.slot_bytes&&ayatsuri_model_instance_bytes(&reopened)==arena_bytes&&!memcmp(copy,data,n);
    int passed=roundtrip&&truncated==n&&atomic&&bad_magic&&poses&&disposed;
    printf("{\"valid\":true,\"passed\":%s,\"readerStatus\":0,\"pointerBits\":%zu,"
        "\"coldBytes\":%zu,\"nodes\":%u,\"packetSlots\":%u,\"workSlotBytes\":%u,\"arenaBytes\":%zu,"
        "\"sizeofModel\":%zu,\"sizeofInstance\":%zu,\"sizeofNodeState\":%zu,"
        "\"nodeKinds\":{\"group\":%u,\"ellipsoid\":%u,\"blend\":%u,\"mesh\":%u},"
        "\"truncatedPrefixesRejected\":%zu,\"rejectionOutputAtomic\":%s,\"invalidMagicRejected\":%s,"
        "\"coldCopyReopenExact\":%s,\"instanceInitAndRestPoseExact\":%s,\"instanceDispose\":%s,"
        "\"renderer\":\"NOT TESTED\",\"deviceMemory\":\"NOT MEASURED\"}\n",
        passed?"true":"false",sizeof(void*)*8,n,(unsigned)model.node_count,(unsigned)model.packet_count,
        (unsigned)model.slot_bytes,arena_bytes,sizeof model,sizeof instance,sizeof(model_node_state),
        kinds[0],kinds[1],kinds[2],kinds[3],truncated,atomic?"true":"false",bad_magic?"true":"false",
        roundtrip?"true":"false",poses?"true":"false",disposed?"true":"false");
    free(allocation);free(copy);free(data);return passed?0:1;
}
'''


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, help='compiled .aynm cold asset')
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--m32', action='store_true', help='requires caller-provided working multilib compiler; no helper setup')
    parser.add_argument('--asan', action='store_true')
    parser.add_argument('--output', type=Path, help='also save the JSON report')
    args = parser.parse_args()
    asset = args.input.resolve()
    report = {'kind': 'REAL C READER / ARENA PROBE / NO RENDERER OR DEVICE', 'input': str(asset)}
    exit_code = 1
    try:
        if not asset.is_file():
            raise RuntimeError('input is not a readable file')
        report['inputSha256'] = sha(asset)
        cc = shutil.which(args.cc)
        if not cc:
            raise RuntimeError(f'compiler not found: {args.cc}; run on Linux/WSL with gcc')
        # Exact small implementation closure; no broad repository freeze/guard.
        names = ('main/ayatsuri/ayatsuri_model.c', 'main/ayatsuri/ayatsuri_model.h',
                 'main/ayatsuri/ayatsuri_transform.h', 'main/ayatsuri/ayatsuri_shade.h',
                 'main/ayatsuri/ayatsuri_visibility.c', 'main/ayatsuri/ayatsuri_visibility.h',
                 'main/ui/kasane/ksn_procedural.h')
        before = {n: sha(ROOT / n) for n in names}
        flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-ffp-contract=off', '-fexcess-precision=standard']
        if args.m32:
            flags += ['-m32', '-malign-double', '-msse2', '-mfpmath=sse']
            sysroot = os.environ.get('M32_SYSROOT')
            if sysroot:
                base = Path(sysroot).resolve()
                version = subprocess.check_output([cc, '-dumpversion'], text=True).strip().split('.')[0]
                gcc32 = base / f'usr/lib/gcc/x86_64-linux-gnu/{version}/32'
                lib32 = base / 'usr/lib32'
                if not (base / '.ok').is_file() or not (lib32 / 'ld-linux.so.2').is_file():
                    raise RuntimeError('M32_SYSROOT is not an already prepared i386 sysroot; setup is never invoked')
                flags += [f'-B{lib32}', f'-B{gcc32}', f'-L{lib32}', f'-L{gcc32}',
                          '-isystem', str(base / 'usr/include/x86_64-linux-gnu'),
                          '-isystem', '/usr/include/x86_64-linux-gnu',
                          f'-Wl,--dynamic-linker={lib32}/ld-linux.so.2', f'-Wl,-rpath,{lib32}',
                          '-Wl,--disable-new-dtags']
        if args.asan:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        with tempfile.TemporaryDirectory(prefix='aynm-native-probe-') as temp:
            folder = Path(temp)
            source, binary = folder / 'probe.c', folder / 'probe'
            source.write_text(PROBE)
            command = [cc, *flags, '-I', str(ROOT / 'main'), str(source),
                       str(ROOT / 'main/ayatsuri/ayatsuri_visibility.c'), '-lm', '-o', str(binary)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if built.returncode:
                raise RuntimeError('native probe compilation failed: ' + built.stderr.strip())
            run = subprocess.run([str(binary), str(asset)], capture_output=True, text=True, timeout=120)
            try:
                report['native'] = json.loads(run.stdout)
            except json.JSONDecodeError as error:
                raise RuntimeError(f'native probe returned {run.returncode} without JSON: {run.stderr.strip()}') from error
            report.update(compiler=str(cc), compilerVersion=subprocess.check_output([cc, '--version'], text=True).splitlines()[0],
                          flags=flags, sourceSha256=before, probeSha256=hashlib.sha256(PROBE.encode()).hexdigest())
            if run.stderr.strip():
                report['nativeStderr'] = run.stderr.strip()
            report['sourcesUnchanged'] = before == {n: sha(ROOT / n) for n in names}
            report['inputUnchanged'] = report['inputSha256'] == sha(asset)
            report['passed'] = run.returncode == 0 and report['sourcesUnchanged'] and report['inputUnchanged']
            exit_code = 0 if report['passed'] else 1
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        report.update(passed=False, error=str(error))
    text = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    sys.stdout.write(text)
    return exit_code


if __name__ == '__main__':
    raise SystemExit(main())
