import copy,unittest
from base_color_plan import plan
from scene_node_plan import plan as scene_plan
from test_scene_node_plan import TestSceneNodePlan


class TestBaseColorPlan(unittest.TestCase):
    def fixture(self):
        package,hierarchy=TestSceneNodePlan().fixture();scene=scene_plan(package,hierarchy)
        def material(name,linked):
            node=dict(name='shader',type='ShaderNodeBsdfPrincipled',inputs=[dict(name='Base Color',index=0,
                default=dict(status='ok',value=[.1,.2,.3,1]))])
            return dict(name=name,nodes=[node],links=[dict(target_node='shader',target_socket=0,muted=False)] if linked else [])
        inventory=dict(schema='hone.material-inventory.v1',runtime_ready=False,source_sha256='synthetic',materials=[material('a',False),material('b',True)])
        routes=dict(schema='hone.material-route-plan.v1',runtime_ready=False,source_sha256='synthetic',unsupported={},shader_routes=[])
        return package,inventory,routes,scene
    def test_constant_and_procedural_are_distinct(self):
        result=plan(*self.fixture());self.assertFalse(result['runtime_ready'])
        self.assertEqual([m['kind'] for m in result['materials']],['constant','graph'])
        self.assertEqual([n['kind'] for n in result['nodes']],['constant','graph'])
        self.assertNotIn('linear_rgba',result['materials'][1])
    def test_missing_part_and_invalid_color_reject(self):
        p,i,r,s=self.fixture();bad=copy.deepcopy(s);bad['nodes'].pop()
        with self.assertRaises(ValueError):plan(p,i,r,bad)
        bad=copy.deepcopy(i);bad['materials'][0]['nodes'][0]['inputs'][0]['default']['value'][0]=2
        with self.assertRaises(ValueError):plan(p,bad,r,s)


if __name__=='__main__':unittest.main()
