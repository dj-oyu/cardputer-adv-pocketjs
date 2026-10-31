// Optional local albedo plane; loaded only by apps that use this pattern.
// Adapter ABI: SourceA handle._node.analytic is the existing 264B packet.
'use strict';
globalThis.AyatsuriLocalPlane=(()=>{
  function fail(text){throw new TypeError('Ayatsuri: '+text);}
  function finite(x){if(typeof x!=='number'||x-x!==0)fail('finite number required');return x;}
  const Pattern={kind:'localPlane',code:4,encode(work){
    const n=this.normal;work[49]=n[0];work[50]=n[1];work[51]=n[2];
    work[52]=this.offset;work[53]=this.feather;work[54]=0;work[56]=0;work[57]=1;
  }};
  Object.freeze(Pattern);
  function create(p){
    if(!p||p.kind!=='localPlane')fail('pattern kind');
    const n=p.normal;if(!n||n.length!==3)fail('vector length');
    const x=finite(n[0]),y=finite(n[1]),z=finite(n[2]);
    finite(Math.fround(x));finite(Math.fround(y));finite(Math.fround(z));
    const length=Math.hypot(x,y,z),o=p.offset,f=p.feather,
      offset=o===undefined?0:finite(o),feather=f===undefined?0:finite(f),color=p.color;
    if(!(length>0)||Math.abs(offset)>2||feather<0||feather>2||(feather>0&&feather<1e-4))fail('localPlane range');
    if(color!==(color>>>0)||color>0xffffff)fail('RGB24 required');
    const pattern=Object.create(Pattern);pattern.normal=[x/length,y/length,z/length];
    pattern.offset=offset;pattern.feather=feather;pattern.color=color;return pattern;
  }
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
  const Scene={add(def){
    const s=this._plane;alive(s);s.busy=true;
    let copy=def,record=null,queued=false;
    try{
      // Reserve every adapter-owned object/slot before base.add publishes.
      const handle=Object.create(Handle);
      handle._node=null;handle._raw=null;handle._plane=s;handle._region=false;
      // The wrapper blocks its entry points, and the base owner blocks raw
      // handle mutation while user pattern/normal/definition getters run.
      const owner=s.owner;if(owner)owner.busy=true;
      try{
        const p=def&&def.pattern;
        if(p&&p.kind==='localPlane'){
          const pattern=create(p);copy={};
          for(const key in def)if(key!=='pattern')copy[key]=def[key];
          if(!copy.geometry||copy.geometry.kind!=='ellipsoid')fail('localPlane requires ellipsoid');
          if(copy.textureTransform)fail('localPlane has no UV transform');
          record={work:null,surface:null,pattern,color565:s.pack(pattern.color)};
        }
      }finally{if(owner)owner.busy=false;}
      if(record){s.records.push(record);queued=true;}
      let h;
      try{h=s.base.add(copy);}catch(error){if(queued)s.records.pop();throw error;}
      if(!s.owner)s.owner=h._node.owner;
      if(record){
        record.work=h._node.analytic.work;record.surface=h._node.analytic.surface;
      }
      handle._node=h._node;handle._raw=h;handle._plane=s;handle._region=!!record;
      s.ready=false;return handle;
    }finally{s.busy=false;}
  },light(patch){change(this._plane,'light',patch);},camera(patch){change(this._plane,'camera',patch);},
  evaluate(timeMs){
    const s=this._plane;alive(s);s.busy=true;s.ready=false;
    try{
      s.base.evaluate(timeMs);
      for(let k=0;k<s.records.length;k++){
        const r=s.records[k];r.pattern.encode(r.work);r.surface[1]=r.color565;r.surface[4]=4;
      }
      s.ready=true;
    }finally{s.busy=false;}
  },render(){
    const s=this._plane;ready(s);s.busy=true;
    try{s.base.render();}catch(error){s.ready=false;throw error;}finally{s.busy=false;}
  },visit(visitor){
    const s=this._plane;ready(s);s.busy=true;
    try{s.base.visit(visitor);}finally{s.busy=false;}
  },stats(out){const s=this._plane;alive(s);s.busy=true;try{s.base.stats(out);}finally{s.busy=false;}},
  dispose(){
    const s=this._plane;if(!s||s.busy||(s.owner&&s.owner.busy))fail('busy scene');if(s.dead)return;s.busy=true;
    try{
      s.base.dispose();for(let k=0;k<s.records.length;k++){
        const r=s.records[k];r.work=r.surface=r.pattern=null;
      }
      s.records.length=0;s.base=s.owner=s.pack=s.records=null;s.dead=true;s.ready=false;
    }finally{s.busy=false;}
  }};
  function scene(options){
    const Base=globalThis.Ayatsuri;
    if(!Base||typeof Base.scene!=='function'||typeof Base.pack!=='function')fail('base Ayatsuri required');
    const factory=Base.scene,pack=Base.pack,facade=Object.create(Scene);
    facade._plane={base:factory(options),pack,owner:null,records:[],ready:false,busy:false,dead:false};return facade;
  }
  const api={create,scene};if(typeof module!=='undefined')module.exports=api;return api;
})();
