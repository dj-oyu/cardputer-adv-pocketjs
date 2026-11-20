"""Blender-only deformation oracle; not a device runtime implementation."""
import bpy, math

def evaluate(mesh,positions):
    if mesh.get('has_custom_normals') or mesh.get('sharp_edges'):
        raise ValueError('custom or sharp-edge normal adapter not implemented')
    if len(positions)!=len(mesh['vertices']):raise ValueError('vertex identity mismatch')
    if any(len(v)!=3 or any(not math.isfinite(x) for x in v) for v in positions):raise ValueError('invalid position')
    tmp=bpy.data.meshes.new('source-package-normal-oracle')
    try:
        tmp.from_pydata(positions,[],[p['vertices'] for p in mesh['polygons']])
        for p,q in zip(tmp.polygons,mesh['polygons']):p.use_smooth=q['smooth']
        tmp.update();tmp.calc_loop_triangles()
        source=[(t['polygon'],tuple(t['vertices']),tuple(t['loops'])) for t in mesh['triangles']]
        current=[(t.polygon_index,tuple(t.vertices),tuple(t.loops)) for t in tmp.loop_triangles]
        if source!=current:raise ValueError('deformation changes source triangulation; fixed topology not established')
        normals=[list(n.vector) for n in tmp.corner_normals]
        if any(abs(sum(x*x for x in n)-1)>1e-4 for n in normals):raise ValueError('oracle normal not unit')
        return dict(blender=bpy.app.version_string,positions=[list(v.co) for v in tmp.vertices],corner_normals=normals,triangles=mesh['triangles'],zero_area_triangles=[i for i,t in enumerate(tmp.loop_triangles) if t.area==0],runtime_ready=False)
    finally:bpy.data.meshes.remove(tmp)
