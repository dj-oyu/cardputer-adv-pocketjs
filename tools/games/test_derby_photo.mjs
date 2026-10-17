// Run the real frame/at paths without the renderer: the finish photo must
// keep positions, lane depths and far-to-near order from the same instant.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const read = name => fs.readFileSync(new URL(`../../apps/derby/${name}`, import.meta.url), 'utf8');
const scene = read('derby_scene.js');
const at = scene.slice(scene.indexOf('function at(u)'), scene.indexOf('// Finish:', scene.indexOf('function at(u)')));
for (const oval of [0, 1]) {
  const ctx = vm.createContext({ console });
  vm.runInContext(`
    const mx=Math.max,mn=Math.min,M=Math,sin=Math.sin,rnd=Math.round,PI=Math.PI,DT=.05,D=1000,Q=1.2,OB=273,VS=[800];
    let scene='race',t=0,camT=0,pts=1000,raceNo=1,pick=0,stake=50,A=null;
    let slow=1,disp=.75,ld=0,dl=0,hold=0,man=0,cm=3,cl=0,cam=0,need=0,replay=0,photoX=null;
    let ph=Array(8).fill(0),lz=Array(8).fill(0),zo=[7,6,5,4,3,2,1,0],ro=[0,1,2,3,4,5,6,7];
    let F={o:${oval},k:[0,1,2,3,4,5,6,7],h:Array.from({length:8},()=>({sty:0}))};
    let rs={t:10,done:0,x:[999.5,999.4,997,995,992,990,988,985],px:[],l:[12,${oval ? 11.95768 : 11.99768},13,14,15,16,17,18],v:Array(8).fill(20),tc:Array(8).fill(10.025)};
    const K={down:()=>[],pressed:()=>false},pocket={},live={},queue=[],NAMES=[],V={patch:()=>{}};
    const log=()=>{},load=()=>{},drop=()=>{},want=()=>{},sound=()=>{},save=()=>{},hud=()=>{},build=()=>{};
    const z8=n=>Array(8).fill(n),order=()=>[0,1,2,3,4,5,6,7],wide=()=>0;
    const enter=s=>{scene=s;t=0},settle=()=>{};
    function step(){rs.px=rs.x.slice();rs.x=rs.x.map(x=>x+1);rs.t+=DT;rs.done=rs.done?3:1;}
    let captured,drawn,latest;
    function shot(){return [];}
    function paint(c,x){drawn={x:x.slice(),z:lz.slice(),o:zo.slice()};}
    ${at}
    const originalAt=at;
    at=u=>{const x=originalAt(u);latest={x:x.slice(),z:lz.slice(),o:zo.slice()};if(!captured)captured=latest;return x;};
  `, ctx);
  vm.runInContext(read('derby_play.js'), ctx);
  vm.runInContext(`attract=()=>{};scene='race';t=0;frame(0);const after=lz.slice();disp=.75;frame(0);frame(0);`, ctx);
  const state=vm.runInContext('({captured,drawn,after,latest})',ctx);
  assert.notDeepEqual(state.after,state.captured.z,'fixture must change lanes after the crossing');
  assert.notDeepEqual(state.latest.o,state.captured.o,'fixture must change draw order after the crossing');
  assert.deepEqual(state.drawn.x,state.captured.x,'photo longitudinal positions');
  assert.deepEqual(state.drawn.z,state.captured.z,'photo lane depths');
  assert.deepEqual(state.drawn.o,state.captured.o,'photo depth order');
  vm.runInContext('frame(0)',ctx);
  assert.deepEqual(vm.runInContext('drawn',ctx),state.drawn,'photo remains frozen across frames');
}
console.log('PASS: straight/oval finish photos retain crossing positions, depths and draw order');
