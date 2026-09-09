(function () {
  var P={w:1,h:2,pos:24,top:25,left:28,bg:64,fg:96};
  ui.setProp(1,P.bg,0x102030ff);
  function text(y,t) {
    var n=ui.createNode(1);
    ui.setProp(n,P.pos,1);ui.setProp(n,P.left,12);ui.setProp(n,P.top,y);
    ui.setProp(n,P.w,216);ui.setProp(n,P.h,14);ui.setProp(n,P.fg,0xffffffff);
    ui.setText(n,t);ui.insertBefore(1,n,0);return n;
  }
  text(10,'MP3 / EXISTING AUDIO PATH');
  var label=text(36,'OPENING'), status=text(60,'');
  text(104,'ESC QUITS');
  var player=null,sub=null,cycle=0,phase=0,wait=0,paused=false;
  var last=0,worst=0,frames=0,sum=0;
  var sources=['test-tone.mp3','test-48k.mp3','test-24k.mp3'];
  function fail(e) {
    console.log('MP3_FAIL '+e.code+' '+e.message);
    ui.setText(label,'FAILED: '+e.code);phase=9;
  }
  function open() {
    paused=false;phase=0;worst=0;frames=0;sum=0;
    pocket.audio.player.open({source:'assets:/'+sources[cycle%3]}).then(function(p){
      player=p;
      console.log('MP3_OPEN '+JSON.stringify(p.info()));
      sub=p.onState(function(e){
        if(e.state==='error') {fail(e.error);return;}
        if(e.state==='ended') {
          console.log('MP3_DONE cycle='+cycle+' src='+sources[cycle%3]+' '+JSON.stringify(p.status())+
            ' duration='+p.info().durationMs+' meanFrameMs='+Math.round(sum/frames)+
            ' worstFrameMs='+worst);
          sub.close();sub=null;p.close();player=null;
          cycle++;phase=3;wait=0;
        }
      });
      ui.setText(label,'CYCLE '+cycle+' / '+(cycle%2?'PAUSE+RESUME':'PLAY'));
      p.play().then(function(){phase=1;},fail);
    },fail);
  }
  globalThis.frame=function(){
    var now=Date.now(),delta=last?now-last:0;last=now;
    if(phase===3) {if(++wait>5)open();return;}
    if(phase===2) {
      if(++wait>5) {phase=0;player.play().then(function(){phase=1;},fail);}
      return;
    }
    if(phase!==1||!player)return;
    frames++;sum+=delta;if(delta>worst)worst=delta;
    var s=player.status();
    if(frames%15===0)ui.setText(status,s.positionMs+' ms / gaps '+s.underruns);
    if(cycle%2&&!paused&&s.positionMs>=700) {
      paused=true;phase=0;
      player.pause().then(function(){phase=2;wait=0;},fail);
    }
  };
  open();
})();
