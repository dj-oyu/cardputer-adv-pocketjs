// Source-based pose fixture; does not impose an experimental draw order.
export function makePanFixture(watch, view, sceneSource, panSource, assert) {
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
  return make();
}
