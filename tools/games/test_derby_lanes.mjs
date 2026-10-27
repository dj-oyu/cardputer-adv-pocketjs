// Regression checks for the real at()/photo() and race model, without a renderer.
// Run: node tools/games/test_derby_lanes.mjs
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

function runLaneTests(sceneSource, modelSource, assert) {
  const start = sceneSource.indexOf('function at(u)');
  const end = sceneSource.indexOf('// Finish:', start);
  assert.ok(start >= 0 && end > start, 'real at/photo source is present');
  const make = () => new Function(modelSource.slice(0, modelSource.indexOf('globalThis.derby =')) + `
    let rs, F, lz, zo, photoX;
    ${sceneSource.slice(start, end)}
    return {
      reset(seed, oval) {
        F = field(seed); F.o = oval; rs = race(F); rs.l = DL.slice();
        F.k = F.m.map(m => F.m.filter(v => v > m).length);
        lz = DL.slice(); zo = [7,6,5,4,3,2,1,0];
      },
      fixture(px, x, w, oval = 0, rank = [0,1,2,3,4,5,6,7]) {
        F = {o:oval, k:rank, h:Array.from({length:8}, (_,i)=>({sty:i%3}))};
        rs = {t:1, px:px.slice(), x:x.slice(), l:w.slice(), tc:Array(8).fill(0), done:0};
        lz = []; zo = [7,6,5,4,3,2,1,0];
      },
      draw(u) { const x = at(u); return {x, z:lz.slice(), o:zo.slice(), w:rs.l.slice()}; },
      step() { step(F, rs, DT); },
      state() { return {t:rs.t, x:rs.x.slice(), px:rs.px.slice(), tc:rs.tc.slice(), done:rs.done, o:order(rs), w:rs.l.slice(), k:F.k.slice(), sty:F.h.map(h=>h.sty)}; },
      freeze(u) { photoX = photo(u); return photoX; },
      photo() { return photo(); },
      depths() { return {z:lz.slice(), o:zo.slice()}; }
    };
  `)();
  const h = make(), lanes = Array.from({length:8}, (_,i)=>12*Math.pow(1.085,i));
  const near = (a,b,msg) => assert.ok(Math.abs(a-b)<1e-11, msg + ': '+a+' vs '+b);
  let frames = 0, steps = 0;
  const check = d => {
    const lead = Math.max(...d.x);
    for (let i=0;i<8;++i) near(d.z[i], d.w[i] + Math.min(.4,.03*(lead-d.x[i]))*Math.sin(d.x[i]/40+2.4*i), 'sway uses displayed gap');
    assert.deepEqual(d.o.slice().sort((a,b)=>a-b), [0,1,2,3,4,5,6,7], 'depth order is a permutation');
    for(let i=1;i<8;++i) assert.ok(d.z[d.o[i-1]] >= d.z[d.o[i]], 'far to near');
    ++frames;
  };
  // Constant-speed leader, and a lead change inside one interpolated step.
  for (const pair of [
    [Array.from({length:8},(_,i)=>100-i),Array.from({length:8},(_,i)=>101-i)],
    [[100,99.8,98,97,96,95,94,93],[100.2,100.8,99,98,97,96,95,94]]
  ]) {
    h.fixture(pair[0], pair[1], lanes);
    for(const u of [0,.25,.5,.75,1]) {
      const d=h.draw(u); check(d);
      const leader=d.x.indexOf(Math.max(...d.x));
      near(d.z[leader],d.w[leader],'displayed leader does not sway');
    }
  }
  // Each segment uses the runner's own position, including exact boundaries.
  for(const x of [272.999,273,649.999,650]) {
    h.fixture(Array(8).fill(x-1),Array(8).fill(x),lanes,1,[7,6,5,4,3,2,1,0]);
    const d=h.draw(1), s=h.state();
    for(let i=0;i<8;++i) {
      const j=x<273?i*(.3+.35*s.sty[i]):x<650?s.k[i]:Math.min(7,s.k[i]+s.sty[i]);
      near(d.w[i],lanes[i]+Math.max(-.04,Math.min(.04,12*Math.pow(1.085,j)-lanes[i])),'segment steering');
    }
    assert.deepEqual(h.draw(.5).w,d.w,'repeated draw does not steer twice');
    check(d);
  }
  for(const oval of [0,1]) for(let seed=1;seed<=8;++seed) {
    const control=make(); h.reset(seed,oval); control.reset(seed,oval);
    while(h.state().done<8 && h.state().t<200) {
      const prev=h.state(); h.step(); control.step(); ++steps;
      let first;
      for(const u of [0,.25,.5,.75,1]) {
        const d=h.draw(u); check(d);
        if(!first) first=d.w;
        else assert.deepEqual(d.w,first,'one steering update per simulation step');
        for(let i=0;i<8;++i) assert.ok(Math.abs(d.w[i]-prev.w[i])<=.040000000001,'0.8 m/s steering cap');
        if(!oval) assert.deepEqual(d.w,lanes,'straight steering stays in original lanes');
      }
      const actual=h.state(), expected=control.state();
      for(const key of ['t','x','px','tc','done','o']) assert.deepEqual(actual[key],expected[key],'drawing preserves race '+key);
    }
    const frozen=h.freeze(.5), snapshot={x:frozen.slice(),...h.depths()};
    h.step(); h.draw(1); h.photo();
    assert.deepEqual({x:h.photo().slice(),...h.depths()},snapshot,'photo restores crossing depths/order and remains frozen');
    h.reset(seed,oval);
    assert.deepEqual(h.state().w,lanes,'replay resets steering lanes');
  }
  return {steps,frames};
}

const read = name => readFileSync(new URL('../../apps/derby/' + name, import.meta.url), 'utf8');
const counts = runLaneTests(read('derby_scene.js'), read('derby_watch.js'), assert);
console.log('PASS: lane/sway/depth-order/photo contracts', counts);
