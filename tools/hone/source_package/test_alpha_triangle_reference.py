from alpha_triangle_reference import render

def tri(v,color=(1,0,0),alpha=.5,**kw):return dict(vertices=v,color=color,alpha=alpha,**kw)
a=tri(((0,0,1,0),(4,0,1,0),(0,4,1,0)));b=tri(((4,0,1,0),(4,4,1,0),(0,4,1,0)))
r=render([a,b],5,5)
# Adjacent triangle shared diagonal must not double-compose.
assert all(c[0] in (0,.5) for c in r)
assert render([b,a],5,5)==r
assert render([dict(a,visible=False),b],5,5)!=r
# Per-pixel crossing depth swaps the compositing order without centroid sorting.
c=tri(((0,0,0,0),(4,0,4,0),(0,4,0,0)))
d=tri(((0,0,4,0),(4,0,0,0),(0,4,4,0)),(0,0,1))
p=render([c,d],5,5);assert p[6]==(.25,0,.5) and p[7]==(.5,0,.25)
assert render([d,c],5,5)[6]==p[6]
# Opaque foreground must cull alpha independently of submission order.
o=tri(a['vertices'],(0,1,0),1);assert render([o,dict(a,vertices=tuple((x,y,0,u) for x,y,z,u in a['vertices']))],5,5)==render([o],5,5)
old=r
try:render([a]*5,5,5)
except ValueError:pass
else:raise AssertionError('overflow accepted')
assert old==r and render([a,b],5,5)==old
m=tri(((0,0,1,-2),(4,0,1,2),(0,4,1,-2)),alpha=.05,mask=(-.1,.1));assert render([m],5,5)[7][0]==1
print('PASS orthographic shared-edge ownership, crossing depth, visibility, object-local mask and reject/retry; flat-linear RGB only')

assert render([a],5,5,depth_bounds=(0,2))==render([a],5,5)
assert all(c==(0.,0.,0.) for c in render([a],5,5,depth_bounds=(2,3)))
from depth_clip_reference import clip_triangle
assert render([c],5,5,depth_bounds=(1,3))==render([dict(c,vertices=v) for v in clip_triangle(c['vertices'],1,3)],5,5)
assert render([a,b],5,5)==old
print('PASS near/far interval inclusion, full exclusion, crossing clip reference and retry')
