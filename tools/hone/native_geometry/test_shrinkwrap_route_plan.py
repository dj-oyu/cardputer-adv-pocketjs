import copy,unittest
from shrinkwrap_route_plan import plan


def modifier(target):
    return dict(type='SHRINKWRAP',viewport=True,render=True,shrinkwrap=dict(
        target=target,auxiliary_target=None,wrap_method='NEAREST_SURFACEPOINT',wrap_mode='ON_SURFACE',
        vertex_group='',invert_vertex_group=False,subsurf_levels=0,offset=.01))


class TestShrinkwrapRoutes(unittest.TestCase):
    def fixture(self):
        meshes=[dict(name=str(i),vertices=[[0,0,0]]*3,triangles=[{'vertices':[0,1,2]}],modifiers=[])
                for i in range(3)]
        meshes[0]['modifiers']=[modifier('1')];meshes[1]['modifiers']=[modifier('2')]
        p=dict(schema='hone.source-preservation.v1',runtime_ready=False,source_sha256='source',meshes=meshes)
        h=dict(schema='hone.scene-hierarchy-snapshot.v1',runtime_ready=False,source_sha256='source',
               meshes=[dict(name=m['name']) for m in meshes])
        return p,h
    def test_targets_resolved_before_dependents(self):
        p,h=self.fixture();result=plan(p,h)
        self.assertFalse(result['runtime_ready'])
        self.assertEqual([(x['source_mesh'],x['target_mesh']) for x in result['routes']],[(1,2),(0,1)])
    def test_reject_cycle_mask_and_mismatch(self):
        p,h=self.fixture();bad=copy.deepcopy(p);bad['meshes'][1]['modifiers'][0]['shrinkwrap']['target']='0'
        with self.assertRaises(ValueError):plan(bad,h)
        bad=copy.deepcopy(p);bad['meshes'][0]['modifiers'][0]['shrinkwrap']['vertex_group']='mask'
        with self.assertRaises(ValueError):plan(bad,h)
        bad=copy.deepcopy(p);bad['meshes'][0]['modifiers'][0]['render']=False
        with self.assertRaises(ValueError):plan(bad,h)


if __name__=='__main__':unittest.main()
