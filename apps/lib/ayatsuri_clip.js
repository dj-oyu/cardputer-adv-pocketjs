// Optional adapter for current Ayatsuri packed ellipsoids. No depth/Z occlusion.
// Internal ABI: handle._node.analytic.work is F32[66] over 264B; screen is [24..31].
// Native mode experimentally borrows AyatsuriNative's instance/n/ready fields.
'use strict';
globalThis.AyatsuriClip=(()=>{
  const EMPTY=new Float32Array(0),sceneStates=new WeakMap(),apertureStates=new WeakMap(),handleStates=new WeakMap();
  function fail(text){throw new TypeError('AyatsuriClip: '+text);}
  function alive(s){if(!s||s.dead||s.busy)throw new Error('AyatsuriClip: disposed/busy scene');}
  function array(v,n){
    if(!(v instanceof Float32Array)||v.length!==n||!(v.buffer instanceof ArrayBuffer))fail('owned-compatible Float32Array required');
    for(let i=0;i<n;i++)if(!Number.isFinite(v[i]))fail('finite vertices required');
  }
  function convex(v,n,stride,expected){
    let sign=0,area=0;
    for(let i=0;i<n;i++){
      const a=i*stride,b=((i+1)%n)*stride,c=((i+2)%n)*stride,
        x=v[a],y=v[a+1],dx=v[b]-x,dy=v[b+1]-y,cross=dx*(v[c+1]-v[b+1])-dy*(v[c]-v[b]);
      if(!Number.isFinite(cross)||Math.abs(cross)<1e-6)fail('strict convex aperture required');
      const next=cross>0?1:-1;if(sign&&next!==sign)fail('folded aperture');sign=next;area+=x*v[b+1]-y*v[b];
      for(let k=0;k<i;k++)if(x===v[k*stride]&&y===v[k*stride+1])fail('duplicate aperture vertex');
    }
    if(!Number.isFinite(area)||Math.abs(area)<1e-6||(expected&&sign!==expected))fail('aperture winding changed');
    for(let i=0;i<n;i++)for(let k=0;k<n;k++)if(k!==i&&k!==(i+1)%n){
      const a=i*stride,b=((i+1)%n)*stride,p=k*stride,
        side=(v[b]-v[a])*(v[p+1]-v[a+1])-(v[b+1]-v[a+1])*(v[p]-v[a]);
      if(!Number.isFinite(side)||side*sign<=1e-6)fail('self-intersecting aperture');
    }
    return sign;
  }
  function packed(s,h){
    const state=h&&handleStates.get(h),n=state&&state.owner===s&&state.raw&&state.raw._node,p=n&&n.analytic,w=p&&p.work;
    if(!n||n.owner!==s.nodeOwner||!p||p.kind!=='ellipsoid'||!(w instanceof Float32Array)||w.length!==66||
      !(w.buffer instanceof ArrayBuffer)||w.buffer.byteLength!==264||w.byteOffset!==0||!n.matrix||n.matrix.buffer!==w.buffer)
      fail('unsupported private packed-handle ABI');
    return w;
  }
  function nativeId(s,h){
    const state=h&&handleStates.get(h),id=state&&state.owner===s&&state.raw&&state.raw.id;
    if(!Number.isInteger(id)||id<0||id>=s.raw.n)fail('native handle from this scene required');return id;
  }
  function wrap(s,raw){
    if(s.nativeMode&&s.slots[raw.id])return s.slots[raw.id];
    const handle=Object.create(Handle),state={owner:s,raw};handleStates.set(handle,state);s.handles.push(state);
    if(s.nativeMode)s.slots[raw.id]=handle;return handle;
  }
  function forward(handle,operation,value){
    const h=handleStates.get(handle),s=h&&h.owner;alive(s);s.busy=true;s.ready=false;
    try{if(typeof h.raw[operation]!=='function')fail('operation unsupported by this scene');h.raw[operation](value);}finally{s.busy=false;}
  }
  const Handle={place(value){forward(this,'place',value);},material(value){forward(this,'material',value);},
    visible(value){forward(this,'visible',value);},texture(value){forward(this,'texture',value);}};
  const Aperture={bind(target){
    const a=apertureStates.get(this),s=a.owner;alive(s);s.busy=true;
    try{
      if(s.nativeMode){
        if(s.sealed)fail('native clip bindings are fixed after first evaluate');
        const id=nativeId(s,target),n=s.bindings.length;
        for(let i=0;i<n;i++)if(s.bindings[i].id===id)fail('target already bound');
        // Setup-only growth. Final arrays are reused unchanged in size per frame.
        const ids=new Uint16Array(n+1),counts=new Uint8Array(n+1),xy=new Float32Array((n+1)*16);
        for(let i=0;i<n;i++)ids[i]=s.bindings[i].id;ids[n]=id;
        s.bindings.push({id,aperture:a});s.ids=ids;s.counts=counts;s.xy=xy;
      }else{
        const work=packed(s,target);for(let i=0;i<s.bindings.length;i++)if(s.bindings[i].buffer===work.buffer)fail('target already bound');
        s.bindings.push({buffer:work.buffer,aperture:a});
      }
      s.ready=false;
    }finally{s.busy=false;}
    return this;
  },setVertices(vertices){
    const a=apertureStates.get(this),s=a.owner;alive(s);s.busy=true;s.ready=false;
    try{array(vertices,a.n*3);let changed=false;
      for(let i=0;i<vertices.length;i++){a.localStage[i]=vertices[i];if(a.localStage[i]!==a.local[i])changed=true;}
      convex(a.localStage,a.n,3,a.restSign);
      if(changed){const swap=a.local;a.local=a.localStage;a.localStage=swap;++a.revision;}
      a.invalid=false;
    }catch(error){a.invalid=true;throw error;}finally{s.busy=false;}
  },setEmpty(reason){
    const a=apertureStates.get(this),s=a.owner;alive(s);s.busy=true;
    try{if(reason!==null&&reason!=='closed'&&reason!=='pose')fail('explicit empty reason required');
      if(a.empty!==reason){a.empty=reason;++a.revision;s.ready=false;}
    }finally{s.busy=false;}
  }};
  const Backend={ellipsoidPacked(buffer,offset,lighting){
    const s=this.owner;if(!s||s.dead||!s.rendering)fail('inactive adapter');
    for(let i=0;i<s.bindings.length;i++){const b=s.bindings[i];if(b.buffer===buffer){s.native.ellipsoidClippedPacked(buffer,offset,lighting,b.aperture.active);return;}}
    s.native.ellipsoidPacked(buffer,offset,lighting);
  },blendEllipsoids(lobes,lighting,material,surface,controls){
    const s=this.owner;if(!s||s.dead||!s.rendering)fail('inactive adapter');s.native.blendEllipsoids(lobes,lighting,material,surface,controls);
  }};
  const Scene={add(def){
    const s=sceneStates.get(this);alive(s);s.busy=true;s.ready=false;
    try{
      if(s.nativeMode)fail('native scene has immutable geometry');
      if(!def||typeof def!=='object')fail('object definition required');
      const copy={};for(const key in def)copy[key]=def[key];
      if(copy.parent!==undefined&&copy.parent!==null){
        const parent=handleStates.get(copy.parent);if(!parent||parent.owner!==s)fail('parent from this scene required');copy.parent=parent.raw;
      }
      const raw=s.raw.add(copy);if(!s.nodeOwner)s.nodeOwner=raw._node.owner;
      return wrap(s,raw);
    }finally{s.busy=false;}
  },handle(id){
    const s=sceneStates.get(this);alive(s);s.busy=true;
    try{if(!s.nativeMode)fail('handle lookup requires native scene');return wrap(s,s.raw.handle(id));}finally{s.busy=false;}
  },node(name){
    const s=sceneStates.get(this);alive(s);s.busy=true;
    try{if(!s.nativeMode)fail('node lookup requires native scene');return wrap(s,s.raw.node(name));}finally{s.busy=false;}
  },aperture(projection,rest){
    const s=sceneStates.get(this);alive(s);s.busy=true;s.ready=false;
    try{if(!rest||rest.length%3)fail('XYZ aperture required');const n=rest.length/3;
      if(s.nativeMode&&s.sealed)fail('native apertures are fixed after first evaluate');
      if(n<3||n>8)fail('aperture needs 3..8 vertices');array(rest,n*3);
      const id=s.nativeMode?nativeId(s,projection):-1,work=s.nativeMode?null:packed(s,projection),copy=new Float32Array(rest),sign=convex(copy,n,3,0),a={};
      Object.assign(a,{owner:s,projection,projectionId:id,node:s.nativeMode?null:handleStates.get(projection).raw._node,work,n,rest:copy,local:new Float32Array(copy),localStage:new Float32Array(n*3),
        xy:new Float32Array(n*2),xyStage:new Float32Array(n*2),screen:new Float32Array(8),screenStage:new Float32Array(8),active:EMPTY,
        restSign:sign,screenSign:0,pendingSign:0,revision:1,evaluatedRevision:0,empty:null,invalid:false,changed:false});
      const handle=Object.create(Aperture);apertureStates.set(handle,a);s.apertures.push(a);return handle;
    }finally{s.busy=false;}
  },light(patch){const s=sceneStates.get(this);alive(s);s.busy=true;s.ready=false;try{s.raw.light(patch);}finally{s.busy=false;}},
  camera(patch){const s=sceneStates.get(this);alive(s);s.busy=true;s.ready=false;try{s.raw.camera(patch);}finally{s.busy=false;}},
  evaluate(timeMs){
    const s=sceneStates.get(this);alive(s);s.busy=true;s.ready=false;
    try{
      s.raw.evaluate(timeMs);
      if(s.nativeMode){
        s.sealed=true;
        // Kind/visibility are intentionally deferred; setup never auto-evaluates.
        for(let i=0;i<s.bindings.length;i++)s.raw.instance.readProjection(s.bindings[i].id,s.readScratch);
      }
      for(let j=0;j<s.apertures.length;j++){
        const a=s.apertures[j],w=a.screenStage;
        if(a.invalid)fail('invalid morph must be replaced');
        if(s.nativeMode)s.raw.instance.readProjection(a.projectionId,w);
        else{
          if(handleStates.get(a.projection).raw._node!==a.node||a.node.analytic.work!==a.work||a.work.buffer.byteLength!==264)fail('private packed-handle ABI changed');
          for(let k=0;k<8;k++)w[k]=a.work[24+k];
        }
        let changed=a.revision!==a.evaluatedRevision;
        for(let k=0;k<8;k++)if(w[k]!==a.screen[k])changed=true;
        a.changed=changed;
        if(!changed||a.empty)continue;
        for(let k=0;k<a.n;k++){
          const at=k*3,x=a.local[at],y=a.local[at+1],z=a.local[at+2];
          a.xyStage[k*2]=w[0]*x+w[1]*y+w[2]*z+w[3];
          a.xyStage[k*2+1]=w[4]*x+w[5]*y+w[6]*z+w[7];
        }
        for(let k=0;k<a.xyStage.length;k++)if(!Number.isFinite(a.xyStage[k])||Math.abs(a.xyStage[k])>4096)fail('projected aperture range');
        a.pendingSign=convex(a.xyStage,a.n,2,a.screenSign);++s.projects;
      }
      // Publish only after every aperture has passed in this same evaluated frame.
      for(let j=0;j<s.apertures.length;j++){
        const a=s.apertures[j];if(!a.changed)continue;
        if(a.empty)a.active=EMPTY;
        else{const swap=a.xy;a.xy=a.xyStage;a.xyStage=swap;a.active=a.xy;if(!a.screenSign)a.screenSign=a.pendingSign;}
        for(let k=0;k<8;k++)a.screen[k]=a.screenStage[k];a.evaluatedRevision=a.revision;
      }
      if(s.nativeMode)for(let i=0;i<s.bindings.length;i++){
        const a=s.bindings[i].aperture;if(!a.changed)continue;
        s.counts[i]=a.active.length/2;
        for(let k=0;k<16;k++)s.xy[i*16+k]=k<a.active.length?a.active[k]:0;
      }
      s.ready=true;
    }finally{s.busy=false;}
  },render(){
    const s=sceneStates.get(this);alive(s);if(!s.ready)fail('evaluate before render');s.busy=true;s.rendering=true;
    try{if(s.nativeMode){if(!s.raw.ready)fail('native evaluate before render');s.raw.instance.submitClipped(s.ids,s.counts,s.xy);}else s.raw.render();}
    catch(error){s.ready=false;throw error;}finally{s.rendering=false;s.busy=false;}
  },visit(visitor){const s=sceneStates.get(this);alive(s);if(s.nativeMode)fail('native packet visit unavailable');if(!s.ready)fail('evaluate before visit');s.busy=true;try{s.raw.visit(visitor);}finally{s.busy=false;}},
  stats(out){
    const s=sceneStates.get(this);alive(s);s.busy=true;try{s.raw.stats(out);if(s.nativeMode)out.objects=s.raw.n;out.clipTargets=s.bindings.length;out.apertures=s.apertures.length;out.clipProjects=s.projects;}finally{s.busy=false;}
  },dispose(){
    const s=sceneStates.get(this);if(s.busy)throw new Error('AyatsuriClip: disposed/busy scene');if(s.dead)return;s.busy=true;
    try{s.raw.dispose();s.dead=true;s.ready=false;
      for(let j=0;j<s.apertures.length;j++){
        const a=s.apertures[j];a.projection=a.node=a.work=a.rest=a.local=a.localStage=a.xy=a.xyStage=a.screen=a.screenStage=a.active=null;
      }
      for(let j=0;j<s.handles.length;j++)s.handles[j].raw=null;
      s.apertures.length=s.bindings.length=s.handles.length=0;if(s.adapter)s.adapter.owner=null;
      s.raw=s.native=s.nodeOwner=s.apertures=s.bindings=s.adapter=s.handles=s.slots=s.ids=s.counts=s.xy=s.readScratch=null;
    }finally{s.busy=false;}
  }};
  function scene(options){
    const native=options.backend;
    if(!native||typeof native.ellipsoidPacked!=='function'||typeof native.ellipsoidClippedPacked!=='function')fail('clipped packed backend required');
    const facade=Object.create(Scene),s={};sceneStates.set(facade,s);Object.assign(s,{raw:null,native,nodeOwner:null,apertures:[],bindings:[],handles:[],projects:0,ready:false,dead:false,busy:false,rendering:false});
    const backend=Object.create(Backend);backend.owner=s;s.adapter=backend;
    // Forward known constructor options without changing the existing utility.
    s.raw=Ayatsuri.scene({backend,geometries:options.geometries,evaluator:options.evaluator,light:options.light,camera:options.camera});return facade;
  }
  function nativeScene(options){
    const facade=Object.create(Scene),s={};sceneStates.set(facade,s);
    Object.assign(s,{nativeMode:true,sealed:false,raw:null,native:null,nodeOwner:null,apertures:[],bindings:[],handles:[],slots:[],projects:0,
      ids:new Uint16Array(0),counts:new Uint8Array(0),xy:new Float32Array(0),readScratch:new Float32Array(8),ready:false,dead:false,busy:false,rendering:false});
    try{
      s.raw=AyatsuriNative.scene(options);
      if(!s.raw.instance||!Number.isInteger(s.raw.n)||typeof s.raw.instance.readProjection!=='function'||typeof s.raw.instance.submitClipped!=='function')fail('unsupported native scene ABI');
      return facade;
    }catch(error){if(s.raw)s.raw.dispose();throw error;}
  }
  return {scene,nativeScene};
})();
if(typeof module!=='undefined')module.exports=globalThis.AyatsuriClip;
