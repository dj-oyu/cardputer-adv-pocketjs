// Optional local albedo plane; loaded only by apps that use this pattern.
// Adapter ABI: SourceA ellipsoid is 264B; blend is 316+108*lobes (+24 for plane).
'use strict';
globalThis.AyatsuriLocalPlane=(()=>{
  function fail(text){throw new TypeError('Ayatsuri: '+text);}
  function finite(x){if(typeof x!=='number'||x-x!==0)fail('finite number required');return x;}
  const Pattern={kind:'localPlane',code:4,encode(work){
    const n=this.normal;work[49]=n[0];work[50]=n[1];work[51]=n[2];
    work[52]=this.offset;work[53]=this.feather;work[54]=0;work[56]=0;work[57]=1;
  }};
  Object.freeze(Pattern);
  const BlendPattern=Object.freeze({kind:'localPlane',code:5});
  function make(p,limit){
    if(!p||p.kind!=='localPlane')fail('pattern kind');
    const n=p.normal;if(!n||n.length!==3)fail('vector length');
    const x=finite(n[0]),y=finite(n[1]),z=finite(n[2]);
    finite(Math.fround(x));finite(Math.fround(y));finite(Math.fround(z));
    const length=Math.hypot(x,y,z),o=p.offset,f=p.feather,
      offset=o===undefined?0:finite(o),feather=f===undefined?0:finite(f),color=p.color;
    if(!(length>0)||Math.abs(offset)>limit||feather<0||feather>limit||(feather>0&&feather<1e-4))fail('localPlane range');
    if(color!==(color>>>0)||color>0xffffff)fail('RGB24 required');
    const pattern=Object.create(limit===2?Pattern:BlendPattern);pattern.normal=[x/length,y/length,z/length];
    pattern.offset=offset;pattern.feather=feather;pattern.color=color;return pattern;
  }
  function create(p){return make(p,2);}
  function alive(s){
    if(!s||s.dead||(s.owner&&s.owner.disposed))fail('disposed scene');
    if(s.busy||(s.owner&&s.owner.busy))fail('busy scene');
  }
  function ready(s){alive(s);if(!s.ready||(s.owner&&s.owner.dirty))fail('evaluate before render');}
  function mutate(h,method,value){
    const s=h._plane;alive(s);s.busy=true;
    try{
      if(method==='texture'&&h._region)fail('localPlane has no UV transform');
      h._raw[method](value);
    }finally{s.busy=false;}
  }
  const Handle={place(value){mutate(this,'place',value);},material(value){mutate(this,'material',value);},
    visible(value){mutate(this,'visible',value);},texture(value){mutate(this,'texture',value);}};
  function change(s,method,patch){
    alive(s);s.busy=true;
    try{s.base[method](patch);s.ready=false;}finally{s.busy=false;}
  }
  function release(r){
    if(!r)return;
    if(r.patch){
      if(r.patch.localPlane!==undefined)r.patch.localPlane=null;
      if(r.patch.localPlaneColor!==undefined)r.patch.localPlaneColor=null;
      if(r.patch.localPlaneNormal!==undefined)r.patch.localPlaneNormal=null;
    }
    r.work=r.surface=r.pattern=r.plane=r.color=r.patch=null;
  }
  function blendPlane(node,context){
    const p=node.analytic,n=p.localPlaneNormal,nx=Math.fround(n[0]),ny=Math.fround(n[1]),nz=Math.fround(n[2]),
      m=node.matrix,camera=context.cameraState,right=context.right,down=context.down,forward=context.forward,z=camera[6],
      a=z*(right[0]*m[0]+right[1]*m[4]+right[2]*m[8]),b=z*(right[0]*m[1]+right[1]*m[5]+right[2]*m[9]),c=z*(right[0]*m[2]+right[1]*m[6]+right[2]*m[10]),
      d=z*(down[0]*m[0]+down[1]*m[4]+down[2]*m[8]),e=z*(down[0]*m[1]+down[1]*m[5]+down[2]*m[9]),f=z*(down[0]*m[2]+down[1]*m[6]+down[2]*m[10]),
      g=forward[0]*m[0]+forward[1]*m[4]+forward[2]*m[8],h=forward[0]*m[1]+forward[1]*m[5]+forward[2]*m[9],j=forward[0]*m[2]+forward[1]*m[6]+forward[2]*m[10],
      det=a*(e*j-f*h)-b*(d*j-f*g)+c*(d*h-e*g);
    if(!Number.isFinite(det)||det===0)fail('blend localPlane singular transform');
    // Divide inverse entries separately, matching the common C model arithmetic.
    const x=nx*((e*j-f*h)/det)+ny*((f*g-d*j)/det)+nz*((d*h-e*g)/det),
      y=nx*((c*h-b*j)/det)+ny*((a*j-c*g)/det)+nz*((b*g-a*h)/det),
      q=nx*((b*f-c*e)/det)+ny*((c*d-a*f)/det)+nz*((a*e-b*d)/det),
      tx=z*(right[0]*(m[3]-camera[3])+right[1]*(m[7]-camera[4])+right[2]*(m[11]-camera[5]))+camera[7],
      ty=z*(down[0]*(m[3]-camera[3])+down[1]*(m[7]-camera[4])+down[2]*(m[11]-camera[5]))+camera[8],
      tz=forward[0]*(m[3]-camera[3])+forward[1]*(m[7]-camera[4])+forward[2]*(m[11]-camera[5]),
      p0=Math.fround(x),p1=Math.fround(y),p2=Math.fround(q),p3=Math.fround(-x*tx-y*ty-q*tz-p.localPlaneOffset);
    if(!Number.isFinite(p0)||!Number.isFinite(p1)||!Number.isFinite(p2)||!Number.isFinite(p3)||
      Math.abs(p0)>1e6||Math.abs(p1)>1e6||Math.abs(p2)>1e6||Math.abs(p3)>1e6||(!p0&&!p1&&!p2))fail('blend localPlane packet range');
    p.localPlane[0]=p0;p.localPlane[1]=p1;p.localPlane[2]=p2;p.localPlane[3]=p3;
  }
  const Blend={bytes(def,backend){
    const bytes=this.raw.bytes.call(this.raw,def,backend),r=this.state.pending;
    if(r&&r.mode===5){r.at=bytes;return bytes+24;}return bytes;
  },create(def,storage){
    const patch=this.raw.create.call(this.raw,def,storage),r=this.state.pending;
    if(r&&r.mode===5){
      if(r.at!==316+108*patch.count||storage.byteLength!==r.at+24)fail('blend localPlane private layout');
      r.patch=patch;
      patch.localPlane=null;patch.localPlaneColor=null;patch.localPlaneNormal=null;patch.patternMode=5;
      r.plane=new Float32Array(storage,r.at,5);r.color=new Uint16Array(storage,r.at+20,1);
      patch.localPlaneNormal=r.pattern.normal;patch.localPlaneOffset=Math.fround(r.pattern.offset);
      patch.localPlaneFeather=Math.fround(r.pattern.feather);
      r.plane[4]=patch.localPlaneFeather;r.color[0]=r.color565;
      patch.localPlane=r.plane;patch.localPlaneColor=r.color;
    }
    return patch;
  },evaluate(node,geometryChanged,cameraChanged,lightChanged,context){
    this.raw.evaluate.call(this.raw,node,geometryChanged,cameraChanged,lightChanged,context);
    if(node.analytic.localPlane&&(geometryChanged||cameraChanged)){
      blendPlane(node,context);++this.state.planeUpdates;
    }
  },render(node,lighting,backend){
    const p=node.analytic;
    if(p.localPlane)backend.blendEllipsoids(p.lobes,lighting,node.materialData,p.surface,p.controls,p.localPlane,p.localPlaneColor);
    else this.raw.render.call(this.raw,node,lighting,backend);
  }};
  Object.freeze(Blend);
  const Scene={add(def){
    const s=this._plane;alive(s);s.busy=true;
    let copy=def,record=null,handle=null,queued=false,handleQueued=false;
    try{
      // Reserve every adapter-owned object/slot before base.add publishes.
      handle=Object.create(Handle);
      handle._node=null;handle._raw=null;handle._plane=s;handle._region=false;
      // The wrapper blocks its entry points, and the base owner blocks raw
      // handle mutation while user pattern/normal/definition getters run.
      const owner=s.owner;if(owner)owner.busy=true;
      try{
        const p=def&&def.pattern;
        if(p&&p.kind==='localPlane'){
          // Literal field definitions propagate allocation failure; a computed
          // property copy could lose a field with a pending QuickJS exception.
          copy={name:def.name,parent:def.parent,geometry:def.geometry,position:def.position,
            rotation:def.rotation,scale:def.scale,material:def.material,visible:def.visible,textureTransform:def.textureTransform};
          const kind=copy.geometry&&copy.geometry.kind,mode=kind==='ellipsoid'?4:kind==='blendEllipsoids'&&s.proxy?5:0;
          if(!mode)fail('localPlane requires ellipsoid or blend extension');
          const pattern=make(p,mode===5?4096:2);
          if(copy.textureTransform)fail('localPlane has no UV transform');
          record={mode,work:null,surface:null,pattern,color565:s.pack(pattern.color),plane:null,color:null,patch:null,at:0};
        }
      }finally{if(owner)owner.busy=false;}
      s.handles.push(handle);handleQueued=true;
      if(record){s.records.push(record);queued=true;}
      s.pending=record;
      const h=s.base.add(copy);
      if(!s.owner)s.owner=h._node.owner;
      if(record&&record.mode===4){
        record.work=h._node.analytic.work;record.surface=h._node.analytic.surface;
      }
      handle._node=h._node;handle._raw=h;handle._plane=s;handle._region=!!record;
      s.ready=false;return handle;
    }catch(error){
      if(queued)s.records.pop();if(handleQueued)s.handles.pop();release(record);
      if(handle)handle._node=handle._raw=handle._plane=null;throw error;
    }finally{s.pending=null;s.busy=false;}
  },light(patch){change(this._plane,'light',patch);},camera(patch){change(this._plane,'camera',patch);},
  evaluate(timeMs){
    const s=this._plane;alive(s);s.busy=true;s.ready=false;
    try{
      s.base.evaluate(timeMs);
      for(let k=0;k<s.records.length;k++){
        const r=s.records[k];if(r.mode===4){r.pattern.encode(r.work);r.surface[1]=r.color565;r.surface[4]=4;}
      }
      s.ready=true;
    }finally{s.busy=false;}
  },render(){
    const s=this._plane;ready(s);s.busy=true;
    try{s.base.render();}catch(error){s.ready=false;throw error;}finally{s.busy=false;}
  },visit(visitor){
    const s=this._plane;ready(s);s.busy=true;
    try{s.base.visit(visitor);}finally{s.busy=false;}
  },stats(out){const s=this._plane;alive(s);s.busy=true;try{s.base.stats(out);out.localPlaneUpdates=s.planeUpdates;}finally{s.busy=false;}},
  dispose(){
    const s=this._plane;if(!s||s.busy||(s.owner&&s.owner.busy))fail('busy scene');if(s.dead)return;s.busy=true;
    try{
      s.base.dispose();for(let k=0;k<s.records.length;k++)release(s.records[k]);
      for(let k=0;k<s.handles.length;k++)s.handles[k]._raw=s.handles[k]._node=s.handles[k]._plane=null;
      if(s.proxy)s.proxy.state=null;
      s.records.length=s.handles.length=0;s.base=s.owner=s.pack=s.records=s.handles=s.proxy=s.pending=null;s.dead=true;s.ready=false;
    }finally{s.busy=false;}
  }};
  function scene(options){
    const Base=globalThis.Ayatsuri;
    if(!Base||typeof Base.scene!=='function'||typeof Base.pack!=='function')fail('base Ayatsuri required');
    options=options||{};
    const factory=Base.scene,pack=Base.pack,facade=Object.create(Scene),
      copy={backend:options.backend,evaluator:options.evaluator,geometries:options.geometries,light:options.light,camera:options.camera};
    const s={base:null,pack,owner:null,records:[],handles:[],proxy:null,pending:null,planeUpdates:0,ready:false,busy:false,dead:false};
    const source=copy.backend||(typeof pocket!=='undefined'?pocket.hone:null);
    if(source){
      const ellipsoid=source.ellipsoid,packed=source.ellipsoidPacked,blend=source.blendEllipsoids;
      copy.backend={ellipsoid:typeof ellipsoid==='function'?ellipsoid.bind(source):undefined,
        ellipsoidPacked:typeof packed==='function'?packed.bind(source):undefined,
        blendEllipsoids:typeof blend==='function'?blend.bind(source):undefined};
    }
    if(Array.isArray(copy.geometries)){
      copy.geometries=copy.geometries.slice();
      for(let k=0;k<copy.geometries.length;k++){
        const ext=copy.geometries[k];if(!ext||ext.kind!=='blendEllipsoids'||s.proxy)continue;
        const raw={kind:ext.kind,materialOffset:ext.materialOffset,materialLength:ext.materialLength,
          bytes:ext.bytes,create:ext.create,initializeMaterial:ext.initializeMaterial,validateMaterial:ext.validateMaterial,
          material:ext.material,evaluate:ext.evaluate,render:ext.render};Object.freeze(raw);
        if(raw.materialOffset!==264||raw.materialLength!==11)fail('blend localPlane extension ABI');
        const proxy=Object.create(Blend);proxy.raw=raw;proxy.state=s;proxy.kind=raw.kind;
        proxy.materialOffset=raw.materialOffset;proxy.materialLength=raw.materialLength;
        proxy.initializeMaterial=raw.initializeMaterial;proxy.validateMaterial=raw.validateMaterial;proxy.material=raw.material;
        copy.geometries[k]=s.proxy=proxy;
      }
    }
    s.base=factory(copy);facade._plane=s;return facade;
  }
  const api={create,scene};if(typeof module!=='undefined')module.exports=api;return api;
})();
