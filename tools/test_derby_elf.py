"""Use real host ELF objects and paired-image negatives to test mode evidence."""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from derby_elf import object_symbol
from verify_derby_runner_batch import ROOT, verify


class ElfEvidence(unittest.TestCase):
    def test_source_scoped_state_and_stored_layout(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'pocket_derby.c';sizes=[]
            for enabled in (False,True):
                source.write_text('#include <stdint.h>\nstatic volatile struct {double pc[10];\n'
                    '#ifdef CLIP\n double clip_norm,clip_f;\n#endif\n uint32_t plans[2];_Bool set;} view;\n'
                    'const uint32_t layout_size=sizeof(view);\nint main(void){view.pc[0]++;return (int)view.pc[0];}\n')
                image=root/'test-elf';flags=['-DCLIP'] if enabled else []
                subprocess.run(['gcc','-O2',*flags,str(source),'-o',str(image)],check=True)
                elf=image.read_bytes();size,_=object_symbol(elf,'view','pocket_derby.c')
                n,data=object_symbol(elf,'layout_size');self.assertEqual(n,4)
                self.assertEqual(size,struct.unpack('<I',data)[0]);sizes.append(size)
                with self.assertRaises(ValueError):object_symbol(elf,'view','other.c')
                with self.assertRaises(ValueError):object_symbol(elf,'absent')
                with self.assertRaises(ValueError):object_symbol(elf[:16],'view')
            self.assertEqual(sizes[1]-sizes[0],16)

    def test_embedded_head_and_description_must_agree(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);head='1'*40
            source=(ROOT/'apps/derby/derby_pan.js').read_bytes();elf=b'example ELF'+source+b'\0'
            (root/'cardputer_pocketjs.elf').write_bytes(elf)
            (root/'CMakeCache.txt').write_text('POCKET_DERBY_RUNNER_BATCH:BOOL=OFF\n')
            (root/'project_description.json').write_text(json.dumps({'project_version':head[:12]}))
            image=bytearray(288);image[0]=0xe9;struct.pack_into('<I',image,32,0xabcd5432)
            image[48:60]=head[:12].encode();image[144:150]=b'v6.0.1';image[176:208]=hashlib.sha256(elf).digest()
            (root/'cardputer_pocketjs.bin').write_bytes(image)
            with patch('verify_derby_runner_batch.subprocess.check_output',return_value=''):
                self.assertEqual(verify(root,'OFF',head=head)['source_head'],head)
                with self.assertRaisesRegex(ValueError,'embedded project version'):verify(root,'OFF',head='2'*40)
                (root/'project_description.json').write_text(json.dumps({'project_version':'other'}))
                with self.assertRaisesRegex(ValueError,'build project version'):verify(root,'OFF',head=head)


if __name__=='__main__':unittest.main()
