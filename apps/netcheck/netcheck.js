// pocket.net self-check; README.md reads the output. URL=null keeps it offline.
(function(){
  var N = pocket.net, T = pocket.time, URL = 'https://example.com/';
  function say(t){ console.log('NETCHK '+t); }
  function no(t){ return function(e){ say(t+' '+e.code+' '+e.outcome); }; }
  function yes(t){ return function(){ say(t+' NOT REJECTED'); }; }
  function cap(n){
    var c = pocket.capabilities.get(n);
    say('CAP '+n+' '+c.supported+'/'+c.available+' '+c.reason+' '+
        JSON.stringify(c.limits));
  }
  function ask(w,url,method,len){
    return N.http.request({wifi:w,url:url,method:method,
                           body:len?new Uint8Array(len):undefined});
  }
  // Each is refused before a socket opens.
  var LAN = 'http://192.168.0.1/';
  var BAD = [['PLAIN','http://example.com/','GET',0],
             ['SCHEME','ftp://192.168.0.1/','GET',0],
             ['METHOD',LAN,'PATCH',0],['BIGBODY',LAN,'POST',5000],
             ['CREDS','https://a:b@example.com/','GET',0]];
  function pull(r,n){
    return r.read(1024).then(function(c){
      if (c === null) { say('BODY '+n+' eof'); return r.close(); }
      n += c.length;
      if (n < 4096) return pull(r,n);
      say('BODY '+n+' stopped'); r.close();
    });
  }
  function leased(w){
    var s = w.status(), chain = Promise.resolve();
    say('LEASE '+s.state+' '+s.address);
    w.onChange(function(c){ say('CHANGE '+c.state); });
    BAD.forEach(function(b){
      chain = chain.then(function(){
        return ask(w,b[1],b[2],b[3]).then(yes(b[0]),no(b[0]));
      });
    });
    return chain.then(function(){
      if (!URL) return null;
      return ask(w,URL,'GET',0).then(function(r){
        say('GET status='+r.status+' type='+r.headers['content-type']+
            ' trunc='+r.headersTruncated);
        return pull(r,0);
      }, no('GET'));
    }).then(function(){
      w.close();
      say('ACLOSE '+w.status().state);
      return ask(w,LAN,'GET',0).then(yes('CLOSED'),no('CLOSED'));
    });
  }
  function scan(tag,opt){
    var t = T.now();
    return N.wifi.scan(opt).then(function(r){
      say(tag+' n='+r.networks.length+' trunc='+r.truncated+
          ' ms='+Math.round(T.now()-t)+
          (r.networks.length?' top='+r.networks[0].rssiDbm:''));
    }, function(e){ say(tag+' '+e.code+' after='+Math.round(T.now()-t)+'ms'); });
  }
  pocket.app.start({
    start: function(){
      cap('net.wifi'); cap('net.http');
      return ask(undefined,URL,'GET',0).then(yes('NOLEASE'),no('NOLEASE'))
        .then(function(){
          return N.wifi.acquire({ profileId: 'other' }).then(yes('PROFILE'),no('PROFILE'));
        })
        .then(function(){ return scan('SCAN'); })
        .then(function(){
          var s = pocket.cancel.source(), p = scan('CANCEL',{cancel:s.token});
          s.cancel();
          return p;
        })
        .then(function(){
          return N.wifi.acquire({profileId:'default'}).then(leased,no('ACQUIRE'));
        })
        .then(function(){ say('DONE'); });
    },
    stop: function(r){ say('STOP '+r); }
  });
})();
