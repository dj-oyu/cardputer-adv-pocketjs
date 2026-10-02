'use strict';

// Offline safety-flow tests using a deliberately small DOM adapter. This is not
// browser rendering/accessibility QA. All requests are in-memory mocks: no HTTP
// server, firmware command, subprocess, serial port, or network is used.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const root = path.join(__dirname, 'static');
class Element{
  constructor(tag='div'){
    this.tagName=tag;
    this.children=[];
    this._text='';
    this._value='';
    this.className='';
    this.dataset={};
    this.attrs={};
    this.listeners={};
    this.checked=false;
    this.disabled=false;
    this.hidden=false;
    this.scrollTop=0;
    this.scrollHeight=20;
    this.clientHeight=20;
  }
  append(...items){
    this.children.push(...items)
  }
  replaceChildren(...items){
    this.children=items;
    this._text=''
  }
  get firstElementChild(){
    return this.children[0]
  }
  get options(){
    return this.children.filter(x=>x.tagName==='option')
  }
  set value(v){
    this._value=String(v)
  }
  get value(){
    return this._value
  }
  set textContent(v){
    this._text=String(v);
    this.children=[]
  }
  get textContent(){
    return this._text+this.children.map(x=>x.textContent).join('')
  }
  setAttribute(k,v){
    this.attrs[k]=v
  }
  addEventListener(k,fn){
    (this.listeners[k]??=[]).push(fn)
  }
  emit(k){
    for(const fn of this.listeners[k]||[])fn({
      target:this
    })
  }
  querySelectorAll(q){
    const all=[];
    const visit=n=>{
      for(const c of n.children){
        all.push(c);
        visit(c)
      }
    };
    visit(this);
    return all.filter(e=>matches(e,q))
  }
  focus(){}
  scrollIntoView(){}
}
function matches(e,q){
  return q.split(',').some(raw=>{
    const s=raw.trim();
    if(s==='.plan-input')return e.className.split(' ').includes('plan-input');
    if(s.includes('input')&&e.tagName!=='input')return false;
    if(s.includes(':checked')&&!e.checked)return false;
    for(const m of s.matchAll(/\[(name|type|value)="?([^"\]]+)"?\]/g)){
      if(e[m[1]]!==m[2])return false
    }
    return s.startsWith('input')
  })
}
const ids={},statics=[];
for(const m of fs.readFileSync(root+'/index.html','utf8').matchAll(/<(\w+)([^>]*)>/g)){
  const attrs={};
  for(const a of m[2].matchAll(/([\w-]+)="([^"]*)"/g))attrs[a[1]]=a[2];
  const e=new Element(m[1]);
  Object.assign(e,{
    id:attrs.id,className:attrs.class||'',name:attrs.name,type:attrs.type,value:attrs.value||''
  });
  e.checked=/\bchecked\b/.test(m[2]);
  e.hidden=/\bhidden\b/.test(m[2]);
  e.disabled=/\bdisabled\b/.test(m[2]);
  if(attrs.id)ids[attrs.id]=e;
  statics.push(e);
}
ids['recovery-confidence'].value='unverified-candidate';
ids['test-cycles'].value='1';
const all=()=>statics.concat(...Object.values(ids).map(e=>e.querySelectorAll('input')));
const document={
  getElementById:id=>{
    assert(ids[id],id);
    return ids[id]
  },createElement:tag=>new Element(tag),querySelectorAll:q=>[...new Set(all().filter(e=>matches(e,q)))],querySelector:q=>document.querySelectorAll(q)[0]
};
const jobs=[],requests=[];
let unknownAck=false,idf=true,seq=0,apiError=null;
const run={
  id:'run1',path:'C:\\work\\run',commit:'a'.repeat(40),status:'built'
};
const candidate={
  id:'recovery1',name:'pocketjs.bin',path:'C:\\work\\good\\pocketjs.bin',bytes:2040000,modifiedUtc:'2026-10-01T00:00:00Z',kind:'app-candidate',worktree:'C:\\work'
};
const snapshot=()=>({
  project:'C:\\work',idf:{
    ready:idf
  },runs:[run],ports:[{
    device:'COM9999',description:'serial'
  }
  ],candidates:[candidate],jobs,discovery:{
    status:'complete'
  }
});
const window={
  location:{
    hash:'#token=unit-test-only-token',pathname:'/',search:''
  },history:{
    replaceState(a,b,c){
      window.location.hash='';
      window.history.url=c
    }
  },setTimeout:()=>1,clearTimeout(){},setInterval(){}
};
const context={
  document,window,URLSearchParams,AbortController,Date,console,JSON,Map,Set,Number,String,Object,Array,Error,encodeURIComponent,fetch:async(path,opts)=>{
    assert.equal(opts.headers['X-Session-Token'],'unit-test-only-token');
    assert.equal(opts.credentials,'omit');
    assert.equal(opts.redirect,'error');
    if(apiError){
      const msg=apiError;
      apiError=null;
      return {
        ok:false,status:409,json:async()=>({
          detail:msg
        })
      }
    }
    let out;
    if(path==='/api/state')out=snapshot();
    else if(path.startsWith('/api/jobs/'))out=jobs.find(j=>j.id===path.split('/').pop());
    else{
      assert.equal(opts.headers['Content-Type'],'application/json');
      const body=JSON.parse(opts.body);
      requests.push({
        path,body
      });
      if(path==='/api/plan'){
        const plan={
          id:'plan'+(++seq),digest:'b'.repeat(64),action:body.action,runRoot:run.path,commit:run.commit,port:body.port,binary:run.path+'\\pocketjs.bin',binarySha256:'c'.repeat(64),manifestSha256:'d'.repeat(64),recoveryImage:candidate.path,recoverySha256:'e'.repeat(64),recoveryConfidence:body.confidence,region:'0x10000',maxBytes:3145728,expiresUtc:new Date(Date.now()+600000).toISOString(),command:['powershell','-File','Flash-Integrated.ps1'],warnings:['plan-level warning'],required_ack:unknownAck?['unknown-guard']:(body.action==='flash'?['replace-app','device-free','manual-download','compatible-layout',body.confidence==='user-attested-known-good'?'recovery-known-good':'recovery-unverified']:['device-free','manual-reboot-home'])
        };
        const job={
          id:'job'+seq,kind:'plan',status:'succeeded',log:['plan generated'],result:{
            plan
          }
        };
        jobs.push(job);
        out={
          job_id:job.id
        };
      } else if(path==='/api/execute'){
        const job={
          id:'job'+(++seq),kind:'flash',status:'running',log:['started']
        };
        jobs.push(job);
        out={
          job_id:job.id
        }
      } else out={
        ok:true
      };
    }
    assert(out,path);
    return {
      ok:true,status:200,json:async()=>out
    };
  }
};
// Instrument only the isolated test copy; production code exports no controls.
const source=fs.readFileSync(root+'/app.js','utf8').replace(/\}\)\(\);\s*$/,'globalThis.t={state,selections,selectionValid,clearPlan,renderSnapshot,renderPlan,updateControls,refreshState,api,rememberJob};})();');
vm.createContext(context);
vm.runInContext(source,context);
const t=context.t;
const settle=async()=>{
  for(let i=0;
  i<12;
  i++)await new Promise(r=>setImmediate(r))
};
const setAction=v=>{
  for(const e of document.querySelectorAll('input[name="action"]'))e.checked=e.value===v;
  t.clearPlan()
};
(async()=>{
  await settle();
  assert.equal(window.location.hash,'');
  assert.equal(ids['run-select'].value,'');
  assert.equal(ids['port-select'].value,'');
  assert.equal(t.state.recoveryId,'');
  assert(ids['build-plan'].disabled);
  ids['run-select'].value='run1';
  ids['port-select'].value='COM9999';
  t.updateControls();
  assert(!t.selectionValid());
  setAction('test');
  assert(t.selectionValid(),'test requires no recovery');
  assert(!ids['build-plan'].disabled);
  ids['build-plan'].emit('click');
  await settle();
  assert(!ids['plan-section'].hidden);
  assert(ids['flash-warning'].hidden);
  assert.equal(ids.acknowledgements.querySelectorAll('input').length,2);
  assert(ids['plan-warnings'].textContent.includes('plan-level warning'));
  assert(ids['execute-plan'].disabled);
  ids['test-cycles'].value='21';
  ids['test-cycles'].emit('input');
  assert(ids['plan-section'].hidden);
  assert(ids['build-plan'].disabled);
  setAction('flash');
  assert(!t.selectionValid());
  t.state.recoveryId='recovery1';
  t.updateControls();
  assert(t.selectionValid(),'hidden test cycle should not invalidate flash');
  ids['build-plan'].emit('click');
  await settle();
  assert(!ids['plan-section'].hidden);
  assert(!ids['flash-warning'].hidden);
  assert.equal(ids.acknowledgements.querySelectorAll('input').length,5);
  let boxes=ids.acknowledgements.querySelectorAll('input');
  boxes[0].checked=true;
  boxes[0].emit('change');
  assert(ids['execute-plan'].disabled);
  ids['recovery-confidence'].value='user-attested-known-good';
  ids['recovery-confidence'].emit('change');
  assert(ids['plan-section'].hidden);
  ids['build-plan'].emit('click');
  await settle();
  boxes=ids.acknowledgements.querySelectorAll('input');
  assert.equal(boxes.at(-1).value,'recovery-known-good');
  for(const b of boxes)b.checked=true;
  t.updateControls();
  assert(!ids['execute-plan'].disabled);
  ids['execute-plan'].emit('click');
  await settle();
  assert.equal(requests.filter(x=>x.path==='/api/execute').length,1);
  const executed = requests.find(x=>x.path==='/api/execute').body;
  assert.deepEqual(Object.keys(executed).sort(), ['acknowledgements', 'plan_digest', 'plan_id']);
  assert.deepEqual(executed.acknowledgements, ['replace-app', 'device-free', 'manual-download', 'compatible-layout', 'recovery-known-good']);
  assert.equal(executed.plan_digest, 'b'.repeat(64));
  assert(ids['plan-section'].hidden);
  assert(ids['run-select'].disabled);
  assert.equal(t.state.jobs.get(t.state.executeJob).kind,'flash');
  jobs.at(-1).status='succeeded';
  jobs.at(-1).result={
    action:'flash',status:'command-succeeded',next:'reboot'
  };
  await t.refreshState();
  assert(!ids['run-select'].disabled);
  unknownAck=true;
  ids['build-plan'].emit('click');
  await settle();
  boxes=ids.acknowledgements.querySelectorAll('input');
  boxes[0].checked=true;
  t.updateControls();
  assert(ids['execute-plan'].disabled);
  const plan=t.state.plan;
  plan.expiresUtc='2000-01-01T00:00:00Z';
  t.updateControls();
  assert(ids['execute-plan'].disabled);
  assert(ids['plan-expires'].textContent.includes('期限切れ'));
  idf=false;
  await t.refreshState();
  assert(ids['build-plan'].disabled);
  assert(!ids['idf-error'].hidden);
  // Server eviction must remove completed history and never poll an unknown job.
  const oldExecute = t.state.executeJob;
  t.state.selectedJob = oldExecute;
  const keep = jobs.at(-1);
  jobs.splice(0, jobs.length, keep);
  keep.log = Array.from({
    length: 500
  }, (_, i) => 'line-' + i);
  await t.refreshState();
  assert.equal(t.state.jobs.size, 1);
  assert(!t.state.jobs.has(oldExecute));
  assert.equal(t.state.executeJob, null);
  assert.equal(t.state.selectedJob, keep.id);
  assert.equal(ids['job-log'].textContent.split('\n').length, 300);
  assert(ids['job-log'].textContent.startsWith('line-200'));
  apiError='Exact FastAPI error';
  await assert.rejects(t.api('/api/plan',{}),/Exact FastAPI error/);
  console.log('PASS: offline UI safety-flow tests (mock DOM/APIs; no rendering or device verification).');
})().catch(e=>{
  console.error(e);
  process.exitCode=1
});
