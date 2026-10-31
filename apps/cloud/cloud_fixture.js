// Optional finite probe: one state machine, shared production motion state.
'use strict';
(()=>{
  let S=null,resetNative,readNative,cursor=0,pass=0,pause=0,pending=false;
  let reference=null,phases=null;
  globalThis.AyatsuriCloudFixtureDynamic={attach(sample,s,reset,read){S=s;resetNative=reset;readNative=read;}};
  function reset(on){resetNative(S);S.fixture=on;S.t=null;S.l=0;S.external=false;}
  function snapshot(compare){
    if(!reference){
      const r=new Float32Array(30),p={phases:new Uint32Array(3),increments:new Uint32Array(3),value:0};
      reference=r;phases=p;
    }
    S.d.snapshot(phases);
    for(let i=0;i<30;i++){
      const v=i<24?S.q[i]:((i&1)?phases.phases[(i-24)>>1]>>>16:phases.phases[(i-24)>>1]&65535);
      if(compare){if(!Object.is(v,reference[i]))throw new Error('CloudFixture: state mismatch');}
      else reference[i]=v;
    }
  }
  const control=AyatsuriCloudProbe.attachControl(function(access){
    const c={active:false,begin(){
      access('lock');try{
        S=null;reference=phases=null;cursor=pass=pause=0;pending=true;c.active=true;
      }finally{access('unlock');}
    },abort(reason){
      access('lock');try{if(S)reset(false);S=null;reference=phases=null;c.active=false;access('discard');}finally{access('unlock');}
      console.log('CLOUD_FIXTURE RETURN reason='+reason);
    }};
    return {control:c,sample(tick){
      if(!c.active)return tick;
      if(pending){if(!S)throw new Error('CloudFixture: actor required');reset(true);pending=false;
        console.log('CLOUD_FIXTURE START seed=42 stepHz=60 displayHz=30');}
      if(tick!==cursor)throw new Error('CloudFixture: cursor changed');return cursor;
    },submitted(tick){
      if(!c.active)return tick+1;
      if(tick!==cursor)throw new Error('CloudFixture: submitted cursor changed');
      if(pause){snapshot(true);if(--pause)return cursor;}
      else{
        if(cursor===30||cursor===60){
          S.g[0]=cursor===30?.18:.35;S.g[1]=cursor===30?-.09:-.22;
          if(cursor===30)S.a[0].retarget(S.g);else S.a[0].impulse(S.g);
          readNative(S,0);S.external=true;
          console.log('CLOUD_FIXTURE EVENT '+(cursor===30?'RETARGET':'IMPULSE')+' pass='+pass+' step='+(cursor*2));
        }
        if(cursor===75&&!pass){snapshot(false);pause=15;console.log('CLOUD_FIXTURE EVENT PAUSE pass=0 step=150');return cursor;}
        if(cursor===140){
          if(!pass){snapshot(false);console.log('CLOUD_FIXTURE EVENT REPLAY step=280');reset(true);pass=1;cursor=0;return 0;}
          snapshot(true);const bits=new Uint32Array(reference.buffer);let hash=2166136261;
          for(let i=0;i<30;i++)hash=Math.imul(hash^bits[i],16777619);
          console.log('CLOUD_FIXTURE COMPLETE result=PASS digest='+(hash>>>0).toString(16)+' pause=15 seed=42');
          reset(false);c.active=false;reference=phases=null;console.log('CLOUD_FIXTURE RETURN reason=complete');return 0;
        }
      }
      return ++cursor;
    },reset(){if(c.active){if(S)reset(false);c.active=false;reference=phases=null;console.log('CLOUD_FIXTURE RETURN reason=cancel');}}};
  });
  AyatsuriCloudProbe.enter=control.begin;
  AyatsuriCloudProbe.failure=function(){if(!control.active)return false;control.abort('failure');return true;};
})();

