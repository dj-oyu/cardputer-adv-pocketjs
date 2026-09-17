(function(){
var v=pocket.kasane,p=pocket.pet,pi=v.petImage(),pg=['CODEX','CLAUDE','ALARM','TIMER'];
var wk=p.clock().wakeMinute,s={g:0,d:p.select(),e:wk<0?420:wk,m:5,x:''};
function hh(n){return('0'+Math.floor(n/60)).slice(-2)+':'+('0'+n%60).slice(-2);}
function lf(r,n){if(r===null||n===null)return'--';var d=Math.max(0,r-n);return Math.floor(d/3600)+'h '+Math.floor(d%3600/60)+'m';}
// select/wake/alarm throw on NVS/timer-slot refusal; report, don't end frame().
function fl(e){s.x=e&&e.code==='LIMIT_EXCEEDED'?'TIMERS BUSY':'SAVE FAILED';}
function cn(t){
var c=p.clock(),h='< '+pg[t.g]+' >     PET '+(t.d+1)+'/12',l=['','','',''],f,i;
if(t.g<2){var u=p.usage(t.g);l[0]=u.stale?'PC OFFLINE / STALE':'PC CONNECTED';
for(var w=0;w<2;w++){var y=u.windows[w];l[w+1]=(w?'LONG ':'SHORT ')+(y.usedPercent===null?'--':Math.round(y.usedPercent)+'%')+' '+lf(y.resetsAt,c.utc);}
l[3]='SNACKS '+p.rewards(t.d);f='UP/DOWN PET  L/R PAGE';i='RESET TIME: CHECK USAGE ON PC';
}else if(t.g===2){l[0]=c.minute===null?'CLOCK NOT SYNCED':'NOW '+hh(c.minute);l[1]='WAKE '+hh(t.e);
l[2]=c.wakeMinute<0?'OFF':'ON '+hh(c.wakeMinute);l[3]=c.minute===null?'CONNECT PC / WI-FI':'DAILY / LOCAL TIME';
f='UP/DOWN 5 MIN  ENTER SET/OFF';i='L/R PAGE  ESC HOME';
}else{var r=p.timer('companion.timer');l[0]=r===null?'TIMER READY':'COUNTING DOWN';
l[1]=r===null?t.m+' MINUTES':Math.ceil(r)+' SECONDS';l[2]='WORKS FROM HOME';l[3]='SNOOZE: 5 MIN';
f='UP/DOWN MIN  ENTER START/STOP';i='L/R PAGE  ESC HOME';
}
return{h:h,l:l,f:t.x||f,i:i};
}
var K=[20,24,24,32];
var sc=v.createScene({
build:function(tx,t){
tx.background(0x091323ff);tx.rect({bounds:[8,27,98,103],color:0x203446ff});
var pr=tx.image({resource:pi,bounds:[20,32,84,96],variant:t.d,frame:0}),c=cn(t);
var h=tx.text({bounds:[10,8,235,18],text:c.h,capacity:24,font:'caption',color:0x70e0d1ff});
var cl=[0xfbe8b8ff,0xbdcedbff,0xbdcedbff,0xbdcedbff],l=[];
for(var i=0;i<4;i++)l.push(tx.text({bounds:[106,32+i*18,236,44+i*18],text:c.l[i],capacity:K[i],font:'caption',color:cl[i]}));
var f=tx.text({bounds:[9,113,237,123],text:c.f,capacity:32,font:'caption',color:0xfbe8b8ff});
var i2=tx.text({bounds:[9,125,237,133],text:c.i,capacity:32,font:'caption',color:0x89a4bfff});
return{p:pr,d:t.d,h:h,l:l,f:f,i:i2};
},
patch:function(tx,r,t){
if(r.d!==t.d){r.p.setImageFrame(tx,t.d,0);r.d=t.d;}
var c=cn(t);r.h.setText(tx,c.h);
for(var i=0;i<4;i++)r.l[i].setText(tx,c.l[i]);
r.f.setText(tx,c.f);r.i.setText(tx,c.i);
}
});
// press only: old edge=buttons&~prev, no up/down repeat.
pocket.input.onAction(function(e){
if(e.phase!=='press')return;
s.x='';
if(e.action==='left')s.g=(s.g+3)%4;
else if(e.action==='right')s.g=(s.g+1)%4;
else if(e.action==='up'||e.action==='down'){
var d=e.action==='up'?1:-1;
if(s.g<2){s.d=(s.d+d+12)%12;try{p.select(s.d);}catch(er){fl(er);}}
else if(s.g===2)s.e=(s.e+d*5+1440)%1440;
else s.m=Math.max(1,Math.min(120,s.m+d));
}else if(e.action==='accept'){
try{
if(s.g===2){var c=p.clock();if(c.wakeMinute===s.e)p.wake(-1,0);else p.wake(Math.floor(s.e/60),s.e%60);}
else if(s.g===3)p.alarm('companion.timer',p.timer('companion.timer')===null?s.m*60:0,'TIMER FINISHED');
}catch(er){fl(er);}
}
sc.invalidate();
});
var last=0;
globalThis.frame=function(){var now=p.now();if(now-last>=1000){last=now;sc.invalidate();}sc.flush(s);};
sc.flush(s);console.log('COMPANION_READY');
})();
