// pocket.audio.capture on the board. Every line is one claim; README.md reads
// them. Three recordings in one session, because whether a second one works is
// the open question and only an app can answer it.
(function(){
  function say(t){ console.log('MICCHK '+t); }
  function no(w){ return function(e){ say(w+' '+e.code); }; }
  var c = pocket.capabilities.get('audio.capture');
  var rec=null;
  // clip is counted, not inferred from peak: a peak of 32767 could be one
  // sample or a thousand, and how often it hits the rail is the question.
  function read(tag){
    var n=0,total=0,peak=0,clip=0;
    function step(){
      if(n>=40){ say(tag+' n='+total+' peak='+peak+' clip='+clip); rec.close(); rec=null; return; }
      n++;
      return rec.read(512).then(function(a){
        total+=a.length;
        for(var i=0;i<a.length;i++){
          var v=a[i]<0?-a[i]:a[i];
          if(v>peak) peak=v;
          if(v>=32767) clip++;
        }
        return step();
      }, no(tag));
    }
    return step();
  }
  function take(tag){
    return pocket.audio.capture.open({sampleRate:c.limits.sampleRate,channels:1})
      .then(function(r){ rec=r; say(tag+' open'); return read(tag); }, no(tag));
  }
  pocket.app.start({
    start:function(){
      say('CAP '+c.supported+'/'+c.available+' '+c.reason);
      say('LIM '+c.limits.sampleRate+'/'+c.limits.channels+'/'+c.limits.maxReadFrames);
      return pocket.audio.capture.open({sampleRate:16000})
        .then(function(){ say('RATE16 opened'); }, no('RATE16'))
        .then(function(){ return take('REC1'); })
        .then(function(){ return take('REC2'); })
        .then(function(){ return take('REC3'); })
        .then(function(){ say('DONE'); });
    },
    stop:function(r){ if(rec) rec.close(); say('STOP '+r); }
  });
})();
