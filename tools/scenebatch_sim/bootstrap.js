'use strict';
pocket.app.load('scene');
(()=>{
 const P=pocket.kasane.shiki,r=P.resource();
 pocket.kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource:r,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});});
 const M=pocket.ayatsuri.modelOpen(new Uint8Array(__MODEL_BYTES__).buffer);
 const S=AyatsuriNative.scene({model:M,camera:{center:[120,67]},light:{direction:[0,0,1]}});
 const D=new Uint16Array([1,6,2,2,4,1]),G=new Float32Array(36),IB=new Float32Array(36);
 const V=new Float32Array(60),J=new Uint16Array(40),W=new Uint16Array(40);
 function ident(a){a.fill(0);for(let i=0;i<a.length;i+=12)a[i]=a[i+5]=a[i+10]=1;}
 ident(G);ident(IB);G[15]=15;IB[15]=-15;
 for(let v=0;v<6;v++){V[v*6]=v%3*30-30;V[v*6+1]=v<3?-15:15;V[v*6+5]=1;J[v*4+1]=1;W[v*4+1]=v%3===0?0:v%3===1?21845:65535;W[v*4]=65535-W[v*4+1];}
 const back=[-35,-20,35,-20,35,20,-35,20];
 for(let v=0;v<4;v++){V[(v+6)*6]=back[v*2];V[(v+6)*6+1]=back[v*2+1];V[(v+6)*6+5]=1;W[(v+6)*4]=65535;}
 let stopped=false;
 globalThis.skinAssetFrame=ms=>{if(stopped)throw Error('stopped');const pose=ms>=1000;G[19]=pose?10:0;G[27]=pose?-8:0;S.evaluateSkins(ms,D,G,IB,V,J,W);P.beginMeshFrame(17,2048,200000,1);S.render();P.commit();};
 globalThis.skinAssetStats=out=>S.stats(out);
 globalThis.skinAssetStop=()=>{if(stopped)return;stopped=true;S.dispose();M.dispose();V.fill(NaN);G.fill(NaN);W.fill(0);};
})();
