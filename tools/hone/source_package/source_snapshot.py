"""Host verification path for preserved poses; NOT continuous native animation."""
import math,hashlib,json,struct

def topology_identity(mesh):
    topology={k:mesh[k] for k in ('name','polygons','triangles')}
    return hashlib.sha256(json.dumps(topology,sort_keys=True,separators=(',',':')).encode()).hexdigest()

def map_snapshot(mesh,plan,snapshot,expected_frame,expected_source):
    if snapshot['frame']!=expected_frame or snapshot['source_sha256']!=expected_source or snapshot['topology_sha256']!=topology_identity(mesh):
        raise ValueError('snapshot identity mismatch')
    p=snapshot['positions'];n=snapshot['corner_normals']
    if len(p)!=len(mesh['vertices']) or len(n)!=len(mesh['corner_normals']):raise ValueError('snapshot topology mismatch')
    for v in p+n:
        if len(v)!=3 or any(not math.isfinite(x) for x in v):raise ValueError('nonfinite geometry')
    if any(abs(sum(x*x for x in v)-1)>1e-4 for v in n):raise ValueError('nonunit normal')
    if any(abs(x)>4096 for v in p for x in v):raise ValueError('position range')
    expected=[];seen=set()
    for t in mesh['triangles']:
        for vi,li in zip(t['vertices'],t['loops']):
            if li not in seen:expected.append((vi,li));seen.add(li)
    if plan['name']!=mesh['name'] or len(plan['source_vertices'])!=len(plan['source_corners']) or list(zip(plan['source_vertices'],plan['source_corners']))!=expected:
        raise ValueError('consumer mapping mismatch')
    # Cold/transient identity is retained without resynthesizing authoritative normals.
    rows=[p[vi]+n[li] for vi,li in zip(plan['source_vertices'],plan['source_corners'])]
    data=b''.join(struct.pack('<6f',*v) for v in rows)
    return dict(frame=expected_frame,topology_sha256=snapshot['topology_sha256'],bytes=data,runtime_ready=False)
