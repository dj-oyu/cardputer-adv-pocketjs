// Export the real PR7 test harness's deterministic projection and draw order.
import assert from 'node:assert/strict';
import {readFileSync,writeFileSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const root=fileURLToPath(new URL('../../',import.meta.url));
execFileSync('git',['-C',root,'diff','--quiet','HEAD']);
const read=n=>readFileSync(root+'/apps/derby/'+n,'utf8');
const src=readFileSync(root+'/tools/games/test_derby_pan_order.mjs','utf8');
const end=src.indexOf('\nconst read =');
assert.ok(end>0);
const fn=src.slice(src.indexOf('function runPanTests('),end)
 .replace('const h=make();let frames=0','const h=make();globalThis.__pr7Harness=h;let frames=0');
const run=new Function(fn+';return runPanTests;')();
const tests=run(read('derby_watch.js'),read('derby_view.js'),read('derby_scene.js'),read('derby_pan.js'),assert);
const h=globalThis.__pr7Harness;
assert.ok(h);
const frames=[];
for(const seed of [2,7])for(const oval of [0,1]){
 h.reset(seed,oval);let previous=0;
 for(const step of [390,391,500]){
  h.advance(step-previous);previous=step;
  for(const camera of [7,8,9])for(const u of [0,.5,1]){
   const d=h.frame(camera,u,false,1);
   frames.push({seed,oval,step,camera,interpolation:u,tier:1,...d});
  }
 }
}
const out={source_commit:execFileSync('git',['-C',root,'rev-parse','HEAD'],{encoding:'utf8'}).trim(),
 source_tree:execFileSync('git',['-C',root,'rev-parse','HEAD^{tree}'],{encoding:'utf8'}).trim(),
 camera_cut_policy:'force-cut-on-each-fixture',
 scope:'host projection/draw-order reference; not device pixels or performance',
 tests,frames};
assert.ok(process.argv[2], 'output JSON path required');
writeFileSync(process.argv[2],JSON.stringify(out,null,2)+'\n');
console.log(JSON.stringify({frames:frames.length,tests}));
