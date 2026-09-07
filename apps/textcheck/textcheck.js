// pocket.input.text, one claim per line. README.md says what each should say.
(function(){
  function L(t){ console.log('TXT '+t); }
  var RECT={x:8,y:56,width:224,height:20};
  var closed=null;                       // a TextSession kept past its close

  function refuse(name,opts){
    try { var s=pocket.input.text.open(opts); s.close();
          L('REFUSE '+name+' -> ACCEPTED(wrong)'); }
    catch(e){ L('REFUSE '+name+' -> '+e.code); }
  }

  function phaseA(){
    var c=pocket.capabilities.get('input.text');
    L('CAP '+c.supported+'/'+c.available+' '+c.reason);
    var k=Object.keys(c.limits).map(function(n){return n+'='+c.limits[n];});
    L('LIMITS '+k.join(' '));

    refuse('no-rect',{});
    refuse('ime-maybe',{rect:RECT,ime:'maybe'});
    refuse('long-initial',{rect:RECT,maxBytes:4,initial:'123456789'});
    refuse('onEdit-not-fn',{rect:RECT,onEdit:1});
    refuse('offscreen',{rect:{x:200,y:0,width:100,height:20}});

    var a=pocket.input.text.open({
      rect:RECT, initial:'ねこ', maxBytes:32, multiline:false, ime:'on',
      onEdit:function(e){ L('EDIT '+e.text.length+' ['+e.text+']'); },
      onSubmit:function(e){ L('SUBMIT ['+e.text+']'); phaseB(); },
      onCancel:function(){ L('CANCEL-A'); phaseB(); }
    });
    L('OPEN-A getText=['+a.getText()+']');
    try { pocket.input.text.open({rect:RECT}); L('SECOND -> ACCEPTED(wrong)'); }
    catch(e){ L('SECOND -> '+e.code); }
    closed=a;
    L('TYPE into A, then ENTER to submit (or ESC to cancel)');
  }

  function phaseB(){
    // The session that just ended, still held by the app.
    try { L('STALE getText -> ['+closed.getText()+'] (wrong)'); }
    catch(e){ L('STALE getText -> '+e.code); }
    closed.close();                      // idempotent, must not throw
    L('STALE close -> ok');

    var b=pocket.input.text.open({
      rect:RECT, maxBytes:24, multiline:true, ime:'off',
      onEdit:function(e){ L('EDIT-B '+e.text.length); },
      onSubmit:function(){ L('SUBMIT-B (wrong: multiline Enter is a newline)'); },
      onCancel:function(){ L('CANCEL-B'); phaseC(); }
    });
    L('OPEN-B multiline: ENTER must NOT submit; ESC ends it');
    b.getText();
  }

  function phaseC(){
    // The stale-commit provocation: the first edit closes the field and opens
    // another at the same place. Anything the host had computed for the first
    // belongs to a focus generation that no longer exists.
    var swapped=false;
    function reopen(){
      pocket.input.text.open({
        rect:RECT, maxBytes:24, ime:'on',
        onEdit:function(e){ L('EDIT-D ['+e.text+']'); },
        onSubmit:function(e){ L('SUBMIT-D ['+e.text+'] -- done'); },
        onCancel:function(){ L('CANCEL-D -- done'); }
      });
      L('OPEN-D (the replacement). Type, then ENTER.');
    }
    var c=pocket.input.text.open({
      rect:RECT, maxBytes:24, ime:'on',
      onEdit:function(e){
        L('EDIT-C ['+e.text+']');
        if(swapped) return;
        swapped=true;
        c.close();
        reopen();
      },
      onCancel:function(){ L('CANCEL-C'); }
    });
    L('OPEN-C: the FIRST character swaps the session underneath the callback');
  }

  pocket.app.start({
    start: function(){ phaseA(); return null; },
    stop:  function(r){ L('STOP '+r); }
  });
})();
