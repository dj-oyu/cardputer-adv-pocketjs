// Feature-tests the surfaces added by the workspace, audio and BLE work.
// README.md reads the output; every line is one claim, checked on the board.
(function(){
  function say(t){ console.log('APICHK '+t); }
  function cap(n){
    var c = pocket.capabilities.get(n);
    say('CAP '+n+' '+c.supported+'/'+c.available+' '+c.reason);
  }
  // Section 2's whole point: a namespace exists even when unsupported, so an
  // app that skipped the feature test gets UNSUPPORTED and not a TypeError.
  function reach(path,fn){
    try { var r = fn(); say('REACH '+path+' '+(r&&r.then?'promise':typeof r)); }
    catch(e){ say('REACH '+path+' threw '+(e&&e.name)+' '+(e&&e.code)); }
  }
  pocket.app.start({
    start: function(){
      cap('ble.central'); cap('audio.playback'); cap('workspace');
      say('TYPEOF ble='+typeof pocket.ble+' ws='+typeof pocket.workspace+
          ' player='+(pocket.audio&&typeof pocket.audio.player));
      reach('ble.scan', function(){ return pocket.ble.scan({}); });
      var i = pocket.app.info();
      say('APP '+i.id+' '+i.runtime+' works='+(i.access&&i.access.works));
      var l = pocket.app.launchContext();
      say('LAUNCH state='+(l&&l.returnState)+' result='+(l&&l.result));
      return pocket.workspace.create({title:'APICHK',text:'print(1)\n'})
        .then(function(ref){
          say('WS created');
          return pocket.workspace.read(ref).then(function(w){
            say('WS read rev='+w.revision+' title='+w.title+' n='+w.text.length);
          });
        }, function(e){ say('WS create '+e.code+' '+e.outcome); })
        .then(function(){ say('DONE'); });
    },
    stop: function(r){ say('STOP '+r); }
  });
})();
