// The shipped WAKE model must retain its geometry, projection and draw order.
// Compare native-call inputs to the pre-migration main for every scene frame.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {execFileSync} = require('node:child_process');
const root = path.resolve(__dirname, '..');
const baseline = execFileSync('git', ['show', '843496c:apps/kasane/megademo_wake.js'], {cwd:root, encoding:'utf8'});
function harness(source, utility) {
  const calls=[];
  const capture = (name, args) => calls.push([name, ...args.map(value => {
    if (value && ArrayBuffer.isView(value)) return Array.from(value);
    if (value && typeof value === 'object') return JSON.parse(JSON.stringify(value));
    return value;
  })]);
  const shapes={
    project(matrix,src,dst,from,count) {
      capture('project',[matrix,src,from,count]);
      for(let i=from;i<from+count;i++) {
        const x=src[3*i],y=src[3*i+1],z=src[3*i+2];
        const w=matrix[8]*x+matrix[9]*y+matrix[10]*z+matrix[11];
        dst[2*i]=w>0?(matrix[0]*x+matrix[1]*y+matrix[2]*z+matrix[3])/w:NaN;
        dst[2*i+1]=w>0?(matrix[4]*x+matrix[5]*y+matrix[6]*z+matrix[7])/w:NaN;
      }
    },
    fill(...args){capture('fill',args);}, outline(...args){capture('outline',args);}
  };
  const context=vm.createContext({Math,Float32Array,pocket:{shapes},M:Math,PI:Math.PI,WH:65535,
    BG(value){calls.push(['BG',value]);},
    smooth:u=>u*u*(3-2*u),ramp:(t,a,b)=>t<=a?0:t>=b?1:(t-a)/(b-a)});
  if(utility) vm.runInContext(fs.readFileSync(path.join(root,'apps/character/motion.js'),'utf8'),context);
  vm.runInContext(source,context);
  return t=>{calls.length=0;vm.runInContext('wake('+t+')',context);return structuredClone(calls);};
}
const before=harness(baseline,false),after=harness(fs.readFileSync(path.join(root,'apps/kasane/megademo_wake.js'),'utf8'),true);
for(let t=0;t<104;t++) assert.deepEqual(after(t),before(t),'WAKE native inputs frame '+t);
console.log('WAKE_ORACLE_PASS 104 frames: exact model/projection/draw inputs; host mock, not device timing');
