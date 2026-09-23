const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname,'../apps/pet/pet.js'),'utf8');
const E=0x4000,L=0x80,R=0x20,U=0x10,D=0x40,B=0x2000;
// This double observes domain values sent to the registered native view.
const APP_COMMANDS=80, APP_TEXT_BYTES=896;
function makeKasane() {
  const refs=[], model={};
  const view={resource(id){assert.equal(id,'pets');return {};},mount(id){assert.equal(id,'pet');return {set(update){
    Object.assign(model,update);refs.length=0;
    for(const key of ['title','index','species','status','food','joy','energy','foot','hint','note']){
      if(model[key]&&!(key==='note'&&!model.bubble)){
        assert(Buffer.byteLength(model[key],'utf8')<=47,'native slot text budget');
        refs.push({kind:'text',visible:true,text:model[key]});
      }
    }
    if(model.ready)refs.push({kind:'image',variant:model.variant,frame:model.frame,
                              bounds:[27,model.petY,91,model.petY+64]});
    assert(refs.length<=APP_COMMANDS);
    assert(refs.filter(r=>r.kind==='text').reduce((n,r)=>n+Buffer.byteLength(r.text),0)<=APP_TEXT_BYTES);
  }};}};
  return {view,refs};
}
async function boot(initial, failRead=false, failWrite=false) {
  let time=0, saved=initial, writes=0, selected=initial&&initial.selected<12?initial.selected:0, reward=0;
  const k=makeKasane();
  const ctx={console:{log(){}},__petNow:()=>time,
    pocket:{kasane:k.view,pet:{select:(n)=>{if(n!==undefined){if(failWrite)throw Object.assign(Error(),{code:'IO_ERROR',outcome:'not-applied'});selected=n;}return selected;},rewards:()=>reward},
      storage:{get:()=>failRead?Promise.reject(Error()):Promise.resolve(initial?{value:structuredClone(initial)}:null),
        set:(key,v)=>{assert.equal(key,'pet.v1');if(failWrite)return Promise.reject(Error());saved=JSON.parse(JSON.stringify(v));writes++;return Promise.resolve();}}}};
  vm.createContext(ctx);vm.runInContext(source,ctx);await Promise.resolve();
  const petRef=()=>k.refs.find(r=>r.kind==='image');
  return {key(b){ctx.frame(b);ctx.frame(0);},advance(ms){for(let n=0;n<ms;n+=1000){time+=Math.min(1000,ms-n);ctx.frame(0);}},
    feed(n){reward=n;},step(ms){time+=ms;ctx.frame(0);},
    get pose(){const p=petRef();return p&&{i:p.variant,y:p.bounds[1],m:p.frame};},
    get saved(){return saved;},get writes(){return writes;},get picture(){const p=petRef();return p?p.variant:-1;},
    has(t){return k.refs.some(r=>r.kind==='text'&&r.visible&&r.text.includes(t));},
    refs:k.refs};
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
  // Exercise real frame timing: blink closes both eyes and reopens, while
  // a care reaction takes priority. Selection must use the displayed pet.
  let motion=await boot();motion.key(E);motion.step(4900);assert.equal(motion.pose.m,0);
  motion.step(100);assert.equal(motion.pose.m,1);
  motion.step(100);assert.equal(motion.pose.m,1);
  motion.step(100);assert.equal(motion.pose.m,0);
  motion.step(4700);motion.key(E);motion.step(100);assert.equal(motion.pose.m,2);
  motion.key(R);motion.key(R);motion.key(E);assert.equal(motion.pose.y,31);
  motion.key(R);motion.key(R);motion.key(E);motion.key(R);
  assert.equal(motion.pose.i,1);assert.notEqual(motion.pose.y,31);
  motion.key(E);assert.notEqual(motion.pose.y,31);
  assert(motion.refs.length<=APP_COMMANDS);
  console.log('PASS: selection, care, sleep, naming, per-pet persistence, timing, corrupt data, I/O failures, asset size, Kasane command/text budget');
})().catch(e=>{console.error(e);process.exitCode=1;});
