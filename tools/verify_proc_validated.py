"""Prove the linked experimental mode and exact source BIN/ELF pair."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import subprocess

def verify_bytes(cache,symbols,elf,image,description,head,mode):
    if mode not in ('OFF','REFERENCE','ON'):raise ValueError('invalid comparison mode')
    if not re.fullmatch(r'[0-9a-f]{40}',head):raise ValueError('invalid source head')
    options={'KSN_PROC_VALIDATED_PLAN':'OFF' if mode=='OFF' else 'ON',
             'KSN_PROC_VALIDATED_REFERENCE':'ON' if mode=='REFERENCE' else 'OFF'}
    for name,expected in options.items():
        if re.findall(rb'^'+name.encode()+rb':BOOL=(ON|OFF)\r?$',cache,re.M)!=[expected.encode()]:
            raise ValueError('missing, ambiguous or incorrect '+name)
    names={line.split()[-1] for line in symbols.splitlines() if line.split()}
    if ('ksn_proc_validated_begin' in names)!=(mode=='ON'):raise ValueError('linked execution path mismatch')
    if ('ksn_proc_validated_prepare' in names)!=(mode!='OFF'):raise ValueError('linked cache mode mismatch')
    digest=hashlib.sha256(elf).digest()
    if (not elf.startswith(b'\x7fELF') or len(image)<288 or image[0]!=0xe9 or
        struct.unpack_from('<I',image,32)[0]!=0xabcd5432 or image[176:208]!=digest or
        image[144:176].split(b'\0')[0]!=b'v6.0.1'):
        raise ValueError('IDF6.0.1 BIN/ELF pair mismatch')
    if image[48:80].split(b'\0')[0]!=head[:12].encode() or description['project_version']!=head[:12]:
        raise ValueError('embedded source version mismatch')
    return dict(mode=mode,source_head=head,elf_sha256=digest.hex(),bin_sha256=hashlib.sha256(image).hexdigest(),
                cmake_cache_sha256=hashlib.sha256(cache).hexdigest(),validated_execution=mode=='ON')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--head',required=True)
    parser.add_argument('--mode',choices=('OFF','REFERENCE','ON'),required=True)
    args=parser.parse_args();b=args.build
    symbols=subprocess.check_output(['xtensa-esp32s3-elf-nm','--defined-only',str(b/'cardputer_pocketjs.elf')],text=True)
    proof=verify_bytes((b/'CMakeCache.txt').read_bytes(),symbols,(b/'cardputer_pocketjs.elf').read_bytes(),
        (b/'cardputer_pocketjs.bin').read_bytes(),json.loads((b/'project_description.json').read_text()),args.head,args.mode)
    (b/'validated-plan-proof.json').write_text(json.dumps(proof,indent=2)+'\n')
    print('PROC_VALIDATED_FIRMWARE_PROOF '+json.dumps(proof))

if __name__=='__main__':main()
