// Continuous real DERBY poses/rin arguments. No integer or perspective rewrite.
import assert from 'node:assert/strict';
import {readFileSync,writeFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
const root=fileURLToPath(new URL('../../',import.meta.url));
const read=n=>readFileSync(root+'/apps/derby/'+n,'utf8');
const test=readFileSync(root+'/tools/games/test_derby_pan_order.mjs','utf8');
const fn=test.slice(test.indexOf('function runPanTests('),test.indexOf('\nconst read ='))
 .replace('const h=make();let frames=0','const h=make();globalThis.__affineHarness=h;let frames=0');
assert.ok(fn.includes('globalThis.__affineHarness'));
new Function(fn+';return runPanTests;')()(read('derby_watch.js'),read('derby_view.js'),read('derby_scene.js'),read('derby_pan.js'),assert);
const h=globalThis.__affineHarness,view=read('derby_view.js'),a=view.indexOf('const rin = '),b=view.indexOf(';',a);
assert.ok(a>=0&&b>a);
const rin=new Function('dr','const sin=Math.sin;'+view.slice(a,b+1)+';return rin;');
const lines=[];let seq=0,frames=0;
const emit=(frame,horse,visible,input)=>lines.push([seq,frame,horse,+visible,...input].join(' '));
for(const seed of [2,7])for(const oval of [0,1])for(const camera of [7,8,9])for(const tier of [0,1,2]){
 ++seq;h.reset(seed,oval);h.advance(389);
 for(let step=390;step<=510;step++){
  h.advance(1);
  // Three successive presentation instants per simulation step; fixed camera
  // position, continuously tracked aim, no forced random camera cuts.
  for(const u of [0,1/3,2/3]){
   const f=h.frame(camera,u,false,tier),frame=(step-390)*3+Math.round(u*3);++frames;
   const visible=new Set(f.drawn.map(r=>r.l));
   for(let l=0;l<8;l++){
    const p=f.projected[l],r=1/p[1],X=p[0]*r;
    // Use the actual rin source for body/leg inputs. Its phase is driven
    // continuously here, rather than the order harness's constant ph array.
    let input;
    rin((_,...v)=>input=v)(l,X,r/6,28+6*r,step*.13+u*.13+l*.1);
    emit(frame,l,visible.has(l),input);
   }
  }
 }
}
// Near-plane visibility/candidate fallback transition. Projection and its
// strict Z>.02 visibility predicate are copied from the unchanged pj() source.
const pan=read('derby_pan.js');const start=pan.indexOf('function pj('),end=pan.indexOf('\n// The series',start);
const project=new Function('pc',pan.slice(start,end)+';return pj;')([0,0,0,1,200,4]);
++seq;
for(let i=0;i<601;i++){
 const Z=.018+i*.00001,p=project([0,Z*200]),r=1/p[1],X=p[0]*r;let input;
 rin((_,...v)=>input=v)(0,X,r/6,28+6*r,i*.01);
 emit(i,0,p[1]>.02&&X>-160&&X<400,input);++frames;
}
// Subpixel pan over a fixed outline: exposes extra quantization toggles
// separately from the float VM's ordinary stair-step pixel movement.
++seq;
for(let i=0;i<1201;i++){
 let input;rin((_,...v)=>input=v)(0,110+i*.001,1.0001+i*.000001,90,.4);
 emit(i,0,true,input);++frames;
}
assert.ok(process.argv[2]);writeFileSync(process.argv[2],lines.join('\n')+'\n');
console.log(JSON.stringify({sequences:seq,presentation_frames:frames,runner_inputs:lines.length,
 scope:'actual pose/pj/rin expressions; deterministic phase sweep; host only, no target timing'}));
