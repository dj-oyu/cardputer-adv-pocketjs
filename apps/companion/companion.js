(function(){
var art=pocket.kasane.resource('pets'),v=pocket.kasane.mount('companion'),p=pocket.pet,pg=['CODEX','CLAUDE','ALARM','TIMER'];
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
function render(){var c=cn(s);v.set({resource:art,variant:s.d,head:c.h,line0:c.l[0],line1:c.l[1],
line2:c.l[2],line3:c.l[3],foot:c.f,hint:c.i});}
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
render();
});
var last=0;
globalThis.frame=function(){var now=p.now();if(now-last>=1000){last=now;render();}};
render();console.log('COMPANION_READY');
})();
