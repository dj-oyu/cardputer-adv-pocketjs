(function () {
  var names=['TABBY','CALICO','BLACK','NEON CAT','PINK','IVORY','MINT','NEON AXO','GRAY','YELLOW','BLUE','NEON BIRD'];
  var kinds=['CAT','AXOLOTL','COCKATIEL'], actions=['FEED','PLAY','SLEEP','NAME','PETS'];
  var s={v:1,selected:0,pets:[]}, ready=false, lf=false, mode=1, choice=0, action=0;
  var last=__petNow(), saved=last, anim=0, prev=0, note='LOADING', until=0, pos=0, draft='';
  var chars=' ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';
  var art=pocket.kasane.resource('pets'), V=pocket.kasane.mount('pet');
  function render(){
    if(!ready){V.set({foot:lf?note:''});return;}
    var now=__petNow(), id=mode===1?choice:s.selected, p=s.pets[id];
    var by=29+(p.sleep?2:Math.floor(now/1000)%2);
    var sk=until>now;
    V.set({ready:true,resource:art,variant:id,frame:p.sleep?1:mode===0&&sk?2:now%5000<200?1:p.food<25?4:0,
      petY:by,background:id%4===3?0x080c20ff:0x0b1727ff,title:mode===1?'CHOOSE: '+names[id]:mode===2?'NAME: '+draft:p.name,
      index:(id+1)+'/12',species:kinds[Math.floor(id/4)],
      status:mode===1?'ENTER TO KEEP':p.sleep?'SLEEPING':p.food<25?'HUNGRY':p.energy<25?'TIRED':'HAPPY',
      food:'FOOD '+Math.round(p.food),joy:'JOY '+Math.round(p.fun),energy:'ENERGY '+Math.round(p.energy),
      bar0:Math.max(1,Math.round(p.food*1.06)),bar1:Math.max(1,Math.round(p.fun*1.06)),
      bar2:Math.max(1,Math.round(p.energy*1.06)),
      foot:mode===1?'LEFT/RIGHT COLOR  UP/DOWN ANIMAL':mode===2?'EDIT LETTER '+(pos+1)+' / 8':sk?note:'< '+actions[action]+' >',
      hint:mode===1?'ENTER SELECT   ESC HOME':mode===2?'ARROWS EDIT  ENTER SAVE':'L/R ACTION  ENTER DO  ESC SAVE/HOME',
      bubble:sk,note:sk?note:'',reveal:sk?Math.min(note.length,Math.max(0,Math.floor((2400-(until-now))/70))):0});
  }
  render();
  function pet(){return s.pets[s.selected];}
  function message(t){note=t;until=__petNow()+2400;}
  function save(){
    if(!ready)return;
    saved=__petNow();
    pocket.storage.set('pet.v1',s).then(function(){},function(){message('SAVE FAILED');render();});
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
    s.selected=pocket.pet.select();choice=s.selected;ready=true;last=__petNow();
    render();
    console.log('PET_READY '+s.selected);
  },function(){lf=true;note='LOAD FAILED: ESC TO RETRY';render();});
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
      if(edge&0x4000){try{pocket.pet.select(choice);}catch(e){message('SAVE FAILED');}s.selected=choice;mode=0;save();console.log('PET_SELECTED '+choice);}
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
        else if(action===4){choice=s.selected;mode=1;}
        save();console.log('PET_ACTION '+actions[action]);
      }
    }
    if(now-saved>=60000)save();
    if(now-anim>=100||edge){anim=now;render();}
  };
})();
