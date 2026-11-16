import os,pathlib,subprocess,tempfile,unittest
from native_geometry import emit
from test_consumer_plan import fixture

class TestNativeDQ(unittest.TestCase):
    def test_existing_dq_and_mapping(self):
        here=pathlib.Path(__file__).parent
        # Repository layout, with an explicit local-checkout override for tests.
        dq=pathlib.Path(os.environ.get('HONE_DQ_SOURCE',str(here.parent/'source_package'/'rigid_dq_candidate.c')))
        if not dq.is_file():self.fail('missing existing DQ source; set HONE_DQ_SOURCE to the public candidate')
        package=fixture();m=package['meshes'][0];m['vertex_groups']=['b','a'];m['weights']=[[[0,1]] for _ in m['vertices']]
        with tempfile.TemporaryDirectory() as td:
            d=pathlib.Path(td);(d/'geometry.h').write_text(emit(package));(d/'native_dq.h').write_text((here/'native_dq.h').read_text())
            (d/'test.c').write_text('''#include "geometry.h"
#include "native_dq.h"
#include <assert.h>
int main(void){
 const double to[12]={2,0,0,4,0,3,0,0,0,0,1,0};
 const double from[12]={.5,0,0,-2,0,1./3,0,0,0,0,1,0};
 const double joints[14]={1,0,0,0,0,0,0,1,0,0,0,2,3,0};
 uint32_t map[2]={1,0};float position[3]={1,2,3},out[3]={7,8,9};
 assert(hone_input_dq_vertex(hone_input_meshes,0,position,map,2,joints,2,to,from,out));
 assert(out[0]==2 && out[1]==3 && out[2]==3);
 map[0]=2;out[0]=7;out[1]=8;out[2]=9;
 assert(!hone_input_dq_vertex(hone_input_meshes,0,position,map,2,joints,2,to,from,out));
 assert(out[0]==7 && out[1]==8 && out[2]==9);
 map[0]=1;
 assert(!hone_input_dq_vertex(hone_input_meshes,6,position,map,2,joints,2,to,from,out));
 assert(!hone_input_dq_vertex(hone_input_meshes,0,position,map,1,joints,2,to,from,out));
 hone_input_mesh bad=hone_input_meshes[0];
 hone_input_range ranges[6]={{0,5},{0,0},{0,0},{0,0},{0,0},{0,0}};
 bad.weight_ranges=ranges;
 assert(!hone_input_dq_vertex(&bad,0,position,map,2,joints,2,to,from,out));
 ranges[0].count=1;
 hone_input_weight weights[1]={{0,-1}};bad.weights=weights;
 assert(!hone_input_dq_vertex(&bad,0,position,map,2,joints,2,to,from,out));
 weights[0].weight=1;weights[0].group=2;
 assert(!hone_input_dq_vertex(&bad,0,position,map,2,joints,2,to,from,out));
 double invalid[12];for(unsigned k=0;k<12;k++)invalid[k]=to[k];invalid[3]=INFINITY;
 assert(!hone_input_dq_vertex(hone_input_meshes,0,position,map,2,joints,2,invalid,from,out));
 assert(out[0]==7 && out[1]==8 && out[2]==9);
 position[0]=NAN;
 assert(!hone_input_dq_vertex(hone_input_meshes,0,position,map,2,joints,2,to,from,out));
 assert(out[0]==7 && out[1]==8 && out[2]==9);
 return 0;
}''')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2','-fsanitize=address,undefined',str(d/'test.c'),str(dq),'-lm','-o',str(d/'test')],check=True)
            subprocess.run([str(d/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
if __name__=='__main__':unittest.main()
