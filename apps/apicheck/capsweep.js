// Every capability name the spec defines, asked of the running firmware, so the
// gap between docs/common-api.md and this build is a measurement rather than a
// reading of the source. README.md carries the last result.
(function(){
  var NAMES = ['ui.basic','input.text','input.action','storage.kv','workspace',
    'fs.volume.app','fs.volume.assets','fs.volume.sd','sensors.imu','power','time','log',
    'audio.tone','audio.cue','audio.capture','audio.playback',
    'io.gpio','io.i2c','io.spi','io.uart','io.ir',
    'net.wifi','net.http','ble.central','ble.peripheral','bridge.pc','app'];
  function say(t){ console.log('CAP '+t); }
  pocket.app.start({
    start: function(){
      var missing=0, unsupported=0, ok=0;
      NAMES.forEach(function(n){
        var c = pocket.capabilities.get(n);
        var mark = c.supported ? (c.available?'ok  ':'idle') : 'NO  ';
        if(!c.supported) unsupported++; else if(c.available) ok++; else missing++;
        say(mark+' '+n+' '+c.supported+'/'+c.available+' '+c.reason);
      });
      say('TOTAL supported+available='+ok+' supported+idle='+missing+
          ' unsupported='+unsupported+' of '+NAMES.length);
      return null;
    },
    stop: function(r){ say('STOP '+r); }
  });
})();
