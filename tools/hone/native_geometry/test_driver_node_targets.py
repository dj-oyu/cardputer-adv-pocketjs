import os
import pathlib
import subprocess
import tempfile
import unittest
from driver_route_plan import plan
from emit_driver_node_targets import emit
from test_driver_route_plan import TestDriverRoutes


class TestDriverNodeTargets(unittest.TestCase):
    def test_emitter_preserves_unbound_programs_and_rejects_unadmitted_pose(self):
        h,s,b=TestDriverRoutes().fixture();routes=plan(h,s,b)
        header,summary=emit(routes)
        self.assertIn('hone_driver_node_route_count = 3',header)
        self.assertEqual(summary['unbound_programs'],2)
        routes['routes'][1]['direct_xyz_property_pose']=False
        with self.assertRaises(ValueError):emit(routes)

    def test_c_targets_and_atomic_failure(self):
        here=pathlib.Path(__file__).resolve().parent;root=here.parents[2]
        h,s,b=TestDriverRoutes().fixture();header,_=emit(plan(h,s,b))
        with tempfile.TemporaryDirectory() as tmp:
            d=pathlib.Path(tmp);(d/'routes.h').write_text(header,encoding='utf8')
            (d/'test.c').write_text('''
#include "routes.h"
#include <assert.h>
#include <string.h>
int main(void){
 float base[27],pose[27],old[27];uint8_t dirty[3],old_dirty[3];
 for(unsigned i=0;i<27;i++)base[i]=i%9>=6?1.0f:0.0f;
 double values[4]={1,0.5,0,0};
 assert(hone_driver_node_targets(hone_driver_node_routes,hone_driver_node_route_count,values,4,base,3,dirty,pose));
 assert(dirty[0]==AYATSURI_TARGET_ROTATION && pose[3]==0.5f);
 assert(dirty[1]==AYATSURI_TARGET_HIDDEN && dirty[2]==AYATSURI_TARGET_HIDDEN);
 values[0]=0;assert(hone_driver_node_targets(hone_driver_node_routes,hone_driver_node_route_count,values,4,base,3,dirty,pose));
 assert(dirty[1]==AYATSURI_TARGET_VISIBLE && dirty[2]==AYATSURI_TARGET_VISIBLE);
 memcpy(old,pose,sizeof old);memcpy(old_dirty,dirty,sizeof old_dirty);
 values[0]=0.5;assert(!hone_driver_node_targets(hone_driver_node_routes,hone_driver_node_route_count,values,4,base,3,dirty,pose));
 assert(!memcmp(old,pose,sizeof old)&&!memcmp(old_dirty,dirty,sizeof dirty));
 values[0]=0;values[1]=NAN;assert(!hone_driver_node_targets(hone_driver_node_routes,hone_driver_node_route_count,values,4,base,3,dirty,pose));
 assert(!memcmp(old,pose,sizeof old)&&!memcmp(old_dirty,dirty,sizeof dirty));
 hone_driver_node_route dup[2]={{0,1,9},{1,1,9}};values[1]=1;
 assert(!hone_driver_node_targets(dup,2,values,4,base,3,dirty,pose));
 assert(!memcmp(old,pose,sizeof old)&&!memcmp(old_dirty,dirty,sizeof dirty));
 return 0;
}''',encoding='utf8')
            cmd=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(here),'-I'+str(root/'main'),str(d/'test.c'),'-lm','-o',str(d/'test')]
            if os.getenv('HONE_SANITIZE')=='1':cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(cmd,check=True);subprocess.run([str(d/'test')],check=True)


if __name__=='__main__':unittest.main()
