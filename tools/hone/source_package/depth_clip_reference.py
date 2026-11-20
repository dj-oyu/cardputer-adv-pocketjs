"""Independent bounded orthographic depth clipping, host reference only."""
import math

def clip_triangle(vertices,low,high):
 if not math.isfinite(low) or not math.isfinite(high) or low>=high:raise ValueError('depth interval')
 if len(vertices)!=3 or any(len(v)!=4 or not all(math.isfinite(x) for x in v) for v in vertices):raise ValueError('triangle xy-depth-local-mask')
 polygon=[tuple(v) for v in vertices]
 for plane,lower in ((low,True),(high,False)):
  if not polygon:break
  output=[];prev=polygon[-1];prev_in=prev[2]>=plane if lower else prev[2]<=plane
  for curr in polygon:
   curr_in=curr[2]>=plane if lower else curr[2]<=plane
   if curr_in!=prev_in:
    t=(plane-prev[2])/(curr[2]-prev[2]);point=tuple(prev[k]+t*(curr[k]-prev[k]) if k!=2 else plane for k in range(4))
    if not all(math.isfinite(x) for x in point):raise ValueError('clip numeric overflow')
    output.append(point)
   if curr_in:output.append(curr)
   prev,prev_in=curr,curr_in
  # Vertices exactly on a plane must not create duplicate zero-area fans.
  polygon=[]
  for v in output:
   if not polygon or v!=polygon[-1]:polygon.append(v)
  if len(polygon)>1 and polygon[0]==polygon[-1]:polygon.pop()
  if len(polygon)>5:raise ValueError('clip capacity')
 return tuple((polygon[0],polygon[i],polygon[i+1]) for i in range(1,len(polygon)-1))
