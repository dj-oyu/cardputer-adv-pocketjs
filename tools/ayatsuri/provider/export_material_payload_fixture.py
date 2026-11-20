"""Offline private route export. All asset inputs/outputs stay caller-private."""
import hashlib,json,pathlib,sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[2]/'hone/native_geometry'))
from provider_partition import plan as partition
from material_route_plan import plan as material_plan
from alpha_factor_compile import emit as alpha_emit
from base_color_compile import emit as color_emit

def export(source,source_bytes,inventory,drivers,scene):
    digest=hashlib.sha256(source_bytes).hexdigest()
    if any(x.get('source_sha256')!=digest for x in (source,inventory,drivers,scene)):
        raise ValueError('actual source bytes differ')
    materials=material_plan(source,inventory,drivers)
    parts=partition(source)['mesh_parts'];flat=[(mid,p) for mid,group in enumerate(parts) for p in group];ids={m['name']:i for i,m in enumerate(inventory['materials'])}
    offsets=[];polygon_material=[]
    for m in source['meshes']:
        offsets.append(len(polygon_material))
        polygon_material.extend(ids[m['material_slots'][p['material']]] for p in m['polygons'])
    nodes=[n for n in scene['nodes'] if n['kind']=='gouraudMesh'];seen=set();rows=[]
    out=['/* Private generated route fixture; not runtime admission. */\n']
    for i,n in enumerate(nodes):
        mid,part=n['source_mesh'],n['part']
        if (mid,part) in seen:raise ValueError('duplicate partition')
        seen.add((mid,part));expected_mid,p=flat[part]
        if mid!=expected_mid:raise ValueError('partition mesh')
        mesh=source['meshes'][mid]
        faces=[]
        for tid in p['source_triangles']:
            poly=offsets[mid]+mesh['triangles'][tid]['polygon'];faces.append('{%d,%d}'%(poly,polygon_material[poly]))
        out.append('static const ay_material_face private_faces_%d[]={%s};\n'%(i,','.join(faces)))
        out.append('static const uint16_t private_indices_%d[]={%s};\n'%(i,','.join(str(v) for t in p['triangles'] for v in t)))
        rows.append('{private_faces_%d,%d}'%(i,len(faces)))
    if len(seen)!=sum(map(len,parts)):raise ValueError('missing partition')
    out.append('static const ay_material_mesh private_meshes[]={%s};\n'%','.join(rows))
    out.append('static const uint16_t*private_indices[]={%s};\n'%','.join('private_indices_%d'%i for i in range(len(nodes))))
    out.append('static const uint16_t private_nodes[]={%s};\n'%','.join(str(n['index']) for n in nodes))
    out.append('static const uint32_t private_polygons[]={%s};\n'%','.join(map(str,polygon_material)))
    out.append('static const uint8_t private_hash[32]={%s};\n'%','.join(str(v) for v in bytes.fromhex(digest)))
    alpha,_=alpha_emit(inventory,materials);color,_=color_emit(inventory,materials)
    return ''.join(out),alpha,color,dict(source_bytes_verified=True,meshes=len(nodes),polygons=len(polygon_material),materials=len(ids),shader_routes=len(materials['shader_routes']),runtime_ready=False)

if __name__=='__main__':
    if len(sys.argv)!=7:raise SystemExit('source-json source-blend inventory driver-routes scene-plan new-out')
    s,blend,inv,drivers,scene,out=map(pathlib.Path,sys.argv[1:])
    load=lambda p:json.loads(p.read_text(encoding='utf8'))
    route,alpha,color,summary=export(load(s),blend.read_bytes(),load(inv),load(drivers),load(scene))
    out.mkdir(parents=True,exist_ok=False)
    for name,data in [('material_routes.h',route),('material_alpha.h',alpha),('material_color.h',color)]:
        (out/name).write_text(data)
    print(json.dumps(summary))
