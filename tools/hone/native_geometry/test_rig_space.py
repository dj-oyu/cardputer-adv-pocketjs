import math,unittest
from rig_space import mul,inverse,mesh_palette,dq_factors
I=[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]]
def translate(x,y,z): return [[1,0,0,x],[0,1,0,y],[0,0,1,z],[0,0,0,1]]
class TestRigSpace(unittest.TestCase):
    def near(self,a,b):
        for ar,br in zip(a,b):
            for x,y in zip(ar,br):self.assertAlmostEqual(x,y,places=10)
    def test_rest_identity(self):
        m=translate(7,-2,3);a=translate(-4,3,9);b=translate(1,2,3)
        self.near(mesh_palette(m,a,b,b),I)
    def test_dynamic_and_apply_world_once(self):
        m=[[2,0,0,7],[0,3,0,-2],[0,0,4,3],[0,0,0,1]]
        a=translate(-4,3,9);b=translate(1,2,3)
        for angle in [0,.2,1,2]:
            c=math.cos(angle);s=math.sin(angle)
            p=mul([[c,-s,0,0],[s,c,0,0],[0,0,1,0],[0,0,0,1]],b)
            candidate=mul(m,mesh_palette(m,a,b,p))
            oracle=mul(mul(mul(mul(a,p),inverse(b)),inverse(a)),m)
            self.near(candidate,oracle)
    def test_analytical_pivot_and_mesh_space(self):
        # Bone rotates +90 degrees around armature-space pivot x=3.
        b=translate(3,0,0)
        p=[[0,-1,0,3],[1,0,0,0],[0,0,1,0],[0,0,0,1]]
        m=translate(4,0,0)
        c=mesh_palette(m,I,b,p)
        # Mesh local origin was world (4,0,0); now world (3,1,0).
        self.assertEqual([mul(m,c)[r][3] for r in range(3)],[3,1,0])
        # Node-local result must be (-1,1,0), not world-space again.
        self.assertEqual([c[r][3] for r in range(3)],[-1,1,0])
    def test_dq_armature_space_factors(self):
        m=[[2,0,0,4],[0,3,0,0],[0,0,1,0],[0,0,0,1]]
        a=translate(-2,1,0);b=translate(3,0,0)
        p=[[0,-1,0,3],[1,0,0,0],[0,0,1,0],[0,0,0,1]]
        to_rig,bone,from_rig=dq_factors(m,a,b,p)
        self.near(mul(from_rig,to_rig),I)
        self.near(mul(mul(from_rig,bone),to_rig),mesh_palette(m,a,b,p))
        self.assertEqual(bone[0][:3],[0,-1,0])
        self.assertEqual(bone[1][:3],[1,0,0])
    def test_reject(self):
        for bad in [[[0]*4]*4,[[1,0,0,float('nan')],*I[1:]],[[1,0,0,float('inf')],*I[1:]]]:
            with self.assertRaises(ValueError): inverse(bad)
if __name__=='__main__':unittest.main()
