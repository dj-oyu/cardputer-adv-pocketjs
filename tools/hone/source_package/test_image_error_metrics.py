import unittest,numpy as np
from image_error_metrics import compare_linear_rgb
class Test(unittest.TestCase):
 def test_identity(self):
  a=np.zeros((2,3,3));self.assertEqual(compare_linear_rgb(a,a)['pixels_over_threshold'],0)
 def test_error_not_hidden_by_quantization(self):
  a=np.zeros((1,1,3));b=a+.001;x=compare_linear_rgb(a,b);self.assertEqual(x['pixels_over_threshold'],1);self.assertEqual(x['linear_rgb565_pixels_different'],0)
 def test_boundary(self):
  a=np.zeros((1,1,3));b=a.copy();b[0,0,0]=.5/31;self.assertEqual(compare_linear_rgb(a,b)['linear_rgb565_pixels_different'],1)
 def test_reject(self):
  a=np.zeros((1,1,3))
  for b in [a+np.nan,np.zeros((1,2,3))]:
   with self.assertRaises(ValueError):compare_linear_rgb(a,b)
  with self.assertRaises(ValueError):compare_linear_rgb(a,a,-1)
if __name__=='__main__':unittest.main()
