"""Compare mesh-to-rig transforms across supplied controls in trusted Blender.

Run with --disable-autoexec; this is sampled evidence, never runtime admission.
The inputs and source package must refer to the currently opened model.
"""
import hashlib,json,math,pathlib,sys
import bpy

sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'source_package'))
import driver_inventory as inventory

args=sys.argv[sys.argv.index('--')+1:]
if len(args)!=4:
    raise SystemExit('expected source package, driver bindings, input samples, new output')
package,bindings,samples=(json.loads(pathlib.Path(p).read_text(encoding='utf8')) for p in args[:3])
source=pathlib.Path(bpy.data.filepath)
digest=hashlib.sha256(source.read_bytes()).hexdigest()
if (package.get('schema')!='hone.source-preservation.v1' or package.get('source_sha256')!=digest or
    bindings.get('schema')!='hone.driver-bindings.v1' or bindings.get('source_sha256')!=digest or
    package.get('runtime_ready') is not False or bindings.get('runtime_ready') is not False):
    raise ValueError('source identity')
if not samples or len(package['armatures'])!=1:raise ValueError('samples or rig')
blocks=[*bpy.data.objects,*bpy.data.shape_keys,*bpy.data.materials,*bpy.data.node_groups]
blocks.extend(m.node_tree for m in bpy.data.materials if m.node_tree)
blocks=list({b.as_pointer():b for b in blocks}.values());targets={}
def key(value):return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=False)
for block in blocks:
    if block.animation_data:
        for curve in block.animation_data.drivers:
            for variable in curve.driver.variables:
                if variable.type!='SINGLE_PROP' or len(variable.targets)!=1:raise ValueError('unsupported driver')
                target=variable.targets[0]
                targets[key({'id':inventory.identity(target.id),'data_path':target.data_path})]=target
if len(targets)!=len(bindings['inputs']):raise ValueError('driver input mismatch')
rig=bpy.context.scene.objects[package['armatures'][0]['name']]
meshes=[m for m in package['meshes'] if any(x['type']=='ARMATURE' and x['viewport'] for x in m['modifiers'])]
if not meshes:raise ValueError('no rigged meshes')
baseline=None;maximum=0.;changed=0
for frame in samples:
    if (len(frame)!=len(bindings['inputs']) or
        any(type(value) not in (int,float) or not math.isfinite(value) for value in frame)):
        raise ValueError('input sample size or value')
    for binding,value in zip(bindings['inputs'],frame):
        target=targets[key(binding)];prop=json.loads(target.data_path[1:-1]);target.id[prop]=value;target.id.update_tag()
    bpy.context.view_layer.update();graph=bpy.context.evaluated_depsgraph_get();graph.update()
    rig_matrix=rig.evaluated_get(graph).matrix_world
    transforms=[]
    for mesh in meshes:
        matrix=rig_matrix.inverted()@bpy.context.scene.objects[mesh['name']].evaluated_get(graph).matrix_world
        flat=[matrix[r][c] for r in range(4) for c in range(4)]
        if not all(math.isfinite(value) for value in flat):raise ValueError('nonfinite transform')
        transforms.append(flat)
    if baseline is None:baseline=transforms
    for current,first in zip(transforms,baseline):
        delta=max(abs(a-b) for a,b in zip(current,first));maximum=max(maximum,delta);changed+=delta!=0
if hashlib.sha256(source.read_bytes()).hexdigest()!=digest:raise ValueError('source changed')
report={'schema':'hone.rig-binding-sweep.v1','runtime_ready':False,'source_sha256':digest,
        'inputs_sha256':hashlib.sha256(pathlib.Path(args[2]).read_bytes()).hexdigest(),
        'samples':len(samples),'rigged_meshes':len(meshes),'changed_sample_meshes':changed,'max_delta':maximum}
with pathlib.Path(args[3]).open('x',encoding='utf8') as out:json.dump(report,out,allow_nan=False)
print(json.dumps(report,allow_nan=False))
