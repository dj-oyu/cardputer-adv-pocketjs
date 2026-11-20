import copy,unittest
from material_route_plan import plan


class TestMaterialRoutes(unittest.TestCase):
    def fixture(self):
        p=dict(schema='hone.source-preservation.v1',runtime_ready=False,source_sha256='source',
            meshes=[dict(name='mesh',material_slots=['mat'],polygons=[dict(material=0)])])
        socket=dict(default=dict(status='ok',value=.5),is_multi_input=False)
        node=dict(name='value',type='ShaderNodeValue',inputs=[],outputs=[socket],mute=False)
        output=dict(name='output',type='ShaderNodeOutputMaterial',inputs=[socket],outputs=[],mute=False,active_output=True)
        inv=dict(schema='hone.material-inventory.v1',runtime_ready=False,source_sha256='source',materials=[dict(
            name='mat',library=None,use_nodes=True,nodes=[node,output],links=[dict(source_node='value',source_socket=0,
                target_node='output',target_socket=0,muted=False)],uses=[dict(object='mesh',slot=0,polygons=[0])])])
        routes=dict(schema='hone.driver-route-plan.v1',runtime_ready=False,source_sha256='source',routes=[dict(
            kind='shader_property',program=0,owner=dict(type='ShaderNodeTree',embedded_in=[dict(type='Material',name='mat')]),
            data_path='nodes["value"].outputs[0].default_value',array_index=0)])
        return p,inv,routes
    def test_polygon_and_driven_socket_bindings(self):
        p,i,r=self.fixture();result=plan(p,i,r)
        self.assertFalse(result['runtime_ready']);self.assertEqual(result['polygons'],1)
        self.assertEqual(len(result['shader_routes']),1);self.assertEqual(result['unsupported'],{})
    def test_missing_polygon_ambiguous_links_and_wrong_source_reject(self):
        p,i,r=self.fixture();bad=copy.deepcopy(i);bad['materials'][0]['uses'][0]['polygons']=[]
        with self.assertRaises(ValueError):plan(p,bad,r)
        bad=copy.deepcopy(i);bad['materials'][0]['links']*=2
        with self.assertRaises(ValueError):plan(p,bad,r)
        bad=copy.deepcopy(r);bad['source_sha256']='other'
        with self.assertRaises(ValueError):plan(p,i,bad)


if __name__=='__main__':unittest.main()
