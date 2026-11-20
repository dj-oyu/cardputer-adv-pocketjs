"""Generated hierarchy node IDs call the actual provider on synthetic data."""
import pathlib
import subprocess
import tempfile
import unittest
from native_geometry import emit as emit_geometry
from provider_node_map import emit as emit_map
from scene_node_plan import plan
from test_scene_node_plan import TestSceneNodePlan


class TestSceneProviderMap(unittest.TestCase):
    def test_generated_map_uses_actual_scene_node_ids(self):
        here=pathlib.Path(__file__).resolve().parent
        root=here.parents[2]
        package,hierarchy=TestSceneNodePlan().fixture()
        scene=plan(package,hierarchy)
        with tempfile.TemporaryDirectory() as tmp:
            d=pathlib.Path(tmp)
            (d/'geometry.h').write_text(emit_geometry(package),encoding='utf8')
            (d/'provider_nodes.h').write_text(emit_map(package,scene['provider_assignments']),encoding='utf8')
            (d/'test.c').write_text('''
#include "geometry.h"
#include "native_gather.h"
#include "ayatsuri/ayatsuri_model_provider.h"
#include "native_provider.h"
#include "provider_nodes.h"
#include <assert.h>
#include <string.h>
int main(void) {
 const hone_input_mesh *m=hone_input_meshes;
 float normals[18]={0,0,1,0,0,1,0,0,1,0,1,0,0,1,0,0,1,0};
 hone_provider_mesh mesh={m,m->source_position,normals};
 hone_provider_context ctx={&mesh,1,hone_provider_nodes,hone_provider_node_count};
 ayatsuri_skin_result slot[3],saved[3];memset(slot,0x5a,sizeof slot);memcpy(saved,slot,sizeof slot);
 assert(hone_provider_node_count==3);
 assert(!hone_native_provider(&ctx,0,3,slot));assert(!memcmp(saved,slot,sizeof slot));
 assert(hone_native_provider(&ctx,1,3,slot));assert(slot[0].normal[2]==1);
 assert(hone_native_provider(&ctx,2,3,slot));assert(slot[0].normal[1]==1);
 return 0;
}''',encoding='utf8')
            binary=d/'test'
            subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                '-I'+str(here),'-I'+str(root/'main'),str(d/'test.c'),'-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)


if __name__=='__main__':unittest.main()
