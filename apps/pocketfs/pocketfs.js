// pocket.fs self-check. Claims: README.md.
(async function(){
var p=pocket,L=console.log,ps=0,fl=0,fs=p.fs,C=n=>p.capabilities.get(n),i,t,c;
var W=(n,x,o)=>fs.writeText(n,x,o),R=(n,m)=>fs.readText(n,{maxBytes:m});
var O=(n,m)=>fs.open(n,{mode:m}),T=n=>fs.stat(n),rm=n=>fs.remove(n).catch(()=>0);
var ok=(n,v)=>{if(v)ps++;else{fl++;L('FS_FAIL '+n);}};
var E=async(n,f,w)=>{var e='ok';try{await f();}catch(x){e=x.code||''+x;}
ok(n+' '+e,e===w);};
var IA='INVALID_ARGUMENT',LE='LIMIT_EXCEEDED',HJ='assets:/hello.js';
var A=C('fs.volume.app'),v=fs.volumes(),F=v[0].features,V=v[0],D=C('fs.volume.sd');
L('FS_CAP '+JSON.stringify(A.limits)+' '+JSON.stringify(V));
ok('cp',A.supported&&A.available&&C('fs.volume.assets').supported&&
D.supported&&!D.available);
ok('vol',v.length===3&&V.id==='app'&&V.caseSensitive&&F.atomicReplace&&
!F.crashSafeReplace&&v[1].readOnly&&v[1].quotaBytes===null);
var S=v[2];
L('FS_SD '+JSON.stringify(S)+' '+JSON.stringify(D.limits));
ok('sdv',S.id==='sd'&&S.state==='absent'&&!S.caseSensitive&&
S.features.write&&!S.features.crashSafeReplace&&S.quotaBytes===null);
for(t of [['app:/a/../b',IA],['app:/a\\b',IA],['sd:/a','PERMISSION_DENIED'],
['assets:/%2e%2e/x','NOT_FOUND']]) await E(t[0],()=>T(t[0]),t[1]);
// Every door into an ungranted card answers the same, so none of them says
// whether a card is in the slot. requestFolder is the only way in, and it
// refuses a volume with no folder to grant without drawing anything.
for(t of [()=>fs.list('sd:/'),()=>O('sd:/a','read'),()=>fs.mkdir('sd:/a'),
()=>fs.remove('sd:/a'),()=>W('sd:/a','x'),()=>R('sd:/a',8),
()=>fs.copy(HJ,'sd:/a'),()=>fs.space('sd:/')])
await E('sd'+(i=(i|0)+1),t,'PERMISSION_DENIED');
await E('rq',()=>fs.requestFolder('app'),IA);
var junk='n/旅 k c e g n'.split(' ').map(x=>'app:/'+x);
for(i of junk) await rm(i);
await fs.mkdir('app:/n');
var s=await W('app:/n/旅','海を見た。',{mode:'create'});
ok('ent',s.kind==='file'&&s.sizeBytes===15&&!!s.revision);
await E('2nd',()=>W(s.path,'x',{mode:'create'}),'ALREADY_EXISTS');
await E('old',()=>W(s.path,'x',{mode:'replace',ifRevision:'9.9'}),'CONFLICT');
var s2=await W(s.path,'海と波を見た。',{mode:'replace',ifRevision:s.revision});
ok('rev',s2.revision!==s.revision);
ok('txt',(await R(s.path,4096))==='海と波を見た。');
await E('dur',()=>W('app:/d','x',{durability:'crash-safe'}),'UNSUPPORTED');
await E('max',()=>R(s.path,4),LE);
var f=await O('app:/e','create');
ok('nil',(await f.commit()).sizeBytes===0&&(await R('app:/e',8))==='');
var h=await O(HJ,'read'),got=0;
while(c=await h.read(1024)) got+=c.length;
ok('str',got===(await T(HJ)).sizeBytes&&got>1024);
ok('eof',(await h.read(1024))===null&&h.tell()===got&&(await h.seek(0))===0&&
(await h.read(4)).length===4&&h.tell()===4);
h.close();h.close();
await E('cls',()=>h.read(4),'CLOSED');
var r=await fs.list('app:/',{limit:1});
ok('pg',r.entries.length===1&&!!r.nextCursor);
ok('cur',(await fs.list('app:/',{cursor:r.nextCursor})).entries.length>=1);
await E('ne',()=>fs.remove('app:/n'),'NOT_EMPTY');
ok('copy',(await fs.copy(HJ,'app:/c')).sizeBytes===got);
await fs.rename('app:/c','app:/k');
ok('ren',(await T('app:/k')).sizeBytes===got);
var a=await O('app:/g','create');
await a.write(new Uint8Array([65,66]));
await a.commit();
var b=await O('app:/g','append'),tl=b.tell();
await E('bsy',()=>fs.remove('app:/g'),'BUSY');
await b.write(new Uint8Array([67]));
await b.flush();b.close();
ok('app',tl===2&&(await R('app:/g',8))==='ABC');
var q=0;
try{for(;;q++) await W('app:/q'+q,'x'.repeat(4e3));}catch(x){c=x.code;}
L('FS_QUOTA '+q+c);
ok('quo',c==='QUOTA_EXCEEDED'||c===LE);
while(q--) await rm('app:/q'+q);
L('FS_RESULT '+ps+' fail='+fl);
})();
globalThis.frame=function(){};
