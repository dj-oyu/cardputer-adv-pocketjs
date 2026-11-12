'use strict';
let BM,BS;
function bridgeSetup(){
 BM=pocket.ayatsuri.modelOpen(fixture);
 BS=AyatsuriNative.scene({model:BM,camera:{position:[200,0,400],target:[0,0,0],zoom:1,center:[120,67]},light:{direction:[0,0,1],color:0xffffff}});
 globalThis.bridgeIB=asset.IB;globalThis.bridgeV=asset.V;globalThis.bridgeJ=asset.J;globalThis.bridgeW=asset.W;
}
function bridgeEvaluate(code){const pose=code>>1,back=code&1;globalThis.bridgeG=asset.samples[pose].G;BS.camera({position:[back?-200:200,0,back?-400:400]});BS.evaluateSkin(pose*1000,0,bridgeG,bridgeIB,bridgeV,bridgeJ,bridgeW);}
function bridgeDraw(){P.beginMeshFrame(17,2048,200000,1);BS.render();P.commit();}
function bridgeCold(){BS.evaluate(0);}
function bridgePoisonInputs(){
 asset.V.fill(NaN);asset.IB.fill(NaN);asset.J.fill(65535);asset.W.fill(0);for(const s of asset.samples)s.G.fill(NaN);
}
function bridgePoisonDispose(){
 BS.dispose();BM.dispose();const stats={};BM.stats(stats);if(stats.instances!==0||!stats.released)throw Error('unreleased model');
 BS=BM=null;globalThis.asset=null;globalThis.fixture=null;globalThis.bridgeG=globalThis.bridgeIB=globalThis.bridgeV=globalThis.bridgeJ=globalThis.bridgeW=null;
}
