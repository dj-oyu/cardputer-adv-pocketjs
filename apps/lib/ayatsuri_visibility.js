// Optional projected-face selection. It does not mutate topology or draw.
'use strict';
globalThis.AyatsuriVisibility=(()=>{
  const states=new WeakMap();
  function fail(message){throw new TypeError('AyatsuriVisibility: '+message);}
  function alive(s){if(!s||s.dead||s.busy)throw new Error('AyatsuriVisibility: disposed/busy selector');}
  const Result={get indices(){const s=states.get(this);alive(s);return s.output;},
    get triangleCount(){const s=states.get(this);alive(s);return s.counts[0];},
    get backfaceCount(){const s=states.get(this);alive(s);return s.counts[1];},
    get degenerateCount(){const s=states.get(this);alive(s);return s.counts[2];}};
  const Selector={evaluate(xy,depth){
    const s=states.get(this);alive(s);s.busy=true;
    try{
      if(!(xy instanceof Float32Array)||xy.length!==s.n*2||!(depth instanceof Float32Array)||depth.length!==s.n)
        fail('matching Float32Array xy/depth required');
      s.backend.selectVisible(xy,depth,s.source,s.output,s.counts,s.order);
    }finally{s.busy=false;}
    return s.result;
  },dispose(){
    const s=states.get(this);if(!s||s.dead)return;if(s.busy)throw new Error('AyatsuriVisibility: busy selector');
    s.dead=true;s.source=null;s.output=null;s.counts=null;s.backend=null;s.result=null;
  }};
  return Object.freeze({create(options){
    if(!options||typeof options!=='object')fail('options required');
    const n=options.vertexCount,source=options.indices,order=options.order===undefined?'authored':options.order;
    if(!Number.isInteger(n)||n<1||n>32||!(source instanceof Uint16Array)||!source.length||source.length%3||source.length>192)
      fail('1..32 vertices and 1..64 Uint16 index triples required');
    if(order!=='authored'&&order!=='centroid-depth')fail('order must be authored or centroid-depth');
    const backend=pocket.ayatsuri;
    const output=new Uint16Array(source.length),counts=new Uint32Array(3),xy=new Float32Array(n*2),depth=new Float32Array(n);
    // Native validation rejects shared/detached/unaligned input before copying.
    backend.selectVisible(xy,depth,source,output,counts,0);
    const copy=new Uint16Array(source),selector=Object.create(Selector),result=Object.create(Result),
      state={n,source:copy,output,counts,backend,order:order==='authored'?0:1,dead:false,busy:false,result};
    counts.fill(0);states.set(selector,state);states.set(result,state);Object.freeze(result);return selector;
  }});
})();
