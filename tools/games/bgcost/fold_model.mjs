// The crowd plan's dots in a float32 model of the VM: how far each way of folding
// the phase start (docs/kasane/derby-background-cost.md) moves them, and how many
// sin arguments stay past 201. node tools/games/bgcost/fold_model.mjs
const f=Math.fround, PI=Math.PI, KN=[[3,2,3],[4,3,4],[5,4,6]], NAMES=['LIGHT','MID','HEAVY'];
const M={
  orig:(j,k,n)=>j*k*2.39996,
  int16:(j,k,n)=>((j*k*25032)&0xffff)*(2*PI/65536),         // (1) 2.39996 rad = 25032.3 units
  mod:(j,k,n)=>{const r=j*k*2.39996%(2*PI);return r<0?r+2*PI:r}, // (2) 0..2pi
  centre:(j,k,n)=>j*k*2.39996%(2*PI)-2*PI*Math.round(n*k*.191), // shipped
};
const cams=[['FIELD',58/40],['WIDE',100/40],['VISION',130/40],['FINISH',170/40]];
for (const [ti,k] of KN.entries()) for (const [cn,q] of cams) {
  const st={}; for (const m in M) st[m]={over:0,max:0,px:0,tot:0,maxd:0};
  for (let j=-5;j<90;j++) {
    const dx=12*q, a=-10+(j%7), n=Math.min(Math.floor((700-a)/dx),Math.ceil((250-a)/dx)+1);
    const run=p0=>{const xs=[];let r13=0;for(let row=0;row<k[1];row++){let r0=f(a),r6=f(f(p0)+r13);
      for(let b=0;b<n;b++)for(let d=0;d<k[2];d++){xs.push([f(f(f(Math.sin(r6))*1.5)+r0),Math.abs(r6)]);r0=f(r0+f(dx/k[2]));r6=f(r6+f(2.39996));}
      r13=f(r13+f(.9));}return xs;};
    const o=run(M.orig(j,k[2],n));
    for (const m in M){const w=run(M[m](j,k[2],n)),s=st[m];
      for(let i=0;i<w.length;i++){s.tot++;if(w[i][1]>201)s.over++;s.max=Math.max(s.max,w[i][1]);
        const d=Math.abs(o[i][0]-w[i][0]);s.maxd=Math.max(s.maxd,d);if(Math.round(o[i][0])!==Math.round(w[i][0]))s.px++;}}
  }
  console.log(NAMES[ti],cn,Object.entries(st).map(([m,s])=>`${m}: >201 ${(100*s.over/s.tot).toFixed(1)}% max ${s.max.toFixed(0)} px ${(100*s.px/s.tot).toFixed(3)}% d ${s.maxd.toFixed(4)}`).join(' | '));
}
