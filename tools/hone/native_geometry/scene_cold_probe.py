"""Build a diagnostic provider manifest, with explicit placeholder materials.

This tests geometry/node wire limits only. It must not be presented as the
model, flashed, or used as a material/alpha/animation correctness result.
"""
import json
import pathlib
import sys
from consumer_plan import prepare
from provider_binding import binding_plan


def build(package,scene):
    if (scene.get('schema')!='hone.scene-node-plan.v1' or scene.get('runtime_ready') is not False or
        not package.get('source_sha256') or scene.get('source_sha256')!=package.get('source_sha256')):
        raise ValueError('scene/source identity')
    geometry=prepare(package)['meshes'];parts=binding_plan(package)['nodes'];nodes=[];seen=[]
    for index,row in enumerate(scene['nodes']):
        if row['index']!=index or type(row['parent']) is not int or not -1<=row['parent']<index:
            raise ValueError('node order')
        node=dict(index=index,name='probe_%d'%index,parent=row['parent'],kind=row['kind'],
                  enabled=row['enabled'],pose=row['pose'])
        if row['kind']=='gouraudMesh':
            ordinal=row['part']
            if type(ordinal) is not int or not 0<=ordinal<len(parts):raise ValueError('part ordinal')
            part=parts[ordinal];mid=part['source_mesh']
            if row['source_mesh']!=mid:raise ValueError('part mesh mismatch')
            mesh=geometry[mid];indices=part['prepared_vertices'];seen.append(ordinal)
            node.update(material=[.25,.75,0,4,0,0,0,0,0,0,0],color=0,thickness=1,opacity=1,edgeSoftness=0,
                mesh=dict(vertices=[v for i in indices for v in mesh['positions'][i]],
                          normals=[v for i in indices for v in mesh['rest_corner_normals'][i]],
                          mask=[-1]*len(indices),indices=[v for tri in part['triangles'] for v in tri],
                          holes=[],order='authored',albedoA=0xffff,albedoB=0xffff,thickness=255))
        elif row['kind']!='group':raise ValueError('unsupported node kind')
        nodes.append(node)
    if sorted(seen)!=list(range(len(parts))):raise ValueError('missing or duplicate geometry')
    return dict(probe_kind='hone-provider-cold-only-v1',runtime_ready=False,
                warnings=['PLACEHOLDER MATERIALS: never flash or present as final model',
                          'static snapshot only; no controls, rig, shrinkwrap or shader'],
                payload=dict(schema='ayatsuri-native-model-design-v1',nodes=nodes))


if __name__=='__main__':
    if len(sys.argv)!=4:raise SystemExit('expected source package, scene plan, new manifest')
    package,scene=(json.loads(pathlib.Path(x).read_text(encoding='utf8')) for x in sys.argv[1:3])
    result=build(package,scene)
    with pathlib.Path(sys.argv[3]).open('x',encoding='utf8',newline='\n') as out:
        json.dump(result,out,separators=(',',':'),allow_nan=False)
    print(json.dumps(dict(runtime_ready=False,nodes=len(result['payload']['nodes']),materials='diagnostic placeholders only')))
