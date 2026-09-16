const assert=require('node:assert/strict'),vm=require('node:vm'),fs=require('node:fs'),path=require('node:path');
let selected=0,clock=0,wake=-1,timer=null;
const pet={select(n){if(n!==undefined)selected=n;return selected;},now:()=>clock,
  clock:()=>({wakeMinute:wake,utc:123456,minute:600}),wake(h,m){wake=h<0?-1:h*60+m;},
  usage:()=>({stale:false,windows:[{usedPercent:30,resetsAt:125000},{usedPercent:70,resetsAt:129000}]}),
  rewards:()=>4,timer:()=>timer,alarm(id,seconds,label){assert.equal(id,'companion.timer');assert.equal(label,'TIMER FINISHED');timer=seconds||null;}};

// Minimal pocket.kasane double: runs the real scene controller
// (apps/kasane/create_scene.js) against stub tx/ref objects so createScene's
// pending/dirty state machine matches production, not a reimplementation.
let onAction;
const input={onAction(fn){onAction=fn;}};
const images=[],texts=[];
function makeTextRef(){const r={};r.setText=(tx,t)=>{r.text=t;texts.push(t);};return r;}
function makeImageRef(spec){const r={variant:spec.variant,frame:spec.frame};
  r.setImageFrame=(tx,variant,frame)=>{r.variant=variant;r.frame=frame;images.push(variant);};return r;}
function makeTx(){return{background(){},rect(){},
  image(spec){images.push(spec.variant);return makeImageRef(spec);},
  text(spec){texts.push(spec.text);return makeTextRef();}};}
let status='PRESENTED';
const view={petImage:()=>({width:64,height:64,variants:12,frames:6}),
  replace(fn){this._candidate=fn(makeTx());status='PRESENTED';},
  patch(fn){fn(makeTx());status='PRESENTED';},
  poll:()=>({status}),cancel(){}};
view.createScene=function(opts){
  const src=fs.readFileSync(path.join(__dirname,'../apps/kasane/create_scene.js'),'utf8');
  return vm.runInNewContext('('+src+')',{})(this,opts);
};
const ctx={pocket:{pet,kasane:view,input},console:{log(){}}};vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(__dirname,'../apps/companion/companion.js'),'utf8'),ctx);
// One frame suffices: createScene's flush() both promotes the pending submit
// and runs the patch reacting to the invalidate() the action just made.
function key(action){clock+=1100;onAction({action,phase:'press'});ctx.frame();}
assert.equal(images.at(-1),0);key('up');assert.equal(selected,1);assert.equal(images.at(-1),1);
key('right');assert(texts.some(t=>t.includes('CLAUDE')));
key('right');key('accept');assert.equal(wake,420);key('accept');assert.equal(wake,-1);
key('right');key('accept');assert.equal(timer,300);key('accept');assert.equal(timer,null);
console.log('PASS: companion Kasane image/text refs, pet select, wake toggle, timer start/stop, no legacy ui.* calls');
