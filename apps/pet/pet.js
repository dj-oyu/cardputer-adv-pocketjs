(function () {
  var names=['TABBY','CALICO','BLACK','NEON CAT','PINK','IVORY','MINT','NEON AXO','GRAY','YELLOW','BLUE','NEON BIRD'];
  var kinds=['CAT','AXOLOTL','COCKATIEL'], actions=['FEED','PLAY','SLEEP','NAME','PETS'];
  var s={v:1,selected:0,pets:[]}, ready=false, lf=false, mode=1, choice=0, action=0;
  var last=__petNow(), saved=last, anim=0, prev=0, note='LOADING', until=0, pos=0, draft='';
  var chars=' ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';
  var barc=[0xf5bb69ff,0xf08bbcff,0x67dfc7ff];
  var V=pocket.kasane, PI=V.petImage();
  function upd(tx,r){
    var now=__petNow(), id=mode===1?choice:s.selected, p=s.pets[id];
    r.t.setText(tx,mode===1?'CHOOSE: '+names[id]:mode===2?'NAME: '+draft:p.name);
    r.ba.setText(tx,(id+1)+'/12');
    r.sn.setText(tx,kinds[Math.floor(id/4)]);
    tx.background(id%4===3?0x080c20ff:0x0b1727ff);
    r.st.setText(tx,mode===1?'ENTER TO KEEP':p.sleep?'SLEEPING':p.food<25?'HUNGRY':p.energy<25?'TIRED':'HAPPY');
    var v=[p.food,p.fun,p.energy], tg=['FOOD','JOY','ENERGY'];
    for(var i=0;i<3;i++){
      r.lb[i].setText(tx,tg[i]+' '+Math.round(v[i]));
      var w=Math.max(1,Math.round(v[i]*1.06));
      r.br[i].setRect(tx,[122,56+i*18,122+w,59+i*18]);
    }
    r.ft.setText(tx,mode===1?'LEFT/RIGHT COLOR  UP/DOWN ANIMAL':mode===2?'EDIT LETTER '+(pos+1)+' / 8':until>now?note:'< '+actions[action]+' >');
    r.hp.setText(tx,mode===1?'ENTER SELECT   ESC HOME':mode===2?'ARROWS EDIT  ENTER SAVE':'L/R ACTION  ENTER DO  ESC SAVE/HOME');
    var by=29+(p.sleep?2:Math.floor(now/1000)%2);
    r.pt.setRect(tx,[27,by,91,by+64]);
    r.pt.setImageFrame(tx,id,p.sleep?1:mode===0&&until>now?2:now%5000<200?1:p.food<25?4:0);
    // Bubble reuses note/until (message() sets both): sk mirrors the footer window.
    var sk=until>now;
    r.bg.setVisible(tx,sk);r.bt.setVisible(tx,sk);
    // KSN-MISSING(text.animate): no native char-reveal track; stepped from JS each patch
    if(sk)r.bt.setReveal(tx,Math.min(note.length,Math.floor((2400-(until-now))/70)));
  }
  var SC=V.createScene({
    build:function(tx){
      function T(bd,c,cap){return tx.text({bounds:bd,font:'caption',color:c,text:'',capacity:cap});}
      function R(bd,c){return tx.rect({bounds:bd,color:c});}
      tx.background(0x0b1727ff);
      var t=T([9,7,184,21],0xf5eedcff,24), ba=T([190,7,238,21],0x67dfc7ff,8);
      R([7,25,112,109],0x1c3043ff);R([15,94,104,96],0x476275ff);
      var sn=T([15,99,111,108],0xb8c7d6ff,12), st=T([122,29,238,43],0x67dfc7ff,16);
      var lb=[], br=[];
      for(var i=0;i<3;i++){
        lb.push(T([122,45+i*18,232,57+i*18],0xc9d5dfff,16));
        br.push(R([122,56+i*18,123,59+i*18],barc[i]));
      }
      var ft=T([8,114,238,124],0xf5bb69ff,40), hp=T([8,126,238,135],0x91a6baff,40);
      // pet/bubble join once ready: no placeholder pose during load.
      var out={t:t,ba:ba,sn:sn,st:st,lb:lb,br:br,ft:ft,hp:hp};
      if(ready){
        out.pt=tx.image({resource:PI,bounds:[27,29,91,93]});
        out.bg=R([96,24,236,45],0x080c21ff);
        out.bt=T([100,31,232,39],0x080c21ff,24);
        upd(tx,out); // topology just gained content refs: fill them now, not next patch
      }
      return out;
    },
    patch:function(tx,r){
      if(lf){r.ft.setText(tx,note);return;}
      if(!ready)return;
      upd(tx,r);
    }
  });
  SC.flush(0);
  function pet(){return s.pets[s.selected];}
  function message(t){note=t;until=__petNow()+2400;}
  function save(){
    if(!ready)return;
    saved=__petNow();
    pocket.storage.set('pet.v1',s).then(function(){},function(){message('SAVE FAILED');SC.invalidate();});
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
    SC.invalidate(true);SC.flush(0);
    console.log('PET_READY '+s.selected);
  },function(){lf=true;note='LOAD FAILED: ESC TO RETRY';SC.invalidate();SC.flush(0);});
  globalThis.frame=function(buttons){
    var now=__petNow(), edge=buttons&~prev;prev=buttons;
    if(!ready){SC.flush(0);return;}
    var p=pet(), dt=Math.max(0,Math.min(5,(now-last)/1000));last=now;
    var gift=pocket.pet.rewards(s.selected),was=typeof p.gift==='number'?p.gift:0;
    if(gift>was){p.food=Math.min(100,p.food+gift-was);p.gift=gift;message('TOKEN SNACK!');}
    p.food=Math.max(0,p.food-dt/90);p.fun=Math.max(0,p.fun-dt/120);
    p.energy=Math.max(0,Math.min(100,p.energy+dt*(p.sleep?0.8:-1/150)));
    if(edge&0x2000){save();SC.flush(0);return;}
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
    if(now-anim>=100||edge){anim=now;SC.invalidate();}
    SC.flush(0);
  };
})();
