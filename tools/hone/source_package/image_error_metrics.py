"""Host image diagnostics only. Metrics never relax geometry admission gates."""
import numpy as np

def compare_linear_rgb(reference,candidate,threshold=1e-4):
 a=np.asarray(reference,dtype=np.float64);b=np.asarray(candidate,dtype=np.float64)
 if a.shape!=b.shape or a.ndim!=3 or a.shape[2]!=3 or min(a.shape[:2])<1:
  raise ValueError('matching nonempty H x W x 3 required')
 if not np.all(np.isfinite(a)) or not np.all(np.isfinite(b)) or not np.isfinite(threshold) or threshold<0:
  raise ValueError('finite samples and nonnegative threshold required')
 error=np.abs(a-b);levels=np.array([31,63,31]);qa=np.floor(np.clip(a,0,1)*levels+.5);qb=np.floor(np.clip(b,0,1)*levels+.5)
 return dict(max_linear_rgb_error=float(error.max()),mean_linear_rgb_error=float(error.mean()),
  pixels_over_threshold=int(np.any(error>threshold,axis=2).sum()),
  linear_rgb565_pixels_different=int(np.any(qa!=qb,axis=2).sum()),pixels=a.shape[0]*a.shape[1])
# RGB565 metric uses linear clipping/rounding, NOT display tone mapping, gamma,
# the production lighting model or an acceptance decision. Boundary pixels are
# included. Compare paired renderer/seed/settings and record those separately.
