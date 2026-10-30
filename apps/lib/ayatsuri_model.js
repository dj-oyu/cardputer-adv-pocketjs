// Experimental visual handles over an immutable native model and owned instance.
'use strict';
globalThis.AyatsuriNative=(()=>{
  function fail(){throw new TypeError('AyatsuriNative: invalid value');}
  function finite(v){if(typeof v!=='number'||!Number.isFinite(v))fail();return v;}
  function rgb(v){if(v!==(v>>>0)||v>0xffffff)fail();return v;}
  function alive(s){if(s.dead||s.busy)throw new Error('AyatsuriNative: disposed/busy scene');}
  function read(out,offset,value,n){if(!value||value.length!==n)fail();for(let k=0;k<n;k++){const v=Math.fround(finite(value[k]));finite(v);out[offset+k]=v;}}
  const Handle={place(patch){
    const s=this.scene;alive(s);s.busy=true;
    try{
      const p=patch.position,r=patch.rotation,z=patch.scale,tmp=s.scratch,o=this.id*9;let flags=0;
      if(p){read(tmp,0,p,3);flags|=1;}if(r){read(tmp,3,r,3);flags|=2;}
      if(z){read(tmp,6,z,3);for(let k=6;k<9;k++)if(tmp[k]===0)fail();flags|=4;}
      for(let k=0;k<9;k++)if(flags&(1<<Math.floor(k/3)))s.poses[o+k]=tmp[k];
      if(flags){s.mask[this.id]|=flags;s.ready=false;}
    }finally{s.busy=false;}
  },material(patch){
    const s=this.scene;alive(s);s.busy=true;
    try{
      for(const key in patch)if(key!=='color'&&key!=='opacity')fail();
      const color=patch.color,opacity=patch.opacity;
      if(color!==undefined)rgb(color);if(opacity!==undefined){finite(opacity);if(opacity<0||opacity>1)fail();}
      if(color!==undefined){s.colors[this.id]=color;s.mask[this.id]|=32;}
      if(opacity!==undefined){s.opacity[this.id]=opacity;s.mask[this.id]|=64;}
      if(color!==undefined||opacity!==undefined)s.ready=false;
    }finally{s.busy=false;}
  },visible(value){
    const s=this.scene;alive(s);if(typeof value!=='boolean')fail();
    s.mask[this.id]=(s.mask[this.id]&~24)|(value?8:16);s.ready=false;
  }};
  function scene(options){
    const model=options.model,n=model.count,instance=model.instance(),
      s={instance,model,n,mask:new Uint8Array(n),poses:new Float32Array(n*9),colors:new Uint32Array(n),opacity:new Float64Array(n),
        cameraState:new Float32Array([0,0,400,0,0,0,1,120,75]),lightState:new Float32Array([-.5,-.4,.8,1,1,1]),scratch:new Float64Array(9),
        handles:[],dead:false,busy:false,ready:false};
    s.handle=function(id){alive(s);if(!Number.isInteger(id)||id<0||id>=n)fail();
      if(!s.handles[id])s.handles[id]=Object.assign(Object.create(Handle),{id,scene:s});return s.handles[id];};
    s.node=function(name){return s.handle(model.node(name));};
    s.light=function(patch){alive(s);s.busy=true;try{
      const d=patch.direction,c=patch.color,tmp=s.scratch;
      if(c!==undefined)rgb(c);
      if(d){if(d.length!==3)fail();const x=finite(d[0]),y=finite(d[1]),z=finite(d[2]),l=Math.hypot(x,y,z);
        finite(Math.fround(x));finite(Math.fround(y));finite(Math.fround(z));if(!(l>0))fail();
        tmp[0]=x/l;tmp[1]=y/l;tmp[2]=z/l;}
      if(d)for(let k=0;k<3;k++)s.lightState[k]=tmp[k];
      if(c!==undefined)for(let k=0;k<3;k++)s.lightState[k+3]=((c>>((2-k)*8))&255)/255;
      s.ready=false;
    }finally{s.busy=false;}};
    s.camera=function(patch){alive(s);s.busy=true;try{
      const p=patch.position,t=patch.target,c=patch.center,z=patch.zoom,tmp=s.scratch,projection=patch.projection;
      if(projection!==undefined&&projection!=='orthographic')fail();
      if(p)read(tmp,0,p,3);if(t)read(tmp,3,t,3);if(c)read(tmp,7,c,2);
      if(z!==undefined){tmp[6]=Math.fround(finite(z));if(!(finite(tmp[6])>0))fail();}
      if(p)for(let k=0;k<3;k++)s.cameraState[k]=tmp[k];if(t)for(let k=3;k<6;k++)s.cameraState[k]=tmp[k];
      if(c){s.cameraState[7]=tmp[7];s.cameraState[8]=tmp[8];}if(z!==undefined)s.cameraState[6]=tmp[6];s.ready=false;
    }finally{s.busy=false;}};
    s.evaluate=function(timeMs){alive(s);if(timeMs!==undefined)finite(timeMs);s.busy=true;s.ready=false;
      try{let dirty=false;for(let k=0;k<n;k++)if(s.mask[k]){dirty=true;break;}
        if(dirty){instance.targets(s.mask,s.poses,s.colors,s.opacity);s.mask.fill(0);}
        instance.evaluate(s.cameraState,s.lightState);s.ready=true;
      }finally{s.busy=false;}};
    s.render=function(){alive(s);if(!s.ready)throw new Error('AyatsuriNative: evaluate before render');s.busy=true;try{instance.submit();}finally{s.busy=false;}};
    s.stats=function(out){alive(s);s.busy=true;try{return instance.stats(out);}finally{s.busy=false;}};
    s.dispose=function(){if(s.busy)throw new Error('AyatsuriNative: disposed/busy scene');if(s.dead)return;
      s.busy=true;try{instance.dispose();s.dead=true;s.handles.length=0;s.model=null;}finally{s.busy=false;}};
    try{if(options.light)s.light(options.light);if(options.camera)s.camera(options.camera);return s;}catch(e){instance.dispose();throw e;}
  }
  return {scene};
})();
if(typeof module!=='undefined')module.exports=globalThis.AyatsuriNative;
