"""Host candidate mapping for AYNM limits; does not emit/admit runtime assets."""
import math,hashlib,json

def sub(a,b):return [x-y for x,y in zip(a,b)]
def dot(a,b):return sum(x*y for x,y in zip(a,b))
def cross(a,b):return [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
def unit(a):
    d=math.sqrt(dot(a,a))
    if not math.isfinite(d) or d<1e-12:raise ValueError('degenerate normal')
    return [x/d for x in a]

def deformed_corner_normals(mesh,positions):
    # Conservative initial contract: triangle polygons only; sharp/custom topology
    # needs a smoothing-group adapter instead of silently using vertex smoothing.
    if mesh.get('sharp_edges') or mesh.get('has_custom_normals'):raise ValueError('custom/sharp normals unsupported')
    if len(positions)!=len(mesh['vertices']):raise ValueError('vertex identity mismatch')
    if any(len(v)!=3 or any(not math.isfinite(x) for x in v) for v in positions):raise ValueError('invalid positions')
    accum=[[0.,0.,0.] for _ in positions];face=[]
    for p in mesh['polygons']:
        if len(p['vertices'])!=3:raise ValueError('non-triangle polygon requires polygon normal adapter')
        ids=p['vertices'];v=[positions[i] for i in ids];n=unit(cross(sub(v[1],v[0]),sub(v[2],v[0])));face.append(n)
        if p['smooth']:
            for j,i in enumerate(ids):
                a=unit(sub(v[(j+1)%3],v[j]));b=unit(sub(v[(j+2)%3],v[j]));theta=math.acos(max(-1.,min(1.,dot(a,b))))
                accum[i]=[a+theta*b for a,b in zip(accum[i],n)]
    out=[None]*len(mesh['corner_normals'])
    for p,n in zip(mesh['polygons'],face):
        for vi,li in zip(p['vertices'],p['loops']):out[li]=unit(accum[vi]) if p['smooth'] else n
    if any(v is None for v in out):raise ValueError('missing corner')
    return out

def partition(mesh,plan):
    chunks=[];vertices=[];lookup={};tris=[];src_tri=[];material=None
    def flush():
        if tris:
            chunks.append(dict(source_mesh=mesh['name'],source_plan_vertices=vertices.copy(),triangles=[r.copy() for r in tris],source_triangles=src_tri.copy(),material_slot=material))
    for ti,t in enumerate(plan['triangles']):
        mat=mesh['polygons'][plan['source_polygons'][ti]]['material']
        added=len(set(t)-set(lookup))
        if tris and (len(vertices)+added>32 or len(tris)>=64 or mat!=material):
            flush();vertices=[];lookup={};tris=[];src_tri=[]
        material=mat;row=[]
        for i in t:
            if i not in lookup:lookup[i]=len(vertices);vertices.append(i)
            row.append(lookup[i])
        tris.append(row);src_tri.append(ti)
    flush()
    identity=hashlib.sha256(json.dumps(chunks,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    return dict(chunks=chunks,topology_sha256=identity,runtime_ready=False)
