"""Synthetic C driver bytecode versus the bounded Python reference."""
import ctypes as c, pathlib, random, subprocess, tempfile, unittest
from driver_bytecode import compile_expression
from driver_subset import evaluate


class Instruction(c.Structure):
    _fields_=[('op',c.c_uint8),('arg',c.c_uint16),('value',c.c_double)]


class TestDriverBytecode(unittest.TestCase):
    def test_reference_and_rejections(self):
        here=pathlib.Path(__file__).parent
        with tempfile.TemporaryDirectory() as tmp:
            d=pathlib.Path(tmp);(d/'driver_bytecode_candidate.h').write_text((here/'driver_bytecode_candidate.h').read_text())
            (d/'driver.c').write_text('#include "driver_bytecode_candidate.h"\nint run(const hone_driver_instruction*c,size_t n,const double*v,size_t m,double*o){return hone_driver_run(c,n,v,m,o);}\n')
            subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-ffp-contract=off',str(d/'driver.c'),'-lm','-o',str(d/'driver.so')],check=True)
            run=c.CDLL(str(d/'driver.so')).run
            run.argtypes=[c.POINTER(Instruction),c.c_size_t,c.POINTER(c.c_double),c.c_size_t,c.POINTER(c.c_double)]
            expressions=['a+b','max(0,min(1,a+b))','1 if a==1 else b',
                         'max(a,b)>=0.75','a**2+b**3-a**4','-a if a>=b else +b']
            rng=random.Random(57)
            for expression in expressions:
                program=compile_expression(expression,['a','b']);code=(Instruction*len(program))(*(Instruction(*row) for row in program))
                for a,b in [(0.,0.),(1.,1.),(-1.,2.)]+[(rng.uniform(-2,2),rng.uniform(-2,2)) for _ in range(1000)]:
                    variables=(c.c_double*2)(a,b);out=c.c_double(42)
                    self.assertEqual(run(code,len(code),variables,2,c.byref(out)),1)
                    self.assertAlmostEqual(out.value,evaluate(expression,{'a':a,'b':b}),places=13)
                variables=(c.c_double*2)(float('nan'),0);out=c.c_double(42)
                self.assertEqual(run(code,len(code),variables,2,c.byref(out)),0);self.assertEqual(out.value,42)
            for expression in ('a**b','a**-1','a/2','a.attr','a**5'):
                with self.assertRaises(ValueError):compile_expression(expression,['a','b'])
            nested='1'
            for _ in range(32):nested='1+('+nested+')'
            with self.assertRaises(ValueError):compile_expression(nested,[])
            for expression in ('min(1,a*a)','1 if a*a>=0 else 2'):
                with self.assertRaises(ValueError):evaluate(expression,{'a':1e308})
                program=compile_expression(expression,['a']);code=(Instruction*len(program))(*(Instruction(*row) for row in program))
                variables=(c.c_double*1)(1e308);out=c.c_double(42)
                self.assertEqual(run(code,len(code),variables,1,c.byref(out)),0);self.assertEqual(out.value,42)
            expression='1 if a>=0 else a*a';program=compile_expression(expression,['a']);code=(Instruction*len(program))(*(Instruction(*row) for row in program))
            variables=(c.c_double*1)(1e308);out=c.c_double(42)
            self.assertEqual(run(code,len(code),variables,1,c.byref(out)),1);self.assertEqual(out.value,1)
            bad=(Instruction*2)(Instruction(13,0,0),Instruction(14,0,0));out=c.c_double(42)
            self.assertEqual(run(bad,2,None,0,c.byref(out)),0);self.assertEqual(out.value,42)

if __name__=='__main__': unittest.main()
