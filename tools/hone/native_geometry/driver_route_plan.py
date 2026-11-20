"""Route every generated driver output to scene identities; no runtime admission."""
import collections
import json
import pathlib
import sys


def plan(hierarchy, scene, bindings):
    if (hierarchy.get('schema')!='hone.scene-hierarchy-snapshot.v1' or
        scene.get('schema')!='hone.scene-node-plan.v1' or
        bindings.get('schema')!='hone.driver-bindings.v1' or
        any(x.get('runtime_ready') is not False for x in (hierarchy,scene,bindings))):
        raise ValueError('unsupported inputs')
    if (not hierarchy.get('source_sha256') or
        any(x.get('source_sha256')!=hierarchy['source_sha256'] for x in (scene,bindings)) or
        hierarchy.get('controls')!=bindings.get('inputs')):
        raise ValueError('source or input identity mismatch')
    mesh_names={m['name']:i for i,m in enumerate(hierarchy['meshes'])}
    if len(mesh_names)!=len(hierarchy['meshes']):raise ValueError('duplicate mesh')
    objects={o['name']:o['type'] for o in hierarchy['scene_objects']}
    if len(objects)!=len(hierarchy['scene_objects']):raise ValueError('duplicate object')
    keys=collections.defaultdict(list)
    for i,m in enumerate(hierarchy['meshes']):
        if m.get('shape_key_id') is not None:keys[m['shape_key_id']].append(i)
    groups={n['source_object']:n['index'] for n in scene['nodes'] if n['kind']=='group'}
    parent_rows={p['name']:p for p in hierarchy['parents']}
    mesh_nodes=collections.defaultdict(list)
    for node in scene['nodes']:
        if node['kind']=='gouraudMesh':mesh_nodes[node['source_mesh']].append(node['index'])
    routes=[]
    for index,program in enumerate(bindings['programs']):
        if program.get('program')!=index:raise ValueError('program order')
        owner=program.get('owner',{});kind=owner.get('type');name=owner.get('name');path=program.get('data_path');array_index=program.get('array_index')
        if not isinstance(path,str) or type(array_index) is not int or array_index<0:raise ValueError('invalid driven property')
        route=dict(program=index,owner=owner,data_path=path,array_index=array_index,status='requires_live_adapter')
        if kind=='Object':
            if name not in objects:raise ValueError('missing object owner')
            if name in mesh_names and path=='hide_render':
                mesh=mesh_names[name];route.update(kind='mesh_visibility',source_mesh=mesh,node_ids=mesh_nodes[mesh])
                if not route['node_ids']:raise ValueError('visibility target has no geometry nodes')
            elif name in groups and path in ('location','rotation_euler','scale'):
                route.update(kind='parent_transform',node_ids=[groups[name]],
                             direct_xyz_property_pose=parent_rows[name].get('direct_xyz_property_pose') is True)
            else:
                route.update(kind='external_object_or_rig',object_type=objects[name])
        elif kind=='Key':
            if name not in keys:raise ValueError('shape key owner not bound to mesh')
            route.update(kind='shape_key',source_meshes=keys[name])
        elif kind=='ShaderNodeTree':
            route.update(kind='shader_property')
        else:
            route.update(kind='unsupported_owner')
        routes.append(route)
    return dict(schema='hone.driver-route-plan.v1',runtime_ready=False,source_sha256=hierarchy['source_sha256'],
                routes=routes,counts=dict(collections.Counter(x['kind'] for x in routes)),
                pending=['freeze inputs in dependency order','convert Blender local transform semantics',
                         'apply mesh visibility to every partition','shape-key value and masks',
                         'rig and external object outputs','shader property and alpha semantics',
                         'frame atomicity and production binding'])


if __name__=='__main__':
    if len(sys.argv)!=5:raise SystemExit('expected hierarchy, scene plan, driver bindings, new output')
    h,s,b=(json.loads(pathlib.Path(x).read_text(encoding='utf8')) for x in sys.argv[1:4])
    result=plan(h,s,b)
    with pathlib.Path(sys.argv[4]).open('x',encoding='utf8',newline='\n') as out:
        json.dump(result,out,separators=(',',':'),allow_nan=False)
    print(json.dumps(dict(runtime_ready=False,programs=len(result['routes']),counts=result['counts'])))
