// Protocol test only: native numeric and heap APIs have separate tests.
const vm=require('node:vm'),fs=require('node:fs'),assert=require('node:assert/strict');
const src=fs.readFileSync('apps/meshbench/mesh_bench.js','utf8');
for(const state of ['absent','pass','fail']){
 const logs=[];let preflight=0,telemetry=0,evaluate=0,submit=0,clock=0;
 const D={cycles:()=>clock++,benchBuild:()=> 'host-hook-stub'};
 if(state!=='absent'){
  D.meshQ7Preflight=()=>{preflight++;return {ok:state==='pass'};};
  D.meshQ7Telemetry=()=>({sample:++telemetry});
 }
 const actor={evaluate(){evaluate++;},submit(){submit++;}};
 const P={resource:()=>({}),beginMeshFrame(){},commit(){}};
 const V={shiki:P,replace(fn){fn({background(){},image(){}});}};
 const ctx=vm.createContext({console:{log:x=>logs.push(x)},pocket:{kasane:V,shikiDebug:D,ayatsuri:{modelOpen:()=>({instance:()=>actor})}}});
 if(state==='fail'){
  assert.throws(()=>vm.runInContext(src,ctx),/admission failed/);
  assert.equal(logs.some(x=>x.startsWith('MB READY')),false);assert.equal(evaluate,0);continue;
 }
 vm.runInContext(src,ctx);vm.runInContext('for(let i=0;i<153;i++)frame();',ctx);
 assert.equal(evaluate,150);assert.equal(submit,150);assert.equal(logs.filter(x=>x.startsWith('MB END')).length,1);
 if(state==='pass'){
  assert.equal(preflight,1);assert.equal(telemetry,7);
  const rows=logs.filter(x=>x.startsWith('MQ7 TELEMETRY')).map(x=>JSON.parse(x.slice(14)));
  assert.deepEqual(rows.map(x=>[x.phase,x.submitted]),[['ready',0],['warmup_ack',30],['submitted',60],['submitted',90],['submitted',120],['submitted',150],['end',150]]);
 }else assert.equal(telemetry,0);
}
console.log('MESHBENCH_PREFLIGHT_HOOKS_PASS absent pass fail; no target numeric/heap claim');
