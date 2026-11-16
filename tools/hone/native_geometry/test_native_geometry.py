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
            (d/'native_morph.h').write_text((pathlib.Path(__file__).parent/'native_morph.h').read_text())
            (d/'test.c').write_text('''#include "candidate.h"
#include "native_morph.h"
#include <assert.h>
int main(void) {
 const hone_input_mesh *m=hone_input_meshes;
 assert(hone_input_mesh_count==1 && m->vertices==6 && m->triangles==2);
 assert(m->source_corner_count==6);
 assert(m->source_vertex_count==4 && m->polygon_count==2 && m->polygon_corner_count==6);
 assert(m->source_representative[0]==0 && m->source_representative[3]==4);
 assert(m->polygon_offsets[2]==6 && m->polygon_vertices[4]==3 && m->polygon_corners[4]==4);
 assert(m->polygon_smooth[0]==1 && m->polygon_smooth[1]==0);
 assert(m->source_position[11]==1);
 assert(m->morphs==2 && m->groups==1 && m->relative_morph);
 assert(m->source_vertex[4]==3 && m->source_corner[4]==4);
 assert(m->smooth[0]==1 && m->smooth[1]==0);
 assert(m->normal[2]==1 && m->normal[10]==1);
 assert(m->morph_positions[18+4*3+2]==0.5f);
 assert(m->morph_relative[1]==0 && m->weight_ranges[3].count==1);
 assert(m->weights[m->weight_ranges[3].first].weight==1);
 float controls[2]={0,0.5f},masks[2]={1,0.25f},out[3]={7,8,9};
 assert(hone_input_morph_position(m,4,controls,masks,2,out));
 assert(out[0]==0 && out[1]==0 && out[2]==0.9375f);
 masks[1]=2;out[0]=7;out[1]=8;out[2]=9;
 assert(!hone_input_morph_position(m,4,controls,masks,2,out));
 assert(out[0]==7 && out[1]==8 && out[2]==9);
 masks[1]=1;controls[1]=-1;
 assert(hone_input_morph_position(m,4,controls,masks,2,out) && out[2]==1.5f);
 assert(!hone_input_morph_position(m,4,controls,masks,1,out));
 assert(!hone_input_morph_position(m,6,controls,masks,2,out));
 for(unsigned i=0;i<301;i++) {
  controls[1]=(float)i/100-1;masks[1]=(float)(i%17)/16;
  assert(hone_input_morph_position(m,4,controls,masks,2,out));
  double reference=1.0-0.5*(double)controls[1]*(double)masks[1];
  assert(fabs(out[2]-reference)<1e-6);
 }
 hone_input_mesh absolute=*m;absolute.relative_morph=0;
 assert(!hone_input_morph_position(&absolute,4,controls,masks,2,out));
 controls[1]=INFINITY;out[2]=9;
 assert(!hone_input_morph_position(m,4,controls,masks,2,out) && out[2]==9);
 return 0;
}''')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(d/'test.c'),'-o',str(d/'test')],check=True)
            import os
            subprocess.run([str(d/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
    def test_unsafe_float_options_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            d=pathlib.Path(td)
            (d/'candidate.h').write_text(emit(fixture()))
            (d/'native_morph.h').write_text((pathlib.Path(__file__).parent/'native_morph.h').read_text())
            (d/'test.c').write_text('#include "candidate.h"\n#include "native_morph.h"\nint main(void){return 0;}\n')
            for flag in ['-ffast-math','-ffinite-math-only']:
                r=subprocess.run(['cc','-std=c11',flag,'-c',str(d/'test.c'),'-o',str(d/'test.o')],capture_output=True,text=True)
                self.assertNotEqual(r.returncode,0)
                self.assertIn('hone morph requires finite checks',r.stderr)
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
    def test_source_topology_and_loose_vertex(self):
        p=fixture();m=p['meshes'][0]
        for block in m['shape_keys']['blocks']:block['positions']=[v[:] for v in block['positions']]
        m['vertices'].append([2,3,4]);m['weights'].append([])
        for block in m['shape_keys']['blocks']:block['positions'].append([2,3,4])
        self.assertIn('4294967295',emit(p))
        m['polygons'].append(dict(vertices=[4,0,1],loops=[6,7,8],smooth=True))
        m['corner_normals'].extend([[0,0,1]]*3)
        with self.assertRaisesRegex(ValueError,'no deformation representative'):emit(p)
    def test_sparse_source_corners(self):
        p=fixture();m=p['meshes'][0]
        m['corner_normals']=[[0,0,1]]*6+m['corner_normals']
        for poly in m['polygons']:poly['loops']=[v+6 for v in poly['loops']]
        for triangle in m['triangles']:triangle['loops']=[v+6 for v in triangle['loops']]
        emitted=emit(p)
        self.assertIn('hone_input_0_polygon_corners[] = {6,7,8,9,10,11}',emitted)
        self.assertIn(',4,2,6,12,hone_input_0_source_position',emitted)
    def test_invalid_source_polygon(self):
        p=fixture();m=p['meshes'][0]
        m['polygons'].append(dict(vertices=[],loops=[],smooth=True))
        with self.assertRaises(ValueError):emit(p)
    def test_no_overwrite_receipt(self):
        with tempfile.TemporaryDirectory() as td:
            d=pathlib.Path(td);(d/'source').write_text(json.dumps(fixture()))
            r=build(d/'source',d/'out.h');self.assertFalse(r['runtime_ready'])
            with self.assertRaises(FileExistsError):build(d/'source',d/'out.h')
if __name__=='__main__':unittest.main()
