import unittest,math
from depth_clip_reference import clip_triangle
from alpha_triangle_reference import render
class Test(unittest.TestCase):
 def test_inside_outside(self):
  t=((0,0,1,0),(2,0,1,1),(0,2,1,2));self.assertEqual(clip_triangle(t,0,2),(t,));self.assertEqual(clip_triangle(t,2,3),())
 def test_both_planes_interpolant(self):
  t=((0,0,-2,0),(4,0,2,4),(0,4,0,0));out=clip_triangle(t,-1,1);self.assertTrue(1<=len(out)<=3)
  for v in [v for t in out for v in t]:self.assertTrue(-1<=v[2]<=1);self.assertAlmostEqual(v[3],v[0])
 def test_boundary(self):
  self.assertEqual(len(clip_triangle(((0,0,0,0),(2,0,-1,0),(0,2,-1,0)),0,1)),0)
 def test_raster(self):
  t=dict(vertices=((0,0,-1,0),(4,0,1,4),(0,4,1,0)),color=(1,0,0),alpha=.5)
  actual=render([t],5,5,depth_bounds=(0,2));pieces=[dict(t,vertices=v) for v in clip_triangle(t['vertices'],0,2)]
  self.assertEqual(actual,render(pieces,5,5));self.assertTrue(any(p[0]>.0 for p in actual))
 def test_invalid(self):
  with self.assertRaises(ValueError):clip_triangle(((0,0,math.nan,0),)*3,0,1)
if __name__=='__main__':unittest.main()
