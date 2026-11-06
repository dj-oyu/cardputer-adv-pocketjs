// Protocol test only: native numeric and heap APIs have separate tests.
const vm=require('node:vm'),fs=require('node:fs'),assert=require('node:assert/strict');
const src=fs.readFileSync('apps/meshbench/mesh_bench.js','utf8');
for(const batch of [1,4,32])for(const state of ['absent','pass','fail']){
 const calls=Math.ceil(10153/batch);
 const report={done:true,ok:state==='pass',checkpoints:3376};
 // Include every native numeric key, including macro-expanded reports/stats.
 const native=fs.readFileSync('main/pocket/pocket_shiki.c','utf8');
 const keys=new Set([...native.matchAll(/q7_num\(ctx,o,"([^"]+)"/g)].map(m=>m[1]));
 for(const m of native.matchAll(/Q7_(?:REPORT|STAT)\((\w+)\)/g))if(m[1]!=='k')keys.add(m[1]);
 for(const prefix of ['last','max'])for(const key of ['Phase','Scene','Fixture','Row'])keys.add(prefix+key);
 for(const key of keys)if(!(key in report))report[key]=45000000;
 report.fcrBefore=report.fcrAfter=report.fsrBefore=report.fsrAfter=4294967295;
 const logs=[];let preflight=0,telemetry=0,evaluate=0,submit=0,clock=0;
 const D={cycles:()=>clock++,benchBuild:()=> 'host-hook-stub'};
 if(state!=='absent'){
  D.meshQ7Preflight=()=>{preflight++;return {...report,checkpoints:preflight===calls?3376:Math.min(3375,Math.floor(preflight*batch/3)),done:preflight===calls};};
  D.meshQ7Telemetry=()=>({sample:++telemetry});
 }
 function checkReport(){
  const chunks=logs.filter(x=>x.startsWith('MQ7 PREFLIGHT '));
  let json='';
  chunks.forEach((x,i)=>{assert.ok(Buffer.byteLength(x)<=200);
   const m=x.match(/^MQ7 PREFLIGHT (\d+)\/(\d+) (.*)$/);assert.ok(m);
   assert.equal(+m[1],i+1);assert.equal(+m[2],chunks.length);json+=m[3];});
  assert.deepEqual(JSON.parse(json),report);
  assert.ok(Buffer.byteLength(json)<3000);
  assert.ok(chunks.reduce((n,x)=>n+Buffer.byteLength(x)+1,0)<4096);
 }
 const actor={evaluate(){evaluate++;},submit(){submit++;}};
 const P={resource:()=>({}),beginMeshFrame(){},commit(){}};
 const V={shiki:P,replace(fn){fn({background(){},image(){}});}};
 const ctx=vm.createContext({console:{log:x=>logs.push(x)},pocket:{kasane:V,shikiDebug:D,ayatsuri:{modelOpen:()=>({instance:()=>actor})}}});
 vm.runInContext(src,ctx);assert.equal(preflight,0);
 if(state!=='absent'){
  assert.equal(logs.some(x=>x.startsWith('MB READY')),false);
  for(let n=1;n<calls;n++){
   vm.runInContext('frame();',ctx);assert.equal(preflight,n);assert.equal(evaluate,0);
  }
  const parts=Math.ceil(JSON.stringify(report).length/150);
  for(let piece=1;piece<=parts;piece++){
   const invoke=()=>vm.runInContext('frame();',ctx);
   if(state==='fail'&&piece===parts)assert.throws(invoke,/admission failed/);
   else invoke();
   assert.equal(preflight,calls);assert.equal(evaluate,0);assert.equal(submit,0);
   assert.equal(logs.filter(x=>x.startsWith('MQ7 PREFLIGHT ')).length,piece);
   assert.equal(logs.some(x=>x.startsWith('MB READY')),state==='pass'&&piece===parts);
  }
  checkReport();
  if(state==='fail'){
   vm.runInContext('frame();frame();',ctx);
   assert.equal(preflight,calls);assert.equal(evaluate,0);
   assert.equal(logs.filter(x=>x.startsWith('MQ7 PREFLIGHT ')).length,parts);continue;
  }
 }
 vm.runInContext('for(let i=0;i<153;i++)frame();',ctx);
 assert.equal(evaluate,150);assert.equal(submit,150);assert.equal(logs.filter(x=>x.startsWith('MB END')).length,1);
 if(state==='pass'){
  assert.equal(preflight,calls);assert.equal(telemetry,7);checkReport();
  const rows=logs.filter(x=>x.startsWith('MQ7 TELEMETRY')).map(x=>JSON.parse(x.slice(14)));
  assert.deepEqual(rows.map(x=>[x.phase,x.submitted]),[['ready',0],['warmup_ack',30],['submitted',60],['submitted',90],['submitted',120],['submitted',150],['end',150]]);
 }else assert.equal(telemetry,0);
}
console.log('MESHBENCH_INCREMENTAL_HOOKS_PASS one batch per turn (1/4/32), absent pass fail, one report chunk/turn, no early READY/evaluation or repeated native allocation');
