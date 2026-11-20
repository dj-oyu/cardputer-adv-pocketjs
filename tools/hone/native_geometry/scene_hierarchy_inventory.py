"""Blender evaluated scene snapshot for node planning; not runtime admission.

Run on a separately opened source file with Blender's autoexec disabled. The
output can contain source names and transforms; keep it with the source asset.
"""
import bpy
import collections
import hashlib
import json
import math
import pathlib
import sys
from mathutils import Matrix


def capture(package, bindings):
    if package.get('schema') != 'hone.source-preservation.v1' or package.get('runtime_ready') is not False:
        raise ValueError('source package')
    if bindings.get('schema') != 'hone.driver-bindings.v1' or bindings.get('runtime_ready') is not False:
        raise ValueError('driver bindings')
    source = pathlib.Path(bpy.data.filepath)
    if not source.is_file():
        raise ValueError('saved source required')
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if package.get('source_sha256') != digest or bindings.get('source_sha256') != digest:
        raise ValueError('source identity mismatch')
    scene = bpy.context.scene
    graph = bpy.context.evaluated_depsgraph_get()
    objects = {o.name: o for o in scene.objects}
    meshes = []
    ancestors = set()
    for mesh in package['meshes']:
        obj = objects.get(mesh['name'])
        if obj is None or obj.type != 'MESH':
            raise ValueError('missing source mesh')
        meshes.append(obj)
        parent = obj.parent
        seen = {obj.name}
        while parent is not None:
            if parent.name in seen or parent.name not in objects:
                raise ValueError('parent cycle or out-of-scene parent')
            seen.add(parent.name)
            if parent.type != 'EMPTY':
                raise ValueError('unsupported parent type')
            ancestors.add(parent.name)
            parent = parent.parent
    def record(obj):
        if obj.parent and obj.parent_type != 'OBJECT':
            raise ValueError('unsupported parenting')
        if any(not c.mute and c.influence != 0 for c in obj.constraints):
            raise ValueError('active object constraint')
        evaluated = obj.evaluated_get(graph)
        world = evaluated.matrix_world.copy()
        parent = obj.parent.evaluated_get(graph).matrix_world if obj.parent else Matrix.Identity(4)
        try:
            local = parent.inverted() @ world
        except ValueError as error:
            raise ValueError('noninvertible parent') from error
        pos, quat, scale = local.decompose()
        euler = quat.to_euler('XYZ')
        rebuilt = Matrix.LocRotScale(pos, euler, scale)
        error = max(abs(local[r][c] - rebuilt[r][c]) for r in range(4) for c in range(4))
        pose = list(pos) + list(euler) + list(scale)
        if any(not math.isfinite(v) for v in pose) or any(v == 0 for v in scale) or error > 1e-5:
            raise ValueError('local matrix cannot be represented by TRS')
        result = dict(name=obj.name, parent=obj.parent.name if obj.parent else None,
                      pose=pose, local_error=error, hide_render=bool(obj.hide_render))
        # Only expose direct XYZ property updates when Blender's basis can be
        # represented without parent-inverse or delta-transform conversion.
        identity = Matrix.Identity(4)
        inverse_error = max(abs(obj.matrix_parent_inverse[r][c]-identity[r][c]) for r in range(4) for c in range(4))
        delta = list(evaluated.delta_location) + list(evaluated.delta_rotation_euler) + [v-1 for v in evaluated.delta_scale]
        direct = Matrix.LocRotScale(evaluated.location, evaluated.rotation_euler, evaluated.scale)
        direct_error = max(abs(local[r][c]-direct[r][c]) for r in range(4) for c in range(4))
        result['direct_xyz_property_pose'] = bool(obj.rotation_mode=='XYZ' and inverse_error<1e-7 and
                                                   all(abs(v)<1e-7 for v in delta) and direct_error<1e-6)
        if obj.type == 'MESH':
            result['shape_key_id'] = obj.data.shape_keys.name if obj.data.shape_keys else None
        return result
    inputs = bindings['inputs']
    for item in inputs:
        ident = item.get('id', {})
        obj = objects.get(ident.get('name')) if ident.get('type') == 'Object' else None
        if obj is None or not isinstance(item.get('data_path'), str):
            raise ValueError('unsupported control target')
        try:
            value = obj.path_resolve(item['data_path'])
        except (ValueError, AttributeError) as error:
            raise ValueError('unresolved control target') from error
        if type(value) not in (int, float, bool) or not math.isfinite(value):
            raise ValueError('non-scalar control')
    types = collections.Counter(o.type for o in scene.objects)
    result = dict(schema='hone.scene-hierarchy-snapshot.v1', runtime_ready=False,
                source_sha256=digest, frame=scene.frame_current,
                meshes=[record(o) for o in meshes], parents=[record(objects[n]) for n in sorted(ancestors)],
                controls=inputs, scene_object_types=dict(types),
                scene_objects=[dict(name=o.name,type=o.type) for o in scene.objects],
                armature_bones=sum(len(o.data.bones) for o in scene.objects if o.type == 'ARMATURE'),
                pending=['live control binding and frame freezing', 'rig and modifier evaluation',
                         'animated pose equivalence', 'camera, lights, materials and alpha'])
    if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
        raise ValueError('source changed during snapshot')
    return result


if __name__ == '__main__':
    args = sys.argv[sys.argv.index('--')+1:]
    if len(args) != 3:
        raise SystemExit('expected source package, driver bindings and new snapshot')
    package, bindings = (json.loads(pathlib.Path(x).read_text(encoding='utf8')) for x in args[:2])
    result = capture(package, bindings)
    with pathlib.Path(args[2]).open('x', encoding='utf8', newline='\n') as out:
        json.dump(result, out, separators=(',', ':'), allow_nan=False)
    print(json.dumps(dict(runtime_ready=False, meshes=len(result['meshes']),
                          parent_nodes=len(result['parents']), controls=len(result['controls']),
                          armature_bones=result['armature_bones'])))
