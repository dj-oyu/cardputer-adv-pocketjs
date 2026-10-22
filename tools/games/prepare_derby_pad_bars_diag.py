#!/usr/bin/env python3
"""Reuse the pinned common pad diagnostic, with strict deadline validation."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

COMMON_HEAD='1e68b268a4b91ec54e88f408118de3e39b8b6bc5'

def deadline_guard(s):
    old='    if(diag_owner_active&&(derby_diag_finished()||!running||esp_timer_get_time()>=diag_owner_deadline)){'
    new='    const int64_t diag_now=esp_timer_get_time();\n    if(diag_owner_active&&(derby_diag_finished()||!running||diag_now>=diag_owner_deadline)){'
    assert s.count(old)==1
    s=s.replace(old,new)
    old='const char *status=!running?"app_error":!derby_diag_finished()?"timeout":'
    new='const char *status=!running?"app_error":diag_now>=diag_owner_deadline?"timeout":!derby_diag_finished()?"timeout":'
    assert s.count(old)==1
    return s.replace(old,new)

def test_owner(template):
    # Compile the actual patched owner callback against a small host clock stub.
    prefix='''#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
static bool running,done,pending;static int64_t now;static const char *status;
static int64_t esp_timer_get_time(void){return now;}
bool derby_diag_finished(void){return done;}
static bool pocket_kasane_needs_present(void){return pending;}
void derby_diag_finish(const char *s){status=s;}
#define begin_run(...) ((void)diag_prelude)
#define end_run(...) (running=false)
#define printf(...) ((void)0)
'''
    suffix='''
#undef printf
int main(void){
    const struct {bool run,done,pending;int64_t now;const char *want;} cases[]={
        {1,1,0,999,"complete"},{1,1,0,1000,"timeout"},
        {1,0,0,999,NULL},{1,0,0,1000,"timeout"},
        {0,1,0,1001,"app_error"},{1,1,1,999,"present_pending"}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
        running=cases[i].run;done=cases[i].done;pending=cases[i].pending;
        now=cases[i].now;status=NULL;diag_owner_ran=diag_owner_active=true;diag_owner_deadline=1000;
        diag_owner_step();
        if(cases[i].want?(!status||strcmp(status,cases[i].want)):status!=NULL)return 1;
    }
    puts("PAD_DIAGNOSTIC_OWNER PASS");return 0;
}
'''
    with tempfile.TemporaryDirectory() as temp:
        p=Path(temp);(p/'owner.c').write_text(prefix+deadline_guard(template)+suffix)
        subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(p/'owner.c'),'-o',str(p/'owner')],check=True)
        subprocess.run([str(p/'owner')],check=True)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--common',type=Path,required=True,help='prepare_derby_app_diag.py from the pinned checkout')
    p.add_argument('--checkout',type=Path)
    p.add_argument('--self-test',action='store_true');a=p.parse_args()
    common=a.common.resolve();root=common.parents[2]
    assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()==COMMON_HEAD
    if a.self_test:
        test_owner((common.parent/'device_diag/owner.inc').read_text());return
    assert a.checkout is not None
    spec=importlib.util.spec_from_file_location('derby_common_diag',common)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    dst=a.checkout.resolve();module.prepare(dst,'pad')
    target=dst/'main/main.c';target.write_text(deadline_guard(target.read_text()))
    patch=subprocess.check_output(['git','diff','--binary'],cwd=dst)
    (dst/'diagnostic.patch').write_bytes(patch)
    manifest=json.loads((dst/'diagnostic.json').read_text())
    manifest['common_generator']=COMMON_HEAD
    manifest['completion_guard']='deadline observed before success; identical for both modes'
    manifest['sha256']['main/main.c']=hashlib.sha256(target.read_bytes()).hexdigest()
    manifest['patch_sha256']=hashlib.sha256(patch).hexdigest()
    (dst/'diagnostic.json').write_text(json.dumps(manifest,indent=2)+'\n')

if __name__=='__main__':main()
