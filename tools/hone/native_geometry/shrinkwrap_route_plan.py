"""Plan source-faithful wrap dependencies; never performs runtime admission."""
import json,math,pathlib,sys


def plan(package,hierarchy):
    if (package.get('schema')!='hone.source-preservation.v1' or
        hierarchy.get('schema')!='hone.scene-hierarchy-snapshot.v1' or
        package.get('runtime_ready') is not False or hierarchy.get('runtime_ready') is not False or
        not package.get('source_sha256') or package.get('source_sha256')!=hierarchy.get('source_sha256')):
        raise ValueError('source identity')
    meshes=package['meshes'];snapshot=hierarchy['meshes']
    if len(meshes)!=len(snapshot) or any(a['name']!=b['name'] for a,b in zip(meshes,snapshot)):
        raise ValueError('mesh identity')
    lookup={m['name']:i for i,m in enumerate(meshes)}
    if len(lookup)!=len(meshes):raise ValueError('duplicate mesh')
    routes=[]
    for mid,mesh in enumerate(meshes):
        for index,mod in enumerate(mesh['modifiers']):
            if type(mod.get('viewport')) is not bool or type(mod.get('render')) is not bool:
                raise ValueError('modifier state unknown')
            if mod['type']!='SHRINKWRAP' or not mod['viewport']:continue
            if not mod['render']:raise ValueError('viewport/render modifier mismatch')
            w=mod.get('shrinkwrap');target=w.get('target') if isinstance(w,dict) else None
            if (not isinstance(w,dict) or w.get('wrap_method')!='NEAREST_SURFACEPOINT' or
                w.get('wrap_mode')!='ON_SURFACE' or w.get('vertex_group')!='' or
                w.get('invert_vertex_group') is not False or w.get('subsurf_levels')!=0 or
                w.get('auxiliary_target') is not None or target not in lookup):
                raise ValueError('unsupported shrinkwrap')
            offset=w.get('offset')
            if type(offset) not in (int,float) or not math.isfinite(offset):raise ValueError('offset')
            target_id=lookup[target]
            if target_id==mid or not meshes[target_id]['triangles']:raise ValueError('invalid wrap target')
            routes.append(dict(source_mesh=mid,target_mesh=target_id,modifier_index=index,offset=offset,
                               source_vertices=len(mesh['vertices']),target_triangles=len(meshes[target_id]['triangles']),
                               status='requires_live_transforms_and_modifier_order'))
    by_source={}
    for route in routes:
        if route['source_mesh'] in by_source:raise ValueError('multiple wraps on one source unsupported')
        by_source[route['source_mesh']]=route
    ordered=[];finished=set();visiting=set()
    def visit(mid):
        if mid in finished:return
        if mid in visiting:raise ValueError('cyclic wrap dependency')
        visiting.add(mid);target=by_source[mid]['target_mesh']
        if target in by_source:visit(target)
        visiting.remove(mid);finished.add(mid);ordered.append(by_source[mid])
    for mid in by_source:visit(mid)
    return dict(schema='hone.shrinkwrap-route-plan.v1',runtime_ready=False,source_sha256=package['source_sha256'],
                routes=ordered,pending=['live source-to-target transforms and offset units',
                    'target deformation and modifier order','mesh-local wrap output and normals',
                    'rig and snapshot atomicity','target memory and performance'])


if __name__=='__main__':
    if len(sys.argv)!=4:raise SystemExit('expected source package, hierarchy snapshot and new output')
    p,h=(json.loads(pathlib.Path(x).read_text(encoding='utf8')) for x in sys.argv[1:3]);result=plan(p,h)
    with pathlib.Path(sys.argv[3]).open('x',encoding='utf8',newline='\n') as out:json.dump(result,out,separators=(',',':'),allow_nan=False)
    print(json.dumps(dict(runtime_ready=False,routes=len(result['routes']),source_vertices=sum(x['source_vertices'] for x in result['routes']),target_triangles=sum(x['target_triangles'] for x in result['routes']))))
