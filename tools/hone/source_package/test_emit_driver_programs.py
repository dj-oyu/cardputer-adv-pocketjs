import copy, json, pathlib, subprocess, tempfile, unittest
from emit_driver_programs import build,emit


def fixture():
    return dict(schema='hone.driver-inventory.v1',runtime_ready=False,source_sha256='a'*64,drivers=[dict(
        type='SCRIPTED',mute=False,curve_valid=True,driver_valid=True,keyframe_count=0,fcurve_modifiers=[],expression='x**2 if x>=0 else -x',owner={'type':'Object','name':'synthetic'},
        data_path='location',array_index=0,variables=[dict(name='x',type='SINGLE_PROP',targets=[dict(id={'type':'Object','name':'control'},data_path='["x"]',current={'status':'ok','value':.5})])])])


class TestEmitDrivers(unittest.TestCase):
    def test_compiled_program_and_no_overwrite(self):
        here=pathlib.Path(__file__).parent
        with tempfile.TemporaryDirectory() as td:
            p=pathlib.Path(td);source=p/'in.json';data=fixture()
            other=copy.deepcopy(data['drivers'][0]);other['expression']='x';other['array_index']=1
            data['drivers'].insert(0,other);source.write_text(json.dumps(data));out=p/'generated';r=build(source,out)
            self.assertEqual(r['programs'],2);self.assertFalse(r['runtime_ready'])
            with self.assertRaises(FileExistsError):build(source,out)
            (out/'driver_bytecode_candidate.h').write_text((here/'driver_bytecode_candidate.h').read_text())
            (out/'test.c').write_text('''#include "driver_programs.private.h"
#include <assert.h>
int main(void){double v=-2,out[2]={0},scratch[2]={0};assert(hone_driver_program_count==2 && hone_driver_input_count==1 && hone_driver_slot_count==2);
assert(hone_driver_run(hone_driver_programs[1].code,hone_driver_programs[1].length,&v,1,&out[0])&&out[0]==2);
v=3;assert(hone_driver_frame(hone_driver_programs,hone_driver_program_count,hone_driver_input_slots,hone_driver_slot_count,&v,hone_driver_input_count,scratch,out)&&out[0]==3&&out[1]==9);
v=1e308;out[0]=42;out[1]=43;assert(!hone_driver_frame(hone_driver_programs,2,hone_driver_input_slots,2,&v,1,scratch,out)&&out[0]==42&&out[1]==43&&scratch[0]==v);return 0;}''')
            subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(out/'test.c'),'-lm','-o',str(out/'test')],check=True)
            subprocess.run([str(out/'test')],check=True)
    def test_reject_unresolved_and_muted(self):
        p=fixture();p['drivers'][0]['variables'][0]['targets'][0]['current']['status']='unresolved'
        with self.assertRaises(ValueError):emit(p)
        p=fixture();p['drivers'][0]['mute']=True
        with self.assertRaises(ValueError):emit(p)
        p=fixture();p['drivers'][0]['fcurve_modifiers']=[{'type':'GENERATOR','mute':False,'mode':'POLYNOMIAL','poly_order':1,'coefficients':[0.,2.],'use_additive':False,'use_restricted_range':False,'use_influence':False}]
        with self.assertRaises(ValueError):emit(p)
        p['drivers'][0]['fcurve_modifiers'][0]['coefficients']=[0.,1.]
        emit(p)

if __name__=='__main__':unittest.main()
