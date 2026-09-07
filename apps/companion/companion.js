(function(){
  var pet=pocket.pet,page=0,pages=['CODEX','CLAUDE','ALARM','TIMER'],prev=0,last=0;
  var selected=pet.select(),wake=pet.clock().wakeMinute,edit=wake<0?420:wake,minutes=5;
  function node(k,x,y,w,h,c,t){var n=ui.createNode(k);ui.setProp(n,24,1);ui.setProp(n,28,x);ui.setProp(n,25,y);ui.setProp(n,1,w);ui.setProp(n,2,h);if(k!==2)ui.setProp(n,k===1?96:64,c);if(t)ui.replaceText(n,t);ui.insertBefore(1,n,0);return n;}
  var seen={};function text(n,t){if(seen[n]!==t){seen[n]=t;ui.replaceText(n,t);}}
  // pet.select/wake/alarm throw a PocketError when NVS refuses the write or the
  // four timer slots are taken. Report it instead of letting frame() end the app.
  var err='';function fail(e){err=e&&e.code==='LIMIT_EXCEEDED'?'TIMERS BUSY':'SAVE FAILED';}
  ui.setProp(1,64,0x091323ff);
  var head=node(1,10,8,225,10,0x70e0d1ff,'PET COMPANION');
  node(0,8,27,90,76,0x203446ff);
  var line=[];
  for(var i=0;i<4;i++)line.push(node(1,106,32+i*18,130,12,i===0?0xfbe8b8ff:0xbdcedbff,''));
  var foot=node(1,9,113,228,10,0xfbe8b8ff,''),hint=node(1,9,125,228,8,0x89a4bfff,'');
  pet.place(selected,20,32);
  function hh(m){return ('0'+Math.floor(m/60)).slice(-2)+':'+('0'+m%60).slice(-2);}
  function left(r,now){if(r===null||now===null)return '--';var s=Math.max(0,r-now);return Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';}
  function draw(){
    var c=pet.clock();text(head,'< '+pages[page]+' >     PET '+(selected+1)+'/12');
    if(page<2){var u=pet.usage(page);text(line[0],u.stale?'PC OFFLINE / STALE':'PC CONNECTED');
      for(var w=0;w<2;w++){var v=u.windows[w];text(line[w+1],(w?'LONG ':'SHORT ')+(v.usedPercent===null?'--':Math.round(v.usedPercent)+'%')+' '+left(v.resetsAt,c.utc));}
      text(line[3],'SNACKS '+pet.rewards(selected));text(foot,'UP/DOWN PET  L/R PAGE');text(hint,'RESET TIME: CHECK USAGE ON PC');
    }else if(page===2){text(line[0],c.minute===null?'CLOCK NOT SYNCED':'NOW '+hh(c.minute));text(line[1],'WAKE '+hh(edit));
      text(line[2],c.wakeMinute<0?'OFF':'ON '+hh(c.wakeMinute));text(line[3],c.minute===null?'CONNECT PC / WI-FI':'DAILY / LOCAL TIME');
      text(foot,'UP/DOWN 5 MIN  ENTER SET/OFF');text(hint,'L/R PAGE  ESC HOME');
    }else{var r=pet.timer('companion.timer');text(line[0],r===null?'TIMER READY':'COUNTING DOWN');text(line[1],r===null?minutes+' MINUTES':Math.ceil(r)+' SECONDS');
      text(line[2],'WORKS FROM HOME');text(line[3],'SNOOZE: 5 MIN');text(foot,'UP/DOWN MIN  ENTER START/STOP');text(hint,'L/R PAGE  ESC HOME');}
    if(err)text(foot,err);
  }
  globalThis.frame=function(buttons){var now=pet.now(),edge=buttons&~prev;prev=buttons;if(edge)err='';
    if(edge&0x80)page=(page+3)%4;if(edge&0x20)page=(page+1)%4;
    if(edge&0x50){var d=edge&0x10?1:-1;
      if(page<2){selected=(selected+d+12)%12;try{pet.select(selected);}catch(e){fail(e);}pet.place(selected,20,32);}
      else if(page===2)edit=(edit+d*5+1440)%1440;
      else minutes=Math.max(1,Math.min(120,minutes+d));
    }
    if(edge&0x4000){try{if(page===2){var c=pet.clock();if(c.wakeMinute===edit)pet.wake(-1,0);else pet.wake(Math.floor(edit/60),edit%60);}
      else if(page===3)pet.alarm('companion.timer',pet.timer('companion.timer')===null?minutes*60:0,'TIMER FINISHED');}catch(e){fail(e);}}
    if(edge||now-last>=1000){last=now;draw();}
  };draw();console.log('COMPANION_READY');
})();
