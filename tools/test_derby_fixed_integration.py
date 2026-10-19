"""Archive-safe fixture proof, OFF-path and generated composition checks."""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
import unittest
import sys
import shutil
from unittest.mock import patch
from derby_fixed_workload_chunks import generate,MANIFEST_SHA256
from record_verification import verify_fixed_mode,verify_mode,FIXED_SYMBOLS,TELEMETRY_SYMBOLS

ROOT=Path(__file__).resolve().parents[1]
BASE=json.loads((ROOT/'tools/derby_fixed_base.json').read_text())['base_tree']

def off_path(source):
    out=[];stack=[]
    def keep():return all(k!='fixed' or v for k,v in stack)
    for line in source.splitlines():
        p=line.strip()
        if p=='#include "pocket/derby_fixed_workload.h"':continue
        if p.startswith(('#if ','#ifdef ','#ifndef ')):
            if p=='#ifdef POCKET_DERBY_FIXED_WORKLOAD':stack.append(('fixed',False));continue
            if keep():out.append(line)
            stack.append(('other',True));continue
        if p=='#else' or p.startswith('#elif '):
            if stack and stack[-1][0]=='fixed':stack[-1]=('fixed',not stack[-1][1]);continue
            if keep():out.append(line)
            continue
        if p=='#endif' or p.startswith('#endif '):
            kind,_=stack.pop()
            if kind!='fixed' and keep():out.append(line)
            continue
        if keep() and not p.startswith(('DF_OWNER_','DF_WORK_END(','DF_PACE_')):out.append(line)
    if stack:raise ValueError('unbalanced source preprocessor')
    return '\n'.join(x.strip() for x in out if x.strip())

