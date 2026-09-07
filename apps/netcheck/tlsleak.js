// The same host, many times, to tell a leak from pools reaching a steady state.
// A leak loses a roughly constant amount per request; pools lose a lot at first
// and then stop. pocket.net's TLSCOST line reports free before each handshake,
// which is the settled figure after the previous request closed.
(function(){
  var N = pocket.net, T = pocket.time;
  function say(t){ console.log('LEAK '+t); }
  var URL = 'https://example.com/', ROUNDS = 12;
  function round(w, i){
    if (i >= ROUNDS) return null;
    return N.http.request({wifi:w, url:URL, method:'GET'}).then(function(r){
      return r.read(512).then(function(){
        return r.close();
      }).then(function(){ say(i+' status='+r.status); });
    }, function(e){
      say(i+' '+e.code+(e.message?' :: '+e.message:''));
    }).then(function(){
      return T.sleep(500).then(function(){ return round(w, i+1); });
    });
  }
  pocket.app.start({
    start: function(){
      function acquire(left){
        return N.wifi.acquire({profileId:'default'}).then(function(w){
          say('LEASE');
          // Twelve back to back, then a long wait, then three more. A leak
          // does not come back; sockets in TIME_WAIT do, after 2*MSL.
          return round(w,0).then(function(){ w.close(); say('DONE'); });
        }, function(e){
          if (e.code==='BUSY' && left>0)
            return T.sleep(400).then(function(){ return acquire(left-1); });
          say('ACQUIRE '+e.code);
        });
      }
      return acquire(8);
    },
    stop: function(r){ say('STOP '+r); }
  });
})();
