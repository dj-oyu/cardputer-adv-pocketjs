"""Run in Blender on a synthetic root rig."""
import hashlib,pathlib,tempfile,bpy
from rig_root_inventory import capture

with tempfile.TemporaryDirectory() as tmp:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.object.armature_add();rig=bpy.context.object;rig.name='synthetic-rig'
    bone=rig.data.bones[0];bone.use_deform=True
    mesh=bpy.data.objects.new('synthetic-mesh',bpy.data.meshes.new('synthetic-mesh'))
    bpy.context.scene.collection.objects.link(mesh);mesh.vertex_groups.new(name=bone.name)
    mod=mesh.modifiers.new('synthetic-skin','ARMATURE');mod.object=rig;mod.use_deform_preserve_volume=True
    bpy.context.view_layer.update()
    source=pathlib.Path(tmp)/'synthetic-rig.blend';bpy.ops.wm.save_as_mainfile(filepath=str(source))
    digest=hashlib.sha256(source.read_bytes()).hexdigest()
    package=dict(schema='hone.source-preservation.v1',runtime_ready=False,source_sha256=digest,
        armatures=[dict(name=rig.name,bones=[dict(name=bone.name)])],meshes=[dict(name=mesh.name,vertex_groups=[bone.name],
            modifiers=[dict(type='ARMATURE',viewport=True,render=True,object=rig.name,
                preserve_volume=True,vertex_groups=True,envelopes=False)])])
    result=capture(package)
    assert len(result['bones'])==1 and len(result['meshes'])==1 and result['meshes'][0]['group_to_joint']==[0]
    rig.pose.bones[0].scale=(2,1,1);rig.update_tag();bpy.context.view_layer.update()
    try:
        capture(package)
        raise AssertionError('bone scale must reject')
    except ValueError:
        pass
print('PASS synthetic Blender root rig and unsupported scale rejection')
