"""Blender 4.3.2 black-box checks for explicit frame boundary candidates.
Uses only synthetic triangles and SINGLE_PROP custom-property drivers.
The offset randomized check is a measured regression envelope, not a claim of
bitwise equivalence for arbitrary topology/normals or admission of a full model.
"""
import argparse
import ctypes as ct
import pathlib
import subprocess
import sys
import tempfile

def oracle(library):
    lib=ct.CDLL(str(library))
    fp=ct.POINTER(ct.c_float);dp=ct.POINTER(ct.c_double)
    lib.input_bundle.argtypes=[dp,ct.c_size_t,dp]
    lib.offset_point.argtypes=[fp,fp,fp,ct.c_float,fp]
    import bpy,struct,random,json,math
    f=lambda x:struct.unpack('f',struct.pack('f',x))[0]
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    source=bpy.data.objects.new('inputs',None);target=bpy.data.objects.new('driven',None);bpy.context.collection.objects.link(source);bpy.context.collection.objects.link(target);source['t']=0.;source['u']=0.
    exprs=['t*0.7+0.3','t*t+u*0.25','(t-u)*1.3','t if t<0.1 else u','min(t,u)*0.31','sin(t)+cos(u)']
    for i,expr in enumerate(exprs):
     fc=target.driver_add('location' if i<3 else 'rotation_euler',i%3);fc.driver.type='SCRIPTED';fc.driver.expression=expr
     for name in ('t','u'):
      v=fc.driver.variables.new();v.name=name;v.type='SINGLE_PROP';v.targets[0].id=source;v.targets[0].data_path='["'+name+'"]'
    rng=random.Random(731);errors=[0.]*6;rawerrors=[0.]*6;bad=[0]*6;rawbad=[0]*6;changed=0
    for sample in range(2048):
     t,u=[rng.uniform(-2,2) for _ in range(2)]
     if sample<6:
      t,u=((math.nextafter(.1,-math.inf),.7),(math.nextafter(.1,math.inf),-.7),(.1-1e-9,.4),(.1+1e-9,-.4),(-0.,0.),(0.,-0.))[sample]
     source['t']=t;source['u']=u;assert source['t']==t and source['u']==u
     source.update_tag();bpy.context.view_layer.update();g=bpy.context.evaluated_depsgraph_get();g.update();obj=target.evaluated_get(g);actual=[*obj.location,*obj.rotation_euler]
     for i,expr in enumerate(exprs):
      rounded=(ct.c_double*2)();assert lib.input_bundle((ct.c_double*2)(t,u),2,rounded)
      expected=f(eval(expr,dict(sin=math.sin,cos=math.cos,min=min),dict(t=rounded[0],u=rounded[1])))
      raw=f(eval(expr,dict(sin=math.sin,cos=math.cos,min=min),dict(t=t,u=u)))
      e=abs(actual[i]-expected);er=abs(actual[i]-raw);errors[i]=max(errors[i],e);rawerrors[i]=max(rawerrors[i],er);bad[i]+=e!=0;rawbad[i]+=er!=0
    print('DRIVER_INPUT_BOUNDARY',json.dumps(dict(samples=2048,expressions=6,float_input_nonexact=bad,float_input_max=errors,double_input_nonexact=rawbad,double_input_max=rawerrors)))
    assert not any(bad)
    import bpy,math,struct,random,json
    from mathutils import Vector
    from mathutils.geometry import normal
    f=lambda x:struct.unpack('f',struct.pack('f',x))[0]
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    def obj(name,vertices,faces):
     m=bpy.data.meshes.new(name);m.from_pydata(vertices,[],faces);o=bpy.data.objects.new(name,m);bpy.context.collection.objects.link(o);return o
    source=obj('source',[(0,0,0)],[]);target=obj('target',[(0,0,0),(1,0,0),(0,1,0)],[(0,1,2)])
    mod=source.modifiers.new('wrap','SHRINKWRAP');mod.target=target;mod.wrap_method='NEAREST_SURFACEPOINT';mod.wrap_mode='ON_SURFACE'
    def evaluate(offset):
     mod.offset=offset;source.data.update();source.update_tag();target.data.update();target.update_tag();bpy.context.view_layer.update();g=bpy.context.evaluated_depsgraph_get();g.update();e=source.evaluated_get(g);m=e.to_mesh();out=tuple(m.vertices[0].co);e.to_mesh_clear();return out
    for bits in (0x33fffffe,0x33ffffff,0x34000000,0x34000001,0x34000002):
        distance=struct.unpack('f',struct.pack('I',bits))[0]
        source.data.vertices[0].co=(-distance,0.,0.)
        actual=evaluate(.01);hit=evaluate(0.);predicted=(ct.c_float*3)()
        assert lib.offset_point((ct.c_float*3)(-distance,0,0),(ct.c_float*3)(*hit),(ct.c_float*3)(0,0,1),.01,predicted)
        assert tuple(predicted)==actual, "epsilon endpoint differs from oracle"
    print('OFFSET_EPSILON_BOUNDARY exact=5')
    rng=random.Random(99271);stats={};maxerror=0.;fail=0
    for i in range(2048):
     shift=Vector([rng.uniform(-3,3) for _ in range(3)])
     u=Vector([rng.uniform(-1,1) for _ in range(3)]).normalized();v=u.cross(Vector([rng.uniform(-1,1) for _ in range(3)])).normalized();u*=rng.uniform(.3,4);v*=rng.uniform(.3,4)
     vertices=[shift,shift+u,shift+v]
     for a,b in zip(target.data.vertices,vertices):a.co=b
     vertices=[a.co.copy() for a in target.data.vertices];n=normal(*vertices)
     distance=10**rng.uniform(-8,-2);side=rng.choice((-1,1));region=i%2
     query=vertices[0]+(u+v)*.25+n*(side*distance) if region else vertices[0]-u.normalized()*distance-v.normalized()*distance+n*(side*distance)
     source.data.vertices[0].co=query;query=tuple(source.data.vertices[0].co);offset=f(rng.choice((-1,1))*10**rng.uniform(-4,-.7));actual=evaluate(offset);hit=evaluate(0.);n=target.data.polygons[0].normal.copy()
     delta=[f(query[k]-hit[k]) for k in range(3)];length=f(math.sqrt(f(f(f(delta[0]*delta[0])+f(delta[1]*delta[1]))+f(delta[2]*delta[2]))))
     predicted=(ct.c_float*3)();assert lib.offset_point((ct.c_float*3)(*query),(ct.c_float*3)(*hit),(ct.c_float*3)(*n),offset,predicted)
     error=max(abs(predicted[k]-actual[k]) for k in range(3));maxerror=max(maxerror,error);fail+=error!=0
     row=stats.setdefault(str(region),dict(count=0,nonexact=0,maximum=0.,zero_distance=0));row['count']+=1;row['nonexact']+=error!=0;row['maximum']=max(row['maximum'],error);row['zero_distance']+=length==0
    print('HELDOUT',json.dumps(dict(cases=2048,nonexact=fail,max_error=maxerror,regions=stats)))

    assert maxerror<=1.1920928955078125e-7, "synthetic oracle regression envelope exceeded"

if __name__=='__main__':
    if '--oracle' in sys.argv:
        oracle(pathlib.Path(sys.argv[sys.argv.index('--oracle')+1]))
    else:
        parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--blender',required=True);args=parser.parse_args()
        from test_f32_frame_boundaries import build_library
        with tempfile.TemporaryDirectory() as directory:
            library=build_library(directory)
            subprocess.run([args.blender,'--background','--factory-startup','--python-exit-code','1','--python',str(pathlib.Path(__file__).resolve()),'--','--oracle',str(library)],check=True)
