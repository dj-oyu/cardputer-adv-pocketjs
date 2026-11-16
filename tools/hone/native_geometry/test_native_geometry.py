import copy
import json
import pathlib
import subprocess
import tempfile
import unittest
from native_geometry import emit, build, f32
from test_consumer_plan import fixture

class Test(unittest.TestCase):
    def test_native_layout(self):
        p=fixture();p['meshes'][0]['vertex_groups']=['bone']
        p['meshes'][0]['weights'][0]=[[0,1.0]]
        before=copy.deepcopy(p)
        output=emit(p)
        self.assertEqual(p,before);self.assertEqual(output,emit(p))
        with tempfile.TemporaryDirectory() as td:
            d=pathlib.Path(td);(d/'candidate.h').write_text(output)
            (d/'test.c').write_text('''#include "candidate.h"
#include <assert.h>
int main(void) {
 const hone_input_mesh *m=hone_input_meshes;
 assert(hone_input_mesh_count==1 && m->vertices==6 && m->triangles==2);
 assert(m->morphs==2 && m->groups==1);
 assert(m->source_vertex[4]==3 && m->source_corner[4]==4);
 assert(m->smooth[0]==1 && m->smooth[1]==0);
 assert(m->normal[2]==1 && m->normal[10]==1);
 assert(m->morph_positions[18+4*3+2]==0.5f);
 assert(m->morph_relative[1]==0 && m->weight_ranges[3].count==1);
 assert(m->weights[m->weight_ranges[3].first].weight==1);
 return 0;
}''')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(d/'test.c'),'-o',str(d/'test')],check=True)
            import os
            subprocess.run([str(d/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
    def test_bound_and_rounding(self):
        for bad in [float('nan'),1e100]:
            p=fixture();p['meshes'][0]['vertices'][0][0]=bad
            with self.assertRaises(ValueError):emit(p)
        p=fixture();p['runtime_ready']=True
        with self.assertRaises(ValueError):emit(p)
    def test_loop_alias_rejected(self):
        p=fixture();m=p['meshes'][0]
        m['polygons'][1]['loops'][0]=0;m['triangles'][1]['loops'][0]=0
        m['polygons'][1]['vertices'][0]=3;m['triangles'][1]['vertices'][0]=3
        with self.assertRaises(ValueError):emit(p)
        p=fixture();p['meshes'][0]['polygons'][0]['loops'][1]=0
        with self.assertRaises(ValueError):emit(p)
    def test_triangulation_loop_reuse(self):
        p=fixture();m=p['meshes'][0]
        m['polygons']=[dict(vertices=[0,1,2,3],loops=[0,1,2,3],smooth=True)]
        m['triangles']=[dict(vertices=[0,1,2],loops=[0,1,2],polygon=0),
                        dict(vertices=[0,2,3],loops=[0,2,3],polygon=0)]
        self.assertIn('hone_input_mesh_count = 1',emit(p))
    def test_float32_rounding(self):
        self.assertEqual(f32(16777217),f32(16777216))
    def test_no_overwrite_receipt(self):
        with tempfile.TemporaryDirectory() as td:
            d=pathlib.Path(td);(d/'source').write_text(json.dumps(fixture()))
            r=build(d/'source',d/'out.h');self.assertFalse(r['runtime_ready'])
            with self.assertRaises(FileExistsError):build(d/'source',d/'out.h')
if __name__=='__main__':unittest.main()
