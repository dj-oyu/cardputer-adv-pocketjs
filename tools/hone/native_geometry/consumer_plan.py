"""Host-only source-package reader and corner-split planning. Not AYNM admission."""
import json, math, pathlib, sys

class Unsupported(ValueError):
    pass

def xyz(v):
    if not isinstance(v, list) or len(v) != 3 or any(type(x) not in (int, float) or not math.isfinite(x) for x in v):
        raise ValueError('invalid xyz')
    return v

def index(i, count):
    if type(i) is not int or not 0 <= i < count:
        raise ValueError('index out of bounds')
    return i

def prepare(package):
    if package.get('schema') != 'hone.source-preservation.v1' or package.get('runtime_ready') is not False:
        raise ValueError('unsupported source schema/admission flag')
    result=[]
    for mesh in package['meshes']:
        vertices=[xyz(v) for v in mesh['vertices']]
        normals=[xyz(v) for v in mesh['corner_normals']]
        weights=mesh['weights']
        if len(weights)!=len(vertices):raise ValueError('weight count')
        for w in weights:
            for g, v in w:
                index(g,len(mesh['vertex_groups']))
                if type(v) not in (int,float) or not math.isfinite(v) or not 0<=v<=1:raise ValueError('weight')
        keys=mesh['shape_keys']
        if keys:
            names=[k['name'] for k in keys['blocks']]
            if len(set(names))!=len(names):raise ValueError('duplicate morph')
            for k in keys['blocks']:
                if len(k['positions'])!=len(vertices) or k['relative_key'] not in names:raise ValueError('morph topology')
                for p in k['positions']:xyz(p)
        # One source loop belongs to one polygon/corner. Triangulation may reuse
        # that same corner, but two polygons cannot alias its index.
        loop_owner={}
        for pi, polygon in enumerate(mesh['polygons']):
            if len(polygon['vertices']) != len(polygon['loops']):
                raise ValueError('polygon loop count')
            for vi, li in zip(polygon['vertices'], polygon['loops']):
                index(vi,len(vertices)); index(li,len(normals))
                if li in loop_owner:
                    raise ValueError('duplicate source loop ownership')
                loop_owner[li]=(pi,vi)
        remap={}; source=[]; corners=[]; triangles=[]; polygon_ids=[]; smooth=[]
        # Corner splitting is deliberately not welding by current normal equality:
        # equal rest normals can diverge after morph/skin deformation.
        for t in mesh['triangles']:
            if len(t['vertices'])!=3 or len(t['loops'])!=3:raise ValueError('triangle')
            pi=index(t['polygon'],len(mesh['polygons']));p=mesh['polygons'][pi]
            if type(p['smooth']) is not bool:raise ValueError('smooth mode')
            if len(p['vertices'])!=len(p['loops']):raise ValueError('polygon loop count')
            membership=dict(zip(p['loops'],p['vertices']))
            row=[]
            for vi,li in zip(t['vertices'],t['loops']):
                index(vi,len(vertices));index(li,len(normals))
                if membership.get(li)!=vi:raise ValueError('triangle corner mismatch')
                if li not in remap:
                    remap[li]=len(source);source.append(vi);corners.append(li)
                row.append(remap[li])
            triangles.append(row);polygon_ids.append(pi);smooth.append(p['smooth'])
        result.append(dict(name=mesh['name'],source_vertices=source,source_corners=corners,
            positions=[vertices[i] for i in source],rest_corner_normals=[normals[i] for i in corners],
            weights=[weights[i] for i in source],triangles=triangles,source_polygons=polygon_ids,
            polygon_smooth=smooth,shape_keys=None if keys is None else dict(keys,blocks=[dict(k,positions=[k['positions'][i] for i in source]) for k in keys['blocks']]),
            pending=['recompute deformed corner normals preserving source smoothing groups','material/opacity adapter','node-local transform and skin adapter','cold AYNM mesh topology generation']))
    return dict(schema='hone.corner-consumer-plan.v1',runtime_ready=False,source_sha256=package.get('source_sha256'),meshes=result,unresolved=package.get('unresolved',[]))

def admit_runtime(plan):
    # No path silently strips rig/morph/opacity to fit today's cold importer.
    raise Unsupported('host plan only: deformation, normal, material and cold topology adapters are not implemented')

if __name__=='__main__':
    plan=prepare(json.loads(pathlib.Path(sys.argv[1]).read_text()))
    print(json.dumps(dict(runtime_ready=False,meshes=len(plan['meshes']),vertices=sum(len(m['positions']) for m in plan['meshes']),triangles=sum(len(m['triangles']) for m in plan['meshes']),morph_meshes=sum(m['shape_keys'] is not None for m in plan['meshes']),flat_triangles=sum(not s for m in plan['meshes'] for s in m['polygon_smooth']),unresolved=plan['unresolved']),indent=2))
