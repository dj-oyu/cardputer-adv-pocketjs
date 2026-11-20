"""Emit sampled root-rig tables; live rig/mesh transforms remain required."""
import ast,json,math,pathlib,re,sys


def emit(rig,route_plan):
    if (rig.get('schema')!='hone.root-rig-snapshot.v1' or route_plan.get('schema')!='hone.driver-route-plan.v1' or
        rig.get('runtime_ready') is not False or route_plan.get('runtime_ready') is not False or
        not rig.get('source_sha256') or rig.get('source_sha256')!=route_plan.get('source_sha256')):
        raise ValueError('source identity')
    bones=rig['bones'];names={b['name']:i for i,b in enumerate(bones)}
    if not bones or len(bones)>65536 or len(names)!=len(bones):raise ValueError('bone inventory')
    def values(row,n):
        if not isinstance(row,list) or len(row)!=n or any(type(x) not in (int,float) or not math.isfinite(x) for x in row):raise ValueError('numeric span')
        return '{'+','.join(float(x).hex() for x in row)+'}'
    bone_rows=[]
    for b in bones:
        if b['mode'] not in ('XYZ','QUATERNION'):raise ValueError('bone mode')
        bone_rows.append('{'+','.join([values(b['rest_q'],4),values(b['rest_t'],3),values(b['location'],3),values(b['rotation'],4),str(int(b['mode']=='QUATERNION'))])+'}')
    routes=[];unbound=0;seen=set()
    for r in route_plan['routes']:
        owner=r['owner'];path=r['data_path']
        if r['kind']!='external_object_or_rig' or owner.get('type')!='Object' or owner.get('name')!=rig['rig_object'] or not path.startswith('pose.bones['):
            unbound+=1;continue
        match=re.fullmatch(r'pose\.bones\[("(?:[^"\\]|\\.)*")\]\.rotation_euler',path)
        if not match:raise ValueError('unsupported bone driver')
        try:name=ast.literal_eval(match.group(1))
        except (ValueError,SyntaxError) as error:raise ValueError('bone name') from error
        axis=r['array_index'];program=r['program']
        if (name not in names or bones[names[name]]['mode']!='XYZ' or type(axis) is not int or not 0<=axis<3 or
            type(program) is not int or not 0<=program<=65535 or (names[name],axis) in seen):raise ValueError('bone driver route')
        seen.add((names[name],axis));routes.append((program,names[name],axis))
    out=['/* Generated snapshot: recompute mesh/rig space transforms each frame. */\n',
         '#ifndef HONE_PRIVATE_ROOT_RIG_H\n#define HONE_PRIVATE_ROOT_RIG_H\n#include "rig_root_palette_candidate.h"\n',
         'static const hone_root_bone hone_root_bones[] = {'+','.join(bone_rows)+'};\n']
    mesh_rows=[];seen_meshes=set()
    for i,m in enumerate(rig['meshes']):
        mesh=m['source_mesh'];groups=m['group_to_joint']
        if type(mesh) is not int or mesh<0 or mesh in seen_meshes or not isinstance(groups,list) or not groups or any(type(j) is not int or not 0<=j<len(bones) for j in groups):raise ValueError('rig mesh binding')
        seen_meshes.add(mesh);name='hone_root_groups_%d'%i
        out.append('static const uint32_t %s[] = {%s};\n'%(name,','.join(map(str,groups))))
        mesh_rows.append('{%d,%s,%d,%s,%s}'%(mesh,name,len(groups),values(m['to_rig'],12),values(m['from_rig'],12)))
    if not mesh_rows:raise ValueError('missing rigged mesh')
    out.append('static const hone_root_mesh_binding hone_root_meshes[] = {'+','.join(mesh_rows)+'};\n')
    out.append('static const hone_root_driver_route hone_root_routes[] = {'+(','.join('{%d,%d,%d}'%x for x in routes) if routes else '{0,0,0}')+'};\n')
    out.append('static const size_t hone_root_bone_count=%d,hone_root_mesh_count=%d,hone_root_route_count=%d;\n#endif\n'%(len(bones),len(mesh_rows),len(routes)))
    return ''.join(out),dict(runtime_ready=False,bones=len(bones),rigged_meshes=len(mesh_rows),bone_driver_routes=len(routes),unbound_programs=unbound)


if __name__=='__main__':
    if len(sys.argv)!=4:raise SystemExit('expected root rig snapshot, driver routes and new header')
    rig,routes=(json.loads(pathlib.Path(x).read_text(encoding='utf8')) for x in sys.argv[1:3]);header,summary=emit(rig,routes)
    with pathlib.Path(sys.argv[3]).open('x',encoding='utf8',newline='\n') as out:out.write(header)
    print(json.dumps(summary))
