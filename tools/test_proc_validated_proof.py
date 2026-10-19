import hashlib
import struct
import unittest
from verify_proc_validated import verify_bytes

class Proof(unittest.TestCase):
    def fixture(self,mode):
        head='ab'*20;elf=b'\x7fELF source fixture';image=bytearray(288);image[0]=0xe9
        struct.pack_into('<I',image,32,0xabcd5432);image[48:60]=head[:12].encode()
        image[144:151]=b'v6.0.1\0';image[176:208]=hashlib.sha256(elf).digest()
        cache=('KSN_PROC_VALIDATED_PLAN:BOOL='+('OFF' if mode=='OFF' else 'ON')+'\nKSN_PROC_VALIDATED_REFERENCE:BOOL='+('ON' if mode=='REFERENCE' else 'OFF')+'\n').encode()
        symbols=('0 T ksn_proc_validated_prepare\n' if mode!='OFF' else '')+('1 T ksn_proc_validated_begin\n' if mode=='ON' else '')
        return [cache,symbols,elf,image,{'project_version':head[:12]},head,mode]
    def test_all_modes_and_rejections(self):
        for mode in ('OFF','REFERENCE','ON'):
            args=self.fixture(mode);self.assertEqual(verify_bytes(*args)['mode'],mode)
            bad=args.copy();bad[0]+=bad[0]
            with self.assertRaises(ValueError):verify_bytes(*bad)
            bad=args.copy();bad[2]+=b'wrong ELF'
            with self.assertRaises(ValueError):verify_bytes(*bad)
            bad=args.copy();bad[5]='cd'*20
            with self.assertRaises(ValueError):verify_bytes(*bad)
            bad=args.copy()
            bad[1]='' if mode=='ON' else bad[1]+'\n1 T ksn_proc_validated_begin'
            with self.assertRaises(ValueError):verify_bytes(*bad)

if __name__=='__main__':unittest.main()
