// One TLS handshake per host, in sequence, so the threshold set from a single
// server can be judged against chains and key sizes it never saw. README.md
// reads the output; pocket.net's TLSCOST line reports the memory each one cost.
(function(){
  var N = pocket.net, T = pocket.time;
  function say(t){ console.log('TLSH '+t); }
  var HOSTS = [
    'https://example.com/',            // the one the thresholds were set from
    'https://www.google.com/',
    'https://github.com/',
    'https://letsencrypt.org/',
    'https://rsa4096.badssl.com/',     // 4096-bit key, the memory stress
    'https://sha512.badssl.com/'
  ];
  function one(w, i){
    if (i >= HOSTS.length) return null;
    var url = HOSTS[i], began = T.now();
    return N.http.request({wifi:w, url:url, method:'GET'}).then(function(r){
      say(url+' status='+r.status+' ms='+Math.round(T.now()-began));
      return r.read(512).then(function(){ return r.close(); });
    }, function(e){
      say(url+' '+e.code+' ms='+Math.round(T.now()-began)+
          (e.message?' :: '+e.message:''));
    }).then(function(){
      // A pause between handshakes so each one starts from a settled heap
      // rather than from whatever the last one had not finished releasing.
      return T.sleep(600).then(function(){ return one(w, i+1); });
    });
  }
  pocket.app.start({
    start: function(){
      function acquire(left){
        return N.wifi.acquire({profileId:'default'}).then(function(w){
          say('LEASE '+w.status().address);
          return one(w,0).then(function(){ w.close(); say('DONE'); });
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
