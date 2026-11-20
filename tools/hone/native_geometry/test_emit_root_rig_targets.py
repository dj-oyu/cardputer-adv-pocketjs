import copy,unittest
from emit_root_rig_targets import emit


class TestRootRigEmitter(unittest.TestCase):
    def fixture(self):
        m=[1,0,0,0,0,1,0,0,0,0,1,0]
        rig=dict(schema='hone.root-rig-snapshot.v1',runtime_ready=False,source_sha256='synthetic',rig_object='rig',
            bones=[dict(name='root',rest_q=[1,0,0,0],rest_t=[0,0,0],location=[0,0,0],rotation=[0,0,0,0],mode='XYZ')],
            meshes=[dict(source_mesh=0,group_to_joint=[0],to_rig=m,from_rig=m)])
        routes=dict(schema='hone.driver-route-plan.v1',runtime_ready=False,source_sha256='synthetic',routes=[
            dict(kind='external_object_or_rig',owner=dict(type='Object',name='rig'),data_path='pose.bones["root"].rotation_euler',array_index=1,program=2),
            dict(kind='shader_property',owner=dict(type='ShaderNodeTree',name='shader'),data_path='x',array_index=0,program=3)])
        return rig,routes
    def test_root_route_and_unbound_are_explicit(self):
        rig,routes=self.fixture();header,summary=emit(rig,routes)
        self.assertEqual(summary['bone_driver_routes'],1);self.assertEqual(summary['unbound_programs'],1)
        self.assertIn('{2,0,1}',header)
    def test_duplicate_and_mismatched_identity_reject(self):
        rig,routes=self.fixture();bad=copy.deepcopy(routes);bad['routes'].append(copy.deepcopy(bad['routes'][0]))
        with self.assertRaises(ValueError):emit(rig,bad)
        bad=copy.deepcopy(rig);bad['bones'][0]['mode']='QUATERNION'
        with self.assertRaises(ValueError):emit(bad,routes)
        bad=copy.deepcopy(rig);bad['source_sha256']='other'
        with self.assertRaises(ValueError):emit(bad,routes)


if __name__=='__main__':unittest.main()
