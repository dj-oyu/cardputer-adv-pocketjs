"""Snapshot a restricted root-bone rig in Blender; no runtime admission."""
import bpy,hashlib,json,math,pathlib,sys
from mathutils import Matrix


def capture(package):
    if package.get('schema')!='hone.source-preservation.v1' or package.get('runtime_ready') is not False:
        raise ValueError('source package')
    source=pathlib.Path(bpy.data.filepath)
    if not source.is_file():raise ValueError('saved Blender source required')
    digest=hashlib.sha256(source.read_bytes()).hexdigest()
    if package.get('source_sha256')!=digest:raise ValueError('source identity')
    if len(package['armatures'])!=1:raise ValueError('exactly one armature required')
    rig=bpy.context.scene.objects.get(package['armatures'][0]['name'])
    if rig is None or rig.type!='ARMATURE':raise ValueError('armature missing')
    graph=bpy.context.evaluated_depsgraph_get();evaluated=rig.evaluated_get(graph)
    bones=[]
    for source_bone in package['armatures'][0]['bones']:
        p=evaluated.pose.bones.get(source_bone['name'])
        if (p is None or p.parent is not None or not p.bone.use_deform or
            any(not c.mute and c.influence!=0 for c in p.constraints) or
            p.rotation_mode not in ('XYZ','QUATERNION') or any(abs(v-1)>1e-7 for v in p.scale)):
            raise ValueError('unsupported bone hierarchy, constraint, mode or scale')
        rest=p.bone.matrix_local.copy();quat=rest.to_quaternion()
        reconstructed=Matrix.LocRotScale(rest.translation,quat,(1,1,1))
        if max(abs(rest[r][c]-reconstructed[r][c]) for r in range(4) for c in range(4))>1e-6:
            raise ValueError('non-rigid rest bone')
        rotation=list(p.rotation_euler)+[0] if p.rotation_mode=='XYZ' else list(p.rotation_quaternion)
        values=list(quat)+list(rest.translation)+list(p.location)+rotation
        if any(not math.isfinite(v) for v in values):raise ValueError('nonfinite bone')
        bones.append(dict(name=p.name,rest_q=list(quat),rest_t=list(rest.translation),location=list(p.location),
                          rotation=rotation,mode=p.rotation_mode))
    names={b['name']:i for i,b in enumerate(bones)}
    if len(names)!=len(bones) or not bones:raise ValueError('duplicate or empty bones')
    bindings=[]
    for mid,mesh in enumerate(package['meshes']):
        active=[(i,m) for i,m in enumerate(mesh['modifiers']) if m['type']=='ARMATURE' and m['viewport']]
        if not active:continue
        if len(active)!=1:raise ValueError('multiple armatures')
        index,mod=active[0]
        if (mod['object']!=rig.name or mod['render'] is not True or mod['preserve_volume'] is not True or
            mod['vertex_groups'] is not True or mod['envelopes'] is not False or
            any(m['viewport'] for m in mesh['modifiers'][:index])):
            raise ValueError('unsupported armature modifier')
        if any(name not in names for name in mesh['vertex_groups']):raise ValueError('unmapped vertex group')
        obj=bpy.context.scene.objects.get(mesh['name'])
        if obj is None or obj.type!='MESH':raise ValueError('rigged mesh missing')
        try:
            to_rig=evaluated.matrix_world.inverted()@obj.evaluated_get(graph).matrix_world
            from_rig=to_rig.inverted()
        except ValueError as error:raise ValueError('noninvertible mesh/rig transform') from error
        bindings.append(dict(source_mesh=mid,modifier_index=index,group_to_joint=[names[n] for n in mesh['vertex_groups']],
                             to_rig=[to_rig[r][c] for r in range(3) for c in range(4)],
                             from_rig=[from_rig[r][c] for r in range(3) for c in range(4)]))
    if hashlib.sha256(source.read_bytes()).hexdigest()!=digest:raise ValueError('source changed')
    return dict(schema='hone.root-rig-snapshot.v1',runtime_ready=False,source_sha256=digest,frame=bpy.context.scene.frame_current,
                rig_object=rig.name,bones=bones,meshes=bindings,
                pending=['same-frame bone property drivers and mesh/rig transforms','modifier ordering',
                         'rig and surface deformation validation','frame atomicity and device budget'])


if __name__=='__main__':
    args=sys.argv[sys.argv.index('--')+1:]
    if len(args)!=2:raise SystemExit('expected source package and new output')
    result=capture(json.loads(pathlib.Path(args[0]).read_text(encoding='utf8')))
    with pathlib.Path(args[1]).open('x',encoding='utf8',newline='\n') as out:json.dump(result,out,separators=(',',':'),allow_nan=False)
    print(json.dumps(dict(runtime_ready=False,bones=len(result['bones']),rigged_meshes=len(result['meshes']))))
