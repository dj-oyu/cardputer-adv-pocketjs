"""Plan a source-bound AYNM node snapshot and provider map, not a runtime model."""
import json
import math
import pathlib
import sys
from provider_binding import binding_plan
from provider_node_map import emit as emit_provider_map


def plan(package, hierarchy):
    if (hierarchy.get('schema') != 'hone.scene-hierarchy-snapshot.v1' or
        hierarchy.get('runtime_ready') is not False or
        hierarchy.get('source_sha256') != package.get('source_sha256')):
        raise ValueError('hierarchy source identity')
    binding = binding_plan(package)
    meshes = hierarchy['meshes']
    if len(meshes) != len(package['meshes']) or any(m['name'] != p['name'] for m,p in zip(meshes,package['meshes'])):
        raise ValueError('mesh order or identity mismatch')
    parents = hierarchy['parents']
    by_name = {p['name']: p for p in parents}
    if len(by_name) != len(parents):
        raise ValueError('duplicate parent')
    nodes, assignment, group_ids = [], [], {}
    def pose(row):
        values=row.get('pose')
        if (not isinstance(values,list) or len(values)!=9 or
            any(type(v) not in (int,float) or not math.isfinite(v) for v in values) or
            any(v==0 for v in values[6:])):
            raise ValueError('invalid pose')
        if type(row.get('hide_render')) is not bool:
            raise ValueError('missing visibility')
        return values
    def add_group(name, visiting):
        if name in group_ids:
            return group_ids[name]
        if name in visiting or name not in by_name:
            raise ValueError('missing or cyclic parent')
        visiting.add(name)
        row=by_name[name]
        parent_id=add_group(row['parent'],visiting) if row['parent'] is not None else -1
        visiting.remove(name)
        index=len(nodes);group_ids[name]=index
        # Blender hide_render is per object. This transform-only group has no
        # geometry, while disabling it in Ayatsuri hides all its descendants.
        nodes.append(dict(index=index,kind='group',parent=parent_id,pose=pose(row),enabled=True,source_object=name))
        assignment.append(None)
        return index
    for row in meshes:
        if row['parent'] is not None:
            add_group(row['parent'],set())
    if len(group_ids)!=len(parents):
        raise ValueError('unused parent inventory')
    for ordinal, part in enumerate(binding['nodes']):
        mid=part['source_mesh'];row=meshes[mid]
        index=len(nodes)
        nodes.append(dict(index=index,kind='gouraudMesh',parent=group_ids[row['parent']] if row['parent'] is not None else -1,
                          pose=pose(row),enabled=not row['hide_render'],source_mesh=mid,part=ordinal))
        assignment.append(ordinal)
    if len(nodes)>256:
        raise ValueError('model node limit exceeded')
    controls=hierarchy.get('controls')
    if not isinstance(controls,list):raise ValueError('missing controls')
    return dict(schema='hone.scene-node-plan.v1', runtime_ready=False,
                source_sha256=package.get('source_sha256'),frame=hierarchy['frame'],
                nodes=nodes,provider_assignments=assignment,controls=controls,
                armature_bones=hierarchy['armature_bones'],
                pending=['live controls, animated transforms and modifier ordering',
                         'rig palette and skinning', 'visibility drivers and materials',
                         'camera, light and alpha mapping', 'memory and model admission'])


def build(source, snapshot, destination):
    package=json.loads(pathlib.Path(source).read_text(encoding='utf8'))
    hierarchy=json.loads(pathlib.Path(snapshot).read_text(encoding='utf8'))
    result=plan(package,hierarchy)
    header=emit_provider_map(package,result['provider_assignments'])
    dest=pathlib.Path(destination);dest.mkdir(parents=True,exist_ok=False)
    (dest/'scene_nodes.private.json').write_text(json.dumps(result,separators=(',',':'),allow_nan=False),encoding='utf8')
    (dest/'provider_nodes.private.h').write_text(header,encoding='utf8')
    return dict(runtime_ready=False,nodes=len(result['nodes']),geometry_nodes=sum(n['kind']=='gouraudMesh' for n in result['nodes']),
                control_inputs=len(result['controls']),armature_bones=result['armature_bones'])


if __name__=='__main__':
    if len(sys.argv)!=4:raise SystemExit('expected source package, hierarchy snapshot, new output directory')
    print(json.dumps(build(*sys.argv[1:])))
