// Real panning submission order, with renderer calls recorded rather than rasterized.
// Run: node tools/games/test_derby_pan_order.mjs
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

function runPanTests(watch, view, sceneSource, panSource, assert) {
  const cut = (src, start, end) => {
    const a=src.indexOf(start), b=src.indexOf(end,a);
    assert.ok(a>=0 && b>a, 'source markers: '+start);
    return src.slice(a,b);
  };
  const make = () => new Function(
    cut(watch,'// DERBY WATCH:','globalThis.derby') +
    `
    const VS=[840,34,20,6,16], FN=[3,4,6];
    let cx=0, slow=0, scene='race', disp=0, t=0, tier=1;
    let F,rs,lz,zo,photoX,ro,ph=Array(8).fill(0),vr=null,von=11;
    const KN=[[0,0,0,0,12,10],[0,0,0,0,12,10],[0,0,0,0,12,10],[0,0,0,0,0,1]];
    const live={}, pocket={derby:{view(){},ser(){}}}, dr=()=>{};
    let drawn=[],feedDrawn=[],inFeed=false,feedCalls=0,projectionCalls=0;
    const rin=(l,X,S,Y,a)=>(inFeed?feedDrawn:drawn).push({l,X,S,Y,a});
    ` +
    cut(view,'const OB =','// ---- Cameras') +
    cut(view,'const CAMS =','// Draws one frame') +
    cut(view,'function course(','// A runner:') +
    view.slice(view.indexOf('function feed(')) +
    cut(sceneSource,'function at(u)','// Finish:') +
    panSource +
    `
    const originalPj=pj;
    pj=P=>{++projectionCalls;return originalPj(P);};
    const originalFeed=feed;
    feed=(xs,z)=>{++feedCalls;inFeed=true;originalFeed(xs,z);inFeed=false;};
    return {
      reset(seed,oval) {
        F=field(seed);F.o=oval;rs=race(F);rs.l=DL.slice();F.k=z8(0);
        for(let i=0;i<64;++i)F.k[i>>3]+=F.m[i&7]>F.m[i>>3];
        CRS=oval?OC:SC;lz=DL.slice();zo=[7,6,5,4,3,2,1,0];
      },
      advance(n) {for(let i=0;i<n;++i){step(F,rs,DT);at(1);}},
      frame(m,u,vision,loadTier=1) {
        tier=loadTier;const xs=at(u);ro=order(rs);shot(m,xs,0,1);
        const side=zo.slice(), depths=lz.slice();
        const projected=xs.map((x,l)=>pj(pose(x-12.5*U,lz[l])));
        vr=vision?[10,20,130,50]:null;vq=[0,.1,120,.1];
        drawn=[];feedDrawn=[];feedCalls=0;
        const before=projectionCalls;
        pan(xs);
        return {xs,depths,side,projected,drawn,feedDrawn,feedCalls,
          projections:projectionCalls-before,after:zo.slice(),t:rs.t};
      },
      capture(u) {
        photoX=photo(u);
        return {x:photoX.slice(),z:photoX.z.slice(),o:photoX.o.slice()};
      },
      restore() {
        const x=photo();
        return {x:x.slice(),z:lz.slice(),o:zo.slice()};
      }
    };
  `)();
  const h=make();let frames=0, visible=0,feeds=0;
  const near=(a,b,msg)=>assert.ok(Math.abs(a-b)<1e-10,msg);
  const check=d=>{
    const expected=d.projected.map((p,l)=>({p,l}))
      .filter(({p})=>p[1]>.02 && p[0]/p[1]>-160 && p[0]/p[1]<400)
      .sort((a,b)=>b.p[1]-a.p[1] || d.side.indexOf(a.l)-d.side.indexOf(b.l));
    assert.deepEqual(d.drawn.map(p=>p.l),expected.map(p=>p.l),'pan must draw far to near in camera depth');
    for(let i=0;i<d.drawn.length;++i) {
      const p=d.drawn[i], q=d.projected[p.l];
      near(p.X,q[0]/q[1],'projected X unchanged');
      near(p.S,1/q[1]/6,'projected scale unchanged');
      near(p.Y,28+6/q[1],'projected ground unchanged');
    }
    assert.equal(d.projections,14,'eight runner and six pole projections, without comparator re-projection');
    if(d.feedCalls) {
      assert.deepEqual(d.feedDrawn.map(p=>p.l),d.side.filter(l=>d.feedDrawn.some(p=>p.l===l)),
        'side-view feed retains lateral-depth order');
      if(d.feedDrawn.length)++feeds;
    }
    visible+=d.drawn.length;++frames;
  };
  h.reset(2,1);h.advance(391);
  const fixture=h.frame(8,1,true,2);
  assert.ok(fixture.side.indexOf(7)<fixture.side.indexOf(5),'fixture lateral order is 7 before 5');
  assert.ok(fixture.projected[7][1]<fixture.projected[5][1],'fixture camera depth reverses 7 and 5');
  assert.ok(fixture.drawn.some(p=>p.l===7)&&fixture.drawn.some(p=>p.l===5),'both overlapping horses are visible');
  near(fixture.projected[7][0]/fixture.projected[7][1],110.47439656440548,'seed 2 reproduction X');
  check(fixture);
  for(const oval of [0,1])for(const seed of [2,7,19]) {
    h.reset(seed,oval);
    let previous=0;
    for(const step of [1,320,390,391,500,780,820,1000,1150]) {
      h.advance(step-previous);previous=step;
      for(const tier of [0,1,2])for(const camera of [7,8,9])for(const u of [0,.5,1]) {
        check(h.frame(camera,u,true,tier));
        check(h.frame(camera,u,false,tier));
      }
    }
    const captured=h.capture(.5);
    // pan mutates its working order, but may not mutate the stored photo order.
    check(h.frame(8,1,true,2));
    assert.deepEqual(h.restore(),captured,'photo snapshot survives pan sorting');
    h.reset(seed,oval);h.advance(391);
    const first=h.frame(8,1,true,2);
    h.reset(seed,oval);h.advance(391);
    assert.deepEqual(h.frame(8,1,true,2),first,'same seed replays draw order');
  }
  assert.ok(feeds>0,'real side course/feed submitted runners');
  return {frames,visible,feeds,fixture:fixture.drawn.map(p=>p.l)};
}

const read = name => readFileSync(new URL('../../apps/derby/' + name, import.meta.url), 'utf8');
console.log('PASS: camera-depth order, feed order, cached projection and photo contracts',
  runPanTests(read('derby_watch.js'), read('derby_view.js'), read('derby_scene.js'), read('derby_pan.js'), assert));
