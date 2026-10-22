#!/usr/bin/env python3
"""Reuse the pinned oval app diagnostic and strict owner deadline guard."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

COMMON_HEAD='1e68b268a4b91ec54e88f408118de3e39b8b6bc5'
GUARD_HEAD='146128065e81bb3493985e0b1b7123ff819baaf4'

def pinned_module(path,head,name):
    path=path.resolve()
    assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=path.parents[2],text=True).strip()==head
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--common',type=Path,required=True)
    p.add_argument('--guard',type=Path,required=True)
    p.add_argument('--checkout',type=Path)
    p.add_argument('--self-test',action='store_true');a=p.parse_args()
    common=pinned_module(a.common,COMMON_HEAD,'rank_common')
    guard=pinned_module(a.guard,GUARD_HEAD,'rank_guard')
    if a.self_test:
        guard.test_owner((a.common.parent/'device_diag/owner.inc').read_text());return
    assert a.checkout is not None
    dst=a.checkout.resolve();common.prepare(dst,'oval')
    target=dst/'main/main.c';target.write_text(guard.deadline_guard(target.read_text()))
    patch=subprocess.check_output(['git','diff','--binary'],cwd=dst)
    (dst/'diagnostic.patch').write_bytes(patch)
    manifest=json.loads((dst/'diagnostic.json').read_text())
    manifest['common_generator']=COMMON_HEAD
    manifest['deadline_guard_generator']=GUARD_HEAD
    manifest['completion_guard']='deadline observed before success; identical for both modes'
    manifest['sha256']['main/main.c']=hashlib.sha256(target.read_bytes()).hexdigest()
    manifest['patch_sha256']=hashlib.sha256(patch).hexdigest()
    (dst/'diagnostic.json').write_text(json.dumps(manifest,indent=2)+'\n')

if __name__=='__main__':main()
