// Actual model/settle/HUD source; completed ties and the existing time-cap boundary.
import fs from 'node:fs';
import vm from 'node:vm';
import cp from 'node:child_process';
import assert from 'node:assert/strict';
const watch=fs.readFileSync('apps/derby/derby_watch.js','utf8');
const view=fs.readFileSync('apps/derby/derby_view.js','utf8');
const formats=view.slice(view.indexOf('const hex ='),view.indexOf(';',view.indexOf('const hex ='))+1);
const candidate=fs.readFileSync('apps/derby/derby_scene.js','utf8');
const baseline=cp.execFileSync('git',['show','e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267:apps/derby/derby_scene.js'],{encoding:'utf8'});
function context(scene) {
 const c=vm.createContext({Math,log(){},save(){},WIN:[],LOSE:[]});
 vm.runInContext(watch+'\n'+formats+'\n'+scene,c);
 return c;
}
const contexts=[context(baseline),context(candidate)];
let checked=0;
for(const partial of [false,true])for(let pick=0;pick<8;pick++) {
 const states=contexts.map(c=>vm.runInContext(`
  globalThis.F=field(1234);globalThis.pick=${pick};globalThis.replay=0;
  globalThis.stake=100;globalThis.pts=1000;globalThis.od=[2,2,2,2,2,2,2,2];
  globalThis.raceNo=1;globalThis.notes=[];globalThis.fin=null;
  globalThis.rs={done:${partial?3:8},t:200,tc:${partial?'[60,60,62,0,0,0,0,0]':'[60,60,62,63,64,65,66,67]'},x:[1005,1005,1001,998,998,995,990,990],v:[15,15,15,15,15,15,15,15]};
  settle();globalThis.scene='photo';globalThis.camT=0;globalThis.cm=0;globalThis.NAMES=[];
  globalThis.R={t:Array.from({length:5},()=>({setText(tx,s){this.text=s;}}))};
  JSON.stringify({order:fin.o,points:pts,margin:fin.mg});`,c));
 assert.equal(states[0],states[1]);
 const originals=contexts.map(c=>vm.runInContext('JSON.stringify(fin.o)',c));
 for(const t of [0,8,49,50,110])for(let repeat=0;repeat<2;repeat++) {
  const labels=contexts.map(c=>vm.runInContext(`globalThis.t=${t};hud(null);JSON.stringify(R.t.map(x=>x.text));`,c));
  assert.equal(labels[0],labels[1]);
  contexts.forEach((c,i)=>assert.equal(vm.runInContext('JSON.stringify(fin.o)',c),originals[i]));
  checked++;
 }
}
console.log(JSON.stringify({actual_model_settle_hud:true,completed_ties:true,time_cap_partial:true,picks:8,paired_label_cases:checked,fin_order_unchanged:true}));
