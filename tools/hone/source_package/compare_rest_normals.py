"""Compare an experimental native normal DLL to a private Blender export.

Print aggregate error only. This measures rest positions, not animated poses,
and must not be treated as runtime admission or physical-device evidence.
"""
import argparse, ctypes as c, json, math, pathlib


def compare(package, library, tolerance):
    f=c.CDLL(str(pathlib.Path(library).resolve())).polygon_normals
    f.argtypes=[c.POINTER(c.c_float),c.c_size_t,c.POINTER(c.c_uint32),
                c.POINTER(c.c_uint32),c.POINTER(c.c_uint8),c.c_size_t,c.c_size_t,
                c.POINTER(c.c_double),c.POINTER(c.c_float)]
    worst=angle=0.; violations=total=rejected=0
    for mesh in package['meshes']:
        vertices=mesh['vertices']; polys=mesh['polygons']; nv=len(vertices)
        count=sum(len(q['vertices']) for q in polys)
        if not nv or not count: continue
        coords=(c.c_float*(nv*3))(*(x for v in vertices for x in v))
        offsets=[0]; indices=[]; loops=[]
        for face in polys:
            indices.extend(face['vertices']); loops.extend(face['loops']); offsets.append(len(indices))
        off=(c.c_uint32*len(offsets))(*offsets); vi=(c.c_uint32*count)(*indices)
        smooth=(c.c_uint8*len(polys))(*(int(q['smooth']) for q in polys))
        work=(c.c_double*(3*nv))(); out=(c.c_float*(3*count))()
        if not f(coords,nv,off,vi,smooth,len(polys),count,work,out):
            rejected+=1; continue
        for i, loop in enumerate(loops):
            ref=mesh['corner_normals'][loop]; actual=out[3*i:3*i+3]
            error=max(abs(x-y) for x,y in zip(actual,ref)); worst=max(worst,error)
            dot=sum(x*y for x,y in zip(actual,ref))
            lengths=math.sqrt(sum(x*x for x in actual)*sum(x*x for x in ref))
            if not lengths or not math.isfinite(lengths): raise ValueError('invalid normal')
            angle=max(angle,math.degrees(math.acos(max(-1,min(1,dot/lengths)))))
            violations+=error>tolerance; total+=1
    return dict(corners=total,rejected_meshes=rejected,max_component_error=worst,
                max_angle_degrees=angle,over_tolerance=violations,
                tolerance=tolerance,runtime_ready=False)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source'); parser.add_argument('library')
    parser.add_argument('--tolerance',type=float,default=1e-4)
    args=parser.parse_args()
    if not math.isfinite(args.tolerance) or args.tolerance<=0: parser.error('positive finite tolerance required')
    result=compare(json.loads(pathlib.Path(args.source).read_text(encoding='utf8')),args.library,args.tolerance)
    print(json.dumps(result,indent=2))
    if result['rejected_meshes'] or result['over_tolerance']: raise SystemExit(1)
