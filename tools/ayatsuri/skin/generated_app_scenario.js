'use strict';
let sample;const sampleStats={};
function bridgeSetup(){
 const original=AyatsuriNative.scene;let failed=false,captured;
 AyatsuriNative.scene=options=>{captured=options.model;throw Error('startup injection');};
 try{startGeneratedSkinSample(fixture,asset);}catch(e){failed=true;}finally{AyatsuriNative.scene=original;}
 const stats={};captured.stats(stats);if(!failed||!stats.released||stats.instances!==0)throw Error('startup leak');
 sample=startGeneratedSkinSample(fixture,asset);
 globalThis.bridgeIB=asset.IB;globalThis.bridgeV=asset.V;globalThis.bridgeJ=asset.J;globalThis.bridgeW=asset.W;
}
function bridgeEvaluate(code){const pose=code>>1,back=!!(code&1);globalThis.bridgeG=asset.samples[pose].G;sample.evaluate(pose*1000,back);sample.stats(sampleStats);if(sampleStats.skinWorkBytes!==480)throw Error('skin workspace');}
function bridgeDraw(){sample.render();}
function bridgePoisonInputs(){asset.V.fill(NaN);asset.IB.fill(NaN);asset.J.fill(65535);asset.W.fill(0);for(const s of asset.samples)s.G.fill(NaN);}
function bridgePoisonDispose(){sample.stop();sample.stop();let rejected=false;try{sample.frame(0);}catch(e){rejected=true;}if(!rejected)throw Error('stopped frame');sample=null;globalThis.asset=globalThis.fixture=null;globalThis.bridgeG=globalThis.bridgeIB=globalThis.bridgeV=globalThis.bridgeJ=globalThis.bridgeW=null;}
