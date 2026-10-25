// Execute the actual five source statements; compare exact numeric components.
import fs from 'node:fs';
import cp from 'node:child_process';
const base=cp.execFileSync('git',['show','e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267:apps/bigwave/big_wave.js'],{encoding:'utf8'}).split('\n');
const candidate=fs.readFileSync('apps/bigwave/big_wave.js','utf8').split('\n');
const indices=base.flatMap((s,i)=>s.includes('.concat(w,')?[i]:[]);
if(indices.length!==5||base.length!==candidate.length)throw Error('unexpected source shape');
const names=['e','w','ex','sh','C','m0','m1','s','n','q0','dq','H','a0','fb','s0','y','add'];
const forms=indices.map(i=>[base[i],candidate[i]].map(s=>new Function(...names,s)));
const values=[0,-0,NaN,Infinity,-Infinity,Number.MAX_VALUE,-Number.MAX_VALUE,Number.MIN_VALUE,-Number.MIN_VALUE,1,-1,.25];
let checked=0;
for(let phase=0;phase<values.length;phase++)for(const sh of [false,true])for(const foam of [false,true])for(const form of forms){
 const v=i=>values[(phase+i)%values.length];
 const results=form.map(fn=>{
  const calls=[],ys=[],e=[v(0),v(1)],w=[v(2),v(3)],ex=[v(4),v(5)];
  fn(e,w,ex,sh,{dn:2},3,9,{t:v(6),wt:v(7)},4,v(8),v(9),v(10),5,foam?7:1,2,m=>{ys.push(m);return m===3?-0:.25},(k,input)=>calls.push({k,input}));
  return {calls,ys,e,w,ex};
 });
 const [a,b]=results;
 if(JSON.stringify(a.ys)!==JSON.stringify(b.ys)||a.calls.length!==b.calls.length)throw Error('evaluation/order changed');
 for(let j=0;j<a.calls.length;j++){
  const x=a.calls[j],y=b.calls[j];
  if(x.k!==y.k||!Array.isArray(y.input)||x.input.length!==y.input.length)throw Error('shape changed');
  for(let k=0;k<x.input.length;k++)if(!Object.is(x.input[k],y.input[k]))throw Error(`numeric component ${k} changed`);
 }
 for(const key of ['e','w','ex'])for(let j=0;j<2;j++)if(!Object.is(a[key][j],b[key][j]))throw Error('source mutated');
 checked++;
}
console.log(JSON.stringify({actual_source_forms:forms.length,cases:checked,sh_both:true,foam_both:true,object_is_equal:true,scope:'Private dense numeric arrays and normal Array builtins; not arbitrary concat/species/prototype monkeypatches.'}));
