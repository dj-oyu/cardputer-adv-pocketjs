// Eight requests, then give the lease back and take it again, then eight more.
// If the largest free block recovers across that boundary, a lease cycle is a
// defragmentation the app can perform itself; if it does not, the fragmentation
// outlives the radio and only a connection that is held can avoid it.
(function(){
  var N = pocket.net, T = pocket.time;
  function say(t){ console.log('CYC '+t); }
  var URL = 'https://example.com/';
  function round(w, i, n){
    if (i >= n) return null;
    return N.http.request({wifi:w, url:URL, method:'GET'}).then(function(r){
      return r.read(512).then(function(){ return r.close(); })
              .then(function(){ say(i+' status='+r.status); });
    }, function(e){ say(i+' '+e.code+(e.message?' :: '+e.message:'')); })
     .then(function(){ return T.sleep(400).then(function(){ return round(w,i+1,n); }); });
  }
  function acquire(left){
    return N.wifi.acquire({profileId:'default'}).then(null, function(e){
      if (e.code==='BUSY' && left>0)
        return T.sleep(500).then(function(){ return acquire(left-1); });
      throw e;
    });
  }
  pocket.app.start({
    start: function(){
      var first;
      return acquire(10).then(function(w){
        first = w;
        say('LEASE 1');
        return round(w,0,8);
      }).then(function(){
        first.close();
        say('LEASE CLOSED, settling');
        return T.sleep(3000);
      }).then(function(){
        return acquire(10);
      }).then(function(w){
        say('LEASE 2');
        return round(w,8,16).then(function(){ w.close(); say('DONE'); });
      }, function(e){ say('REACQUIRE '+e.code); });
    },
    stop: function(r){ say('STOP '+r); }
  });
})();
