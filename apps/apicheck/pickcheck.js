// Drives kobako.workspace.pick() -- the host screen that is drawn over a live
// guest and takes the keys away from it. README.md reads the output.
(function(){
  function say(t){ console.log('PICK '+t); }
  kobako.app.start({
    start: function(){
      say('OPENING');
      return kobako.workspace.pick({kind:'source'}).then(function(ref){
        if (!ref) { say('CANCELLED'); return; }
        say('CHOSE');
        return kobako.workspace.read(ref).then(function(w){
          say('READ rev='+w.revision+' title='+w.title+' n='+w.text.length);
        });
      }, function(e){ say('REJECTED '+e.code+' '+e.outcome); })
      .then(function(){ say('DONE'); });
    },
    stop: function(r){ say('STOP '+r); }
  });
})();
