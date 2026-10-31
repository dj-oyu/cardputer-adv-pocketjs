// Optional local albedo plane; loaded only by apps that use this pattern.
'use strict';
globalThis.AyatsuriLocalPlane=(()=>{
  function fail(text){throw new TypeError('Ayatsuri: '+text);}
  function finite(x){if(typeof x!=='number'||x-x!==0)fail('finite number required');return x;}
  const Pattern={kind:'localPlane',code:4,encode(work){
    const n=this.normal;work[49]=n[0];work[50]=n[1];work[51]=n[2];
    work[52]=this.offset;work[53]=this.feather;work[54]=0;work[56]=0;work[57]=1;
  }};
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
  const api={create};if(typeof module!=='undefined')module.exports=api;return api;
})();
