// Exact product rock callback/proj, pure geometry fixtures; full native replay is separate.
import fs from 'node:fs';import cp from 'node:child_process';import assert from 'node:assert/strict';
const base=cp.execFileSync('git',['show','e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267:apps/bigwave/big_wave.js'],{encoding:'utf8'}),candidate=fs.readFileSync('apps/bigwave/big_wave.js','utf8');
function callback(src){const a=src.indexOf('    c.rocks.forEach(r =>'),b=src.indexOf('    c.riders.forEach(r =>',a);assert(a>=0&&b>a);return src.slice(a,b);}
const proj=base.slice(base.indexOf('  function proj('),base.indexOf('  function draws('));
function run(src,far){
 const C={dn:5.2,F:150,vx:160,vy:56},S={cx:0,lc:0,vc:0,co:1,si:0};
 const depths=[C.dn-1e-9,C.dn,C.dn+1e-9,far-1e-9,far,far+1e-9,NaN,-Infinity,Infinity];
 const c={rocks:depths.map((x,i)=>({x,p:i/10}))},ob=[],co=1,si=0;
 // Geometry fixtures remain pure; the candidate may skip calls but not outputs.
 const face=(p,q,h)=>[p+q,h],curlAt=x=>x/100,heightAt=x=>1+x/200;
 Function('S','s','C','c','far','ob','co','si','face','curlAt','heightAt',proj+callback(src))(S,S,C,c,far,ob,co,si,face,curlAt,heightAt);
 return ob;
}
for(const far of [44,54,64])assert.deepEqual(run(candidate,far),run(base,far));
const mutant=candidate.replace('depth < C.dn || depth > far','depth <= C.dn || depth >= far');
assert.notDeepEqual(run(mutant,44),run(base,44),'strict-boundary mutant escaped');
console.log('BIGWAVE_ROCK_BOUNDARY PASS tiers=3 depth_cases=9 strict_mutant_rejected=true');
