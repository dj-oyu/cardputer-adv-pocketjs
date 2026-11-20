import copy
import unittest
from driver_route_plan import plan


class TestDriverRoutes(unittest.TestCase):
    def fixture(self):
        controls=[dict(id=dict(type='Object',name='control'),data_path='["amount"]')]
        h=dict(schema='hone.scene-hierarchy-snapshot.v1',runtime_ready=False,source_sha256='synthetic',
               controls=controls,meshes=[dict(name='mesh',shape_key_id='key')],
               parents=[dict(name='root',direct_xyz_property_pose=True)],
               scene_objects=[dict(name='mesh',type='MESH'),dict(name='root',type='EMPTY')])
        s=dict(schema='hone.scene-node-plan.v1',runtime_ready=False,source_sha256='synthetic',nodes=[
            dict(index=0,kind='group',source_object='root'),
            dict(index=1,kind='gouraudMesh',source_mesh=0),dict(index=2,kind='gouraudMesh',source_mesh=0)])
        outputs=[('Object','mesh','hide_render'),('Object','root','rotation_euler'),
                 ('Key','key','key_blocks["smile"].value'),('ShaderNodeTree','material','nodes[0]')]
        b=dict(schema='hone.driver-bindings.v1',runtime_ready=False,source_sha256='synthetic',inputs=controls,
               programs=[dict(program=i,owner=dict(type=k,name=n),data_path=p,array_index=0)
                         for i,(k,n,p) in enumerate(outputs)])
        return h,s,b

    def test_all_outputs_preserved_and_partitions_routed(self):
        h,s,b=self.fixture();result=plan(h,s,b)
        self.assertFalse(result['runtime_ready'])
        self.assertEqual(result['counts'],dict(mesh_visibility=1,parent_transform=1,shape_key=1,shader_property=1))
        self.assertEqual(result['routes'][0]['node_ids'],[1,2])
        self.assertEqual(result['routes'][1]['node_ids'],[0])
        self.assertEqual(result['routes'][2]['source_meshes'],[0])
        self.assertTrue(all(x['status']=='requires_live_adapter' for x in result['routes']))

    def test_wrong_source_input_or_missing_owner_rejects(self):
        h,s,b=self.fixture()
        bad=copy.deepcopy(b);bad['inputs']=[]
        with self.assertRaises(ValueError):plan(h,s,bad)
        bad=copy.deepcopy(b);bad['programs'][2]['owner']['name']='missing'
        with self.assertRaises(ValueError):plan(h,s,bad)
        bad=copy.deepcopy(b);bad['programs'][0]['program']=5
        with self.assertRaises(ValueError):plan(h,s,bad)


if __name__=='__main__':unittest.main()
