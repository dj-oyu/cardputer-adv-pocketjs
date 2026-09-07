const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname,'../apps/pet/pet.js'),'utf8');
const E=0x4000,L=0x80,R=0x20,U=0x10,D=0x40,B=0x2000;
async function boot(initial, failRead=false, failWrite=false) {
  let time=0, saved=initial, writes=0, picture=-1, selected=initial&&initial.selected<12?initial.selected:0, reward=0, nodes=[null,{props:{}}];
  const ui={createNode(k){assert(nodes.length-2<15,'16th node: past safeNodes, see pocket_ui.c layout_block()');nodes.push({kind:k,props:{}});return nodes.length-1;},
    setProp(n,k,v){nodes[n].props[k]=v;},setText(n,t){nodes[n].text=t;},insertBefore(){}};
  ui.replaceText=ui.setText;ui.setText=()=>{throw Error('ASCII UI must not grow the dynamic font atlas');};
  const ctx={ui,console:{log(){}},__petNow:()=>time,__petImage:(n,i)=>{picture=i;},
    pocket:{pet:{say:t=>{assert(t.length<=22);},place:(i,x,y,m)=>{assert(i>=0&&i<12);assert.equal(x,27);assert(y>=29&&y<=31);assert(m===undefined||m>=0&&m<=5);picture=i;},show:(n,i)=>{picture=i;},select:(n)=>{if(n!==undefined){if(failWrite)throw Object.assign(Error(),{code:'IO_ERROR',outcome:'not-applied'});selected=n;}return selected;},rewards:()=>reward},storage:{get:()=>failRead?Promise.reject(Error()):Promise.resolve(initial?{value:structuredClone(initial)}:null),
      set:(k,v)=>{assert.equal(k,'pet.v1');if(failWrite)return Promise.reject(Error());saved=JSON.parse(JSON.stringify(v));writes++;return Promise.resolve();}}}};
  vm.createContext(ctx);vm.runInContext(source,ctx);await Promise.resolve();
  return {key(k){ctx.frame(k);ctx.frame(0);},advance(ms){for(let n=0;n<ms;n+=1000){time+=Math.min(1000,ms-n);ctx.frame(0);}},
    feed(n){reward=n;},get saved(){return saved;},get writes(){return writes;},get picture(){return picture;},
    has(t){return nodes.some(n=>n&&n.text&&n.text.includes(t));}};
}
(async()=>{
  let a=await boot();assert.equal(a.picture,0);
  a.key(L);assert.equal(a.picture,3);a.key(D);assert.equal(a.picture,7);
  a.key(D);assert.equal(a.picture,11);a.key(D);assert.equal(a.picture,3);
  a.key(R);assert.equal(a.picture,0);a.key(U);assert.equal(a.picture,8);
  a.key(E);assert.equal(a.saved.selected,8);
  a.key(E);assert.equal(a.saved.pets[8].food,100);
  a.key(R);a.key(E);assert.equal(a.saved.pets[8].fun,100);assert.equal(a.saved.pets[8].energy,72);
  a.key(R);a.key(E);assert.equal(a.saved.pets[8].sleep,true);
  a.advance(30000);a.key(B);assert(a.saved.pets[8].energy>95);
  a.key(L);a.key(E);assert(a.has('WAKE ME FIRST'));
  assert(Math.abs(a.saved.pets[8].fun-99.75)<1e-9);
  a.key(R);a.key(E);assert.equal(a.saved.pets[8].sleep,false);
  a.key(R);a.key(E);a.key(U);a.key(E);assert.equal(a.saved.pets[8].name,'HRAY');
  a.key(R);a.key(E);a.key(R);a.key(E);assert.equal(a.saved.selected,9);
  assert.equal(a.saved.pets[8].name,'HRAY');assert.equal(a.saved.pets[9].food,80);
  let b=await boot(a.saved);assert.equal(b.picture,9);assert(b.has('YELLOW'));
  b.advance(60000);assert.equal(b.writes,1);assert(b.saved.pets[9].food<80);
  b.key(B);assert.equal(b.writes,2);
  let food=b.saved.pets[9].food;b.feed(5);b.advance(1);b.key(B);assert(b.saved.pets[9].food>food+4.9);
  food=b.saved.pets[9].food;b.advance(1);b.key(B);assert(b.saved.pets[9].food<=food);
  let corrupt={v:1,selected:999,pets:Array(12).fill(null)};
  b=await boot(corrupt);b.key(B);assert.equal(b.saved.selected,0);assert.equal(b.saved.pets[0].food,80);
  b=await boot(null,true);b.key(E);assert.equal(b.writes,0);assert(b.has('LOAD FAILED'));
  b=await boot(null,false,true);b.key(E);assert(b.has('SAVE FAILED'));
  b=await boot(null,false,true);b.key(E);await Promise.resolve();b.advance(1000);assert(b.has('SAVE FAILED'));
  const compact=fs.readFileSync(path.join(__dirname,'../apps/pet/assets/pets-compact.bin'));
  assert.equal(compact.subarray(0,4).toString(),'PPT2');assert(compact.length<8192);
  console.log('PASS: selection, care, sleep, naming, per-pet persistence, timing, corrupt data, I/O failures, asset size');
})().catch(e=>{console.error(e);process.exitCode=1;});