class Integration(unittest.TestCase):
    def test_normal_owner_and_app_paths_identical(self):
        for name in ('main/main.c','main/app_session.c'):
            old=subprocess.check_output(['git','show',BASE+':'+name],cwd=ROOT,text=True)
            self.assertEqual(off_path(old),off_path((ROOT/name).read_text()),name)

    def test_actual_derby_off_text_identical_at_O0(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp);old=out/'original.c'
            old.write_bytes(subprocess.check_output(['git','show',BASE+':main/pocket/pocket_derby.c'],cwd=ROOT))
            headers=['components/quickjs-ng/quickjs-ng','components/pocketjs_guest/include',
                     'tools/hostshim','main','main/pocket','main/ui','main/ui/kasane','main/text','main/hal']
            blobs=[]
            for i,source in enumerate((old,ROOT/'main/pocket/pocket_derby.c')):
                obj=out/f'{i}.o';raw=out/f'{i}.text'
                subprocess.run(['gcc','-std=gnu11','-O0','-DQUICKJS_NG_BUILD','-D_GNU_SOURCE',
                    *['-I'+str(ROOT/h) for h in headers],'-c',str(source),'-o',str(obj)],check=True)
                self.assertNotIn('df_',subprocess.check_output(['nm',str(obj)],text=True))
                subprocess.run(['objcopy','--only-section=.text','-O','binary',str(obj),str(raw)],check=True)
                blobs.append(raw.read_bytes())
            self.assertEqual(blobs[0],blobs[1],'OFF native machine-code text changed')

    def test_generated_bytes_and_mode_proof_negative_cases(self):
        with tempfile.TemporaryDirectory() as tmp:
            build=Path(tmp);out=build/'generated/derby_fixed_workload';generate(ROOT/'apps/derby',out)
            cache=build/'CMakeCache.txt';cache.write_text('POCKET_DERBY_FIXED_WORKLOAD:BOOL=ON\nPOCKET_DERBY_TELEMETRY:BOOL=ON\n')
            chunks=[(out/name).read_bytes() for name in ('derby_watch.js','derby_view.js','derby_play.js')]
            elf=b'fixture'+b'\0'.join(chunks)+b'\0'+MANIFEST_SHA256.encode()+b'\0'
            path=build/'cardputer_pocketjs.elf';path.write_bytes(elf)
            with patch('record_verification.elf_symbols',return_value=FIXED_SYMBOLS):
                self.assertEqual(verify_fixed_mode(build)['mode'],'ON')
                path.write_bytes(elf+(ROOT/'apps/derby/derby_play.js').read_bytes()+b'\0')
                with self.assertRaises(ValueError):verify_fixed_mode(build)
                path.write_bytes(elf)
                p=out/'derby_view.js';good=p.read_bytes();p.write_bytes(b'wrong')
                with self.assertRaises(ValueError):verify_fixed_mode(build)
                p.write_bytes(good)
            with patch('record_verification.elf_symbols',return_value=FIXED_SYMBOLS-{'df_owner_end'}),self.assertRaises(ValueError):
                verify_fixed_mode(build)
            with patch('record_verification.elf_symbols',return_value=FIXED_SYMBOLS-{'df_measuring','df_fail'}):
                self.assertEqual(verify_fixed_mode(build)['mode'],'ON')
            with patch('record_verification.elf_symbols',return_value=TELEMETRY_SYMBOLS):
                self.assertEqual(verify_mode(build,'ON')['embedded_play_sha256'],hashlib.sha256(chunks[2]).hexdigest())
            cache.write_text('POCKET_DERBY_FIXED_WORKLOAD:BOOL=OFF\n')
            with patch('record_verification.elf_symbols',return_value=FIXED_SYMBOLS),self.assertRaises(ValueError):verify_fixed_mode(build)
            path.write_bytes(b'normal ELF')
            with patch('record_verification.elf_symbols',return_value=set()):self.assertEqual(verify_fixed_mode(build)['mode'],'OFF')
            cache.write_text('POCKET_DERBY_FIXED_WORKLOAD:BOOL=ON\nPOCKET_DERBY_FIXED_WORKLOAD:BOOL=OFF\n')
            with self.assertRaises(ValueError):verify_fixed_mode(build)

    def test_default_off_and_batch_composition(self):
        self.assertIn('option(POCKET_DERBY_FIXED_WORKLOAD "Finite fixed DERBY workload diagnostic (not production)" OFF)',(ROOT/'main/CMakeLists.txt').read_text())
        self.assertNotIn('set(POCKET_DERBY_FIXED_WORKLOAD ON',(ROOT/'CMakeLists.txt').read_text())
        if not (ROOT/'tools/derby_runner_batch_chunks.py').exists():return
        from derby_runner_batch_chunks import generate as batch
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp);generate(ROOT/'apps/derby',out/'fixed');batch(out/'fixed',out/'batch')
            lines=(out/'batch/chunks.txt').read_text().splitlines()
            pairs=dict(line.split() for line in lines[1:])
            self.assertEqual(Path(pairs['view']).read_bytes(),(out/'fixed/derby_view.js').read_bytes())
            self.assertEqual(Path(pairs['play']).read_bytes(),(out/'fixed/derby_play.js').read_bytes())
            self.assertIn('S.runners(',Path(pairs['pan']).read_text())

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'),'CMake/Ninja unavailable; target configuration still required')
    def test_real_cmake_chunk_selection_fragment(self):
        text=(ROOT/'main/CMakeLists.txt').read_text()
        start=text.index('if(POCKET_DERBY_TELEMETRY)\n    if(DERBY_BGCOST_SOURCE')
        end=text.index('execute_process(COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/make_app_chunks.py"',start)
        early_start=text.index('if(POCKET_DERBY_FIXED_WORKLOAD)\n    if(NOT POCKET_DERBY_COMPARE_FIXED_HW')
        early_end=text.index('option(KASANE_PROC_LIMITS_PROBE',early_start)
        chunks_end=text.index('include("${CMAKE_BINARY_DIR}/generated/app_chunks.cmake")',end)
        def source(fragment):
            return fragment.replace('${CMAKE_CURRENT_LIST_DIR}',(ROOT/'main').as_posix()).replace('${CMAKE_SOURCE_DIR}',ROOT.as_posix())
        fragment=source(text[start:end]);early=source(text[early_start:early_end]);chunks=source(text[end:chunks_end])
        batch=(ROOT/'tools/derby_runner_batch_chunks.py').exists()
        for enabled in (False,True):
            with tempfile.TemporaryDirectory() as tmp:
                out=Path(tmp);build=out/'build';fixed=build/'generated/derby_fixed_workload'
                script=out/'CMakeLists.txt'
                script.write_text('cmake_minimum_required(VERSION 3.22)\n'
                    +'project(derby_fixture_selection NONE)\n'
                    +f'set(PYTHON "{sys.executable}")\n'
                    +'set(POCKET_DERBY_TELEMETRY ON)\nset(POCKET_DERBY_COMPARE_FIXED_HW ON)\n'
                    +f'set(POCKET_DERBY_FIXED_WORKLOAD {"ON" if enabled else "OFF"})\n'
                    +f'set(POCKET_DERBY_RUNNER_BATCH {"ON" if batch else "OFF"})\n'
                    +early
                    +f'file(GLOB _app_chunk_lists CONFIGURE_DEPENDS "{(ROOT/"main").as_posix()}/../apps/derby/chunks.txt")\n'
                    +fragment+chunks
                    +f'file(WRITE "{(out/"selected.txt").as_posix()}" "${{_app_chunk_lists}}")\n')
                subprocess.run(['cmake','-G','Ninja','-S',str(out),'-B',str(build)],check=True,capture_output=True)
                subprocess.run(['cmake','--build',str(build)],check=True,capture_output=True)
                selected=(out/'selected.txt').read_text().split(';')
                self.assertEqual(len(selected),1,'duplicate normal/telemetry/fixed app list')
                pairs=dict(line.split() for line in Path(selected[0]).read_text().splitlines()[1:])
                if enabled:
                    for key in ('view','play'):self.assertEqual(Path(pairs[key]).read_bytes(),(fixed/f'derby_{key}.js').read_bytes())
                else:self.assertNotIn('derby_fixed_workload',pairs['play'])
                if batch:self.assertIn('S.runners(',Path(pairs['pan']).read_text())

if __name__=='__main__':unittest.main()
