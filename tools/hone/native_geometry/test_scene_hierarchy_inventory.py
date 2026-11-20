"""Run in Blender; synthetic input only."""
import hashlib
import pathlib
import tempfile
import bpy
from scene_hierarchy_inventory import capture

with tempfile.TemporaryDirectory() as tmp:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    parent=bpy.data.objects.new('synthetic-parent',None)
    bpy.context.scene.collection.objects.link(parent)
    mesh=bpy.data.objects.new('synthetic-mesh',bpy.data.meshes.new('mesh'))
    bpy.context.scene.collection.objects.link(mesh)
    mesh.parent=parent
    parent.location=(1,2,3);mesh.location=(4,5,6)
    parent['synthetic-control']=0.5
    source=pathlib.Path(tmp)/'synthetic.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(source))
    digest=hashlib.sha256(source.read_bytes()).hexdigest()
    package=dict(schema='hone.source-preservation.v1',runtime_ready=False,source_sha256=digest,
                 meshes=[dict(name='synthetic-mesh')])
    bindings=dict(schema='hone.driver-bindings.v1',runtime_ready=False,source_sha256=digest,
                  inputs=[dict(id=dict(type='Object',name='synthetic-parent'),data_path='["synthetic-control"]')])
    result=capture(package,bindings)
    assert len(result['meshes'])==1 and len(result['parents'])==1 and len(result['controls'])==1
    assert result['meshes'][0]['parent']=='synthetic-parent'
    constraint=mesh.constraints.new('COPY_LOCATION');constraint.target=parent
    try:
        capture(package,bindings)
        raise AssertionError('active constraints must reject')
    except ValueError:
        pass
    bindings['source_sha256']='wrong'
    try:
        capture(package,bindings)
        raise AssertionError('source mismatch must reject')
    except ValueError:
        pass
print('PASS synthetic Blender hierarchy, controls and unsupported constraints')
