(function () {
  var names=['TABBY','CALICO','BLACK','NEON CAT','PINK','IVORY','MINT','NEON AXO','GRAY','YELLOW','BLUE','NEON BIRD'];
  var kinds=['CAT','AXOLOTL','COCKATIEL'], actions=['FEED','PLAY','SLEEP','NAME','PETS'];
  var s={v:1,selected:0,pets:[]}, ready=false, mode=1, choice=0, action=0;
  var last=__petNow(), saved=last, anim=0, prev=0, note='LOADING', until=0, pos=0, draft='';
  var chars=' ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';
  function node(k,x,y,w,h,c,t) {
    var n=ui.createNode(k);
    ui.setProp(n,24,1); ui.setProp(n,28,x); ui.setProp(n,25,y);
    ui.setProp(n,1,w); ui.setProp(n,2,h); if(k!==2)ui.setProp(n,k===1?96:64,c);
    if(t) ui.replaceText(n,t);
    ui.insertBefore(1,n,0); return n;
  }
  var shown={};
  function text(n,t) {if(shown[n]!==t){shown[n]=t;ui.replaceText(n,t);}}
  ui.setProp(1,64,0x0b1727ff);
  var title=node(1,9,7,175,10,0xf5eedcff,'POCKET PET');
  var badge=node(1,190,7,46,10,0x67dfc7ff,'');
  node(0,7,25,105,84,0x1c3043ff);
  node(0,15,94,89,2,0x476275ff);
  var species=node(1,15,99,96,10,0xb8c7d6ff,'');
  var status=node(1,122,29,113,10,0x67dfc7ff,'');
  var labels=[], bars=[], colors=[0xf5bb69ff,0xf08bbcff,0x67dfc7ff];
  // Bars have no track: 15 nodes is the ceiling. See README.md.
  for(var i=0;i<3;i++) {
    labels.push(node(1,122,45+i*18,110,9,0xc9d5dfff,''));
    bars.push(node(0,122,56+i*18,85,3,colors[i]));
  }
  var footer=node(1,8,114,230,10,0xf5bb69ff,'');
  var help=node(1,8,126,230,8,0x91a6baff,'');
  function pet(){return s.pets[s.selected];}
  function message(t){note=t;until=__petNow()+2400;pocket.pet.say(t);}
  function save(){
    if(!ready)return;
    saved=__petNow();
    pocket.storage.set('pet.v1',s).then(function(){},function(){message('SAVE FAILED');});
  }
  function image(){
    var id=mode===1?choice:s.selected;
    pocket.pet.place(id,27,29);
    ui.setProp(1,64,id%4===3?0x080c20ff:0x0b1727ff);
    text(species,kinds[Math.floor(id/4)]);
  }
  function draw(){
    var id=mode===1?choice:s.selected, p=s.pets[id];
    text(title,mode===1?'CHOOSE: '+names[id]:mode===2?'NAME: '+draft:p.name);
    text(badge,(id+1)+'/12');
    text(status,mode===1?'ENTER TO KEEP':p.sleep?'SLEEPING':p.food<25?'HUNGRY':p.energy<25?'TIRED':'HAPPY');
    var values=[p.food,p.fun,p.energy], tags=['FOOD','JOY','ENERGY'];
    for(var i=0;i<3;i++){
      text(labels[i],tags[i]+' '+Math.round(values[i]));
      ui.setProp(bars[i],1,Math.max(1,Math.round(values[i]*1.06)));
    }
    text(footer,mode===1?'LEFT/RIGHT COLOR  UP/DOWN ANIMAL':mode===2?'EDIT LETTER '+(pos+1)+' / 8':until>__petNow()?note:'< '+actions[action]+' >');
    text(help,mode===1?'ENTER SELECT   ESC HOME':mode===2?'ARROWS EDIT  ENTER SAVE':'L/R ACTION  ENTER DO  ESC SAVE/HOME');
  }
  pocket.storage.get('pet.v1').then(function(r){
    var v=r&&r.value;
    if(v&&v.v===1&&Array.isArray(v.pets)&&v.pets.length===12){
      s.selected=Number.isInteger(v.selected)&&v.selected>=0&&v.selected<12?v.selected:0;
      s.pets=v.pets;mode=0;
    }
    for(var i=0;i<12;i++){
      var p=s.pets[i];
      if(!p||typeof p!=='object'||Array.isArray(p))p={};
      p.name=typeof p.name==='string'&&p.name.length?p.name.slice(0,12):names[i];
      ['food','fun','energy'].forEach(function(k){p[k]=typeof p[k]==='number'&&isFinite(p[k])?Math.max(0,Math.min(100,p[k])):80;});
      p.sleep=p.sleep===true;s.pets[i]=p;
    }
    s.selected=pocket.pet.select();choice=s.selected;ready=true;last=__petNow();image();draw();
    console.log('PET_READY '+s.selected);
  },function(){note='LOAD FAILED: ESC TO RETRY';text(footer,note);});
  globalThis.frame=function(buttons){
    var now=__petNow(), edge=buttons&~prev;prev=buttons;
    if(!ready)return;
    var p=pet(), dt=Math.max(0,Math.min(5,(now-last)/1000));last=now;
    var gift=pocket.pet.rewards(s.selected),was=typeof p.gift==='number'?p.gift:0;
    if(gift>was){p.food=Math.min(100,p.food+gift-was);p.gift=gift;message('TOKEN SNACK!');}
    p.food=Math.max(0,p.food-dt/90);p.fun=Math.max(0,p.fun-dt/120);
    p.energy=Math.max(0,Math.min(100,p.energy+dt*(p.sleep?0.8:-1/150)));
    if(edge&0x2000){save();return;}
    if(mode===1){
      if(edge&0x80)choice=Math.floor(choice/4)*4+(choice+3)%4;
      if(edge&0x20)choice=Math.floor(choice/4)*4+(choice+1)%4;
      if(edge&0x10)choice=(choice+8)%12;
      if(edge&0x40)choice=(choice+4)%12;
      if(edge&0xf0)image();
      if(edge&0x4000){try{pocket.pet.select(choice);}catch(e){message('SAVE FAILED');}s.selected=choice;mode=0;image();save();console.log('PET_SELECTED '+choice);}
    }else if(mode===2){
      if(edge&0x80)pos=(pos+7)%8;
      if(edge&0x20)pos=(pos+1)%8;
      if(edge&0x50){var c=chars.indexOf(draft[pos]);c=(c+(edge&0x10?1:chars.length-1))%chars.length;draft=draft.slice(0,pos)+chars[c]+draft.slice(pos+1);}
      if(edge&0x4000){p.name=draft.trim()||names[s.selected];mode=0;save();}
    }else{
      if(edge&0x80)action=(action+4)%5;
      if(edge&0x20)action=(action+1)%5;
      if(edge&0xf0)until=0;
      if(edge&0x4000){
        if(action<2&&p.sleep)message('WAKE ME FIRST');
        else if(action===0){p.food=Math.min(100,p.food+20);message('YUM!');}
        else if(action===1){if(p.energy<10)message('NEED A NAP');else{p.fun=Math.min(100,p.fun+20);p.energy-=8;message('LET US PLAY!');}}
        else if(action===2){p.sleep=!p.sleep;message(p.sleep?'GOOD NIGHT':'GOOD MORNING');}
        else if(action===3){draft=(p.name.toUpperCase().replace(/[^ A-Z0-9]/g,'')+'        ').slice(0,8);pos=0;mode=2;}
        else if(action===4){choice=s.selected;mode=1;image();}
        save();console.log('PET_ACTION '+actions[action]);
      }
    }
    if(now-saved>=60000)save();
    if(now-anim>=100||edge){anim=now;pocket.pet.place(mode===1?choice:s.selected,27,29+(p.sleep?2:Math.floor(now/500)%2),p.sleep?1:now%5000<150?5:p.food<25?4:until>now?2:0);draw();}
  };
})();
