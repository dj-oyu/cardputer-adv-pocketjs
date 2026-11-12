'use strict';
let S,meshHandle;
function newScene(){
  S=AyatsuriNative.scene({model:M,camera:{position:[0,0,400],target:[0,0,0],zoom:1,center:[120,67]},
    light:{direction:[0,0,1],color:0xffffff}});meshHandle=S.handle(1);
}
function sceneSetup(){setup();A.dispose();B.dispose();H.dispose();D.dispose();newScene();}
function sceneEvaluate(frame){poseAt(frame);S.evaluateSkin(frame*100,1,G,IB,V,J,W);}
function sceneDraw(){P.beginMeshFrame(17,2048,200000,1);S.render();P.commit();}
function sceneInvalid(){
  const bad=W[0];W[0]=0;rejects(()=>S.evaluateSkin(3000,1,G,IB,V,J,W));W[0]=bad;
  rejects(()=>S.render());sceneEvaluate(30);
  Object.defineProperty(stats,'nodes',{configurable:true,set(){
    rejects(()=>sceneEvaluate(30));rejects(()=>S.dispose());throw Error('setter');}});
  rejects(()=>S.stats(stats));delete stats.nodes;sceneEvaluate(30);
  // Like ordinary evaluate, a rejected nonfinite time does not start evaluation.
  rejects(()=>S.evaluateSkin(NaN,1,G,IB,V,J,W));sceneDraw();
}
function sceneHidden(value){meshHandle.visible(value);sceneEvaluate(30);sceneDraw();}
function sceneCold(){S.evaluate(0);sceneDraw();}
function sceneFresh(){S.dispose();newScene();}
function sceneInspectFailed(){rejects(()=>S.render());S.stats(stats);check(stats.skinWorkBytes===0);}
function scenePendingDispose(){
  sceneEvaluate(30);sceneDraw();M.dispose();S.dispose();S.dispose();
  rejects(()=>meshHandle.visible(true));rejects(()=>sceneEvaluate(30));
  M.stats(modelStats);check(modelStats.instances===0&&modelStats.released);
  G.fill(NaN);IB.fill(NaN);V.fill(NaN);J.fill(65535);W.fill(0);
  S=M=A=B=H=D=meshHandle=null;fixture=holes=sided=null;
}
