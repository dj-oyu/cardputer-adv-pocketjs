const assert=require('node:assert/strict'),vm=require('node:vm'),fs=require('node:fs'),path=require('node:path');
let selected=0,clock=0,wake=-1,timer=null;
const pet={select(n){if(n!==undefined)selected=n;return selected;},now:()=>clock,
  clock:()=>({wakeMinute:wake,utc:123456,minute:600}),wake(h,m){wake=h<0?-1:h*60+m;},
  usage:()=>({stale:false,windows:[{usedPercent:30,resetsAt:125000},{usedPercent:70,resetsAt:129000}]}),
  rewards:()=>4,timer:()=>timer,alarm(id,seconds,label){assert.equal(id,'companion.timer');assert.equal(label,'TIMER FINISHED');timer=seconds||null;}};

let onAction;
const input={onAction(fn){onAction=fn;}};
const images=[],texts=[];
const view={resource(id){assert.equal(id,'pets');return {};},mount(id){assert.equal(id,'companion');return{set(values){
  images.push(values.variant);
  for(const key of ['head','line0','line1','line2','line3','foot','hint'])texts.push(values[key]);
}};}};
const ctx={pocket:{pet,kasane:view,input},console:{log(){}}};vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(__dirname,'../apps/companion/companion.js'),'utf8'),ctx);
function key(action){clock+=1100;onAction({action,phase:'press'});ctx.frame();}
assert.equal(images.at(-1),0);key('up');assert.equal(selected,1);assert.equal(images.at(-1),1);
key('right');assert(texts.some(t=>t.includes('CLAUDE')));
key('right');key('accept');assert.equal(wake,420);key('accept');assert.equal(wake,-1);
key('right');key('accept');assert.equal(timer,300);key('accept');assert.equal(timer,null);
console.log('PASS: companion native-view values, pet select, wake toggle, timer start/stop');
