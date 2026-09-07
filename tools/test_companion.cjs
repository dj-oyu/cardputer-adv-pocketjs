const assert=require('node:assert/strict'),vm=require('node:vm'),fs=require('node:fs'),path=require('node:path');
let selected=0,clock=0,wake=-1,timer=null,placed=[],texts=[];
const pet={select(n){if(n!==undefined)selected=n;return selected;},now:()=>clock,
  place(i,x,y){placed.push(i);assert.equal(x,20);assert.equal(y,32);},
  clock:()=>({wakeMinute:wake,utc:123456}),wake(h,m){wake=h<0?-1:h*60+m;},
  usage:()=>({stale:false,windows:[{usedPercent:30,resetsAt:125000},{usedPercent:70,resetsAt:129000}]}),
  rewards:()=>4,timer:()=>timer,alarm(id,seconds,label){assert.equal(id,'companion.timer');assert.equal(label,'TIMER FINISHED');timer=seconds||null;}};
const ui={createNode:k=>{assert.notEqual(k,2,'no image texture nodes');assert(texts.length<15,'16th node: past safeNodes, see pocket_ui.c layout_block()');return texts.push('')+1;},
  setProp(){},replaceText(n,t){texts[n]=t;},insertBefore(){}};
const ctx={pocket:{pet},ui,console:{log(){}}};vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(__dirname,'../apps/companion/companion.js'),'utf8'),ctx);
function key(k){clock+=100;ctx.frame(k);ctx.frame(0);}
assert.equal(placed.at(-1),0);key(0x10);assert.equal(selected,1);assert.equal(placed.at(-1),1);
key(0x20);assert(texts.some(t=>t.includes('CLAUDE')));
key(0x20);key(0x4000);assert.equal(wake,420);key(0x4000);assert.equal(wake,-1);
key(0x20);key(0x4000);assert.equal(timer,300);key(0x4000);assert.equal(timer,null);
console.log('PASS: companion placement, providers, wake toggle, timer start/stop, no texture nodes or dynamic font calls');
