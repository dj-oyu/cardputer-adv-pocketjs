// A small borrowed-key sampler; MEGADEMO need not load the material engine.
'use strict';
globalThis.CharacterMotion = (() => {
  function fail(message) { throw new TypeError('CharacterMotion: ' + message); }
  function finite(value) { return typeof value==='number' && value-value===0; }
  function track(keys, options) {
    const stride = options.stride, column = options.timeColumn === undefined ? 0 : options.timeColumn;
    const interpolation = options.interpolation || 'linear', unit = options.timeUnit;
    if ((!Array.isArray(keys) && !(keys instanceof Float32Array)) ||
        !finite(stride) || stride%1 || stride < 2 || !finite(column) || column%1 || column < 0 || column >= stride ||
        keys.length < stride || keys.length % stride ||
        (interpolation !== 'linear' && interpolation !== 'smooth' && interpolation !== 'step') ||
        (unit !== 'ms' && unit !== 'frame')) fail('invalid keys/options');
    for (let i = 0; i < keys.length; ++i) {
      if (!finite(keys[i]) || (i >= stride && !finite(keys[i] - keys[i - stride]))) fail('invalid key');
      if (i % stride === column && i >= stride && keys[i] <= keys[i - stride]) fail('times must increase');
    }
    // Keys are borrowed, immutable by contract, for this track's lifetime.
    return {keys, stride, column, interpolation, timeUnit: unit};
  }
  function sample(value, time, out) {
    if (!finite(time) || !out || out.length !== value.stride) fail('invalid sample');
    interpolate(value.keys,value.stride,value.column,value.interpolation==='smooth'?1:value.interpolation==='step'?2:0,time,out);
  }
  function interpolate(keys,stride,column,mode,time,out) {
    const end = keys.length - stride;
    let a=0,b=0,u=0;
    if(time>=keys[end+column])a=b=end;
    else if(time>keys[column]){
      while(keys[a+stride+column]<=time)a+=stride;
      b=a+stride;
      u=(time-keys[a+column])/(keys[b+column]-keys[a+column]);
      if(mode===1)u=u*u*(3-2*u);
      else if(mode===2)u=0;
    }
    for(let i=0;i<stride;++i)out[i]=a===b?keys[a+i]:keys[a+i]+(keys[b+i]-keys[a+i])*u;
  }
  function performance(def) {
    if (!def || !def.track || !Array.isArray(def.channels) || def.channels.length !== def.track.stride)
      fail('performance channels must match track');
    const names=def.channels.slice(), output={__proto__:null}, ops=[], keyTrack=def.track;
    function channel(name) {
      if(typeof name!=='string'||!name)fail('invalid channel');
      let at=0;
      while(at<names.length&&names[at]!==name)++at;
      if(at===names.length)names.push(name);
      return at;
    }
    for(let i=0;i<names.length;++i)if(channel(names[i])!==i)fail('duplicate channel');
    const waves=def.waves||[], gates=def.gates||[], reserved=[];
    for(let i=0;i<waves.length;++i){
      const w=waves[i];
      if(!w||!finite(w.amplitude)||!finite(w.rate)||
        (w.phase!==undefined&&!finite(w.phase))||
        (w.mode!=='add'&&w.mode!=='multiply'))fail('invalid wave');
      const at=channel(w.channel);
      if(w.mode==='multiply'&&at>=keyTrack.stride)fail('invalid wave');
      reserved[at]=true;
      ops.push(at,w.amplitude,w.rate,w.phase||0,w.mode==='multiply'?1:0);
    }
    for(let i=0;i<gates.length;++i){
      const g=gates[i];
      if(!g||!finite(g.from)||!finite(g.to)||g.to<=g.from||!finite(g.value))fail('invalid gate');
      const at=channel(g.channel);
      if(at<keyTrack.stride||reserved[at])fail('invalid gate');
      ops.push(at,g.value,g.from,g.to,2);
    }
    const values=new Array(names.length), keys=keyTrack.keys,stride=keyTrack.stride,column=keyTrack.column,
      mode=keyTrack.interpolation==='smooth'?1:keyTrack.interpolation==='step'?2:0, timeUnit=keyTrack.timeUnit;
    for(let i=0;i<names.length;++i)output[names[i]]=0;
    function evaluate(time) {
      if(typeof time!=='number'||time-time!==0)throw new TypeError('CharacterMotion: invalid time');
      for(let i=0;i<values.length;++i)values[i]=0;
      // Keep playback self-contained so setup-only helpers can be collected.
      const end=keys.length-stride;
      let a=0,b=0,u=0;
      if(time>=keys[end+column])a=b=end;
      else if(time>keys[column]){
        while(keys[a+stride+column]<=time)a+=stride;
        b=a+stride;
        u=(time-keys[a+column])/(keys[b+column]-keys[a+column]);
        if(mode===1)u=u*u*(3-2*u);else if(mode===2)u=0;
      }
      for(let i=0;i<stride;++i)values[i]=a===b?keys[a+i]:keys[a+i]+(keys[b+i]-keys[a+i])*u;
      for(let i=0;i<ops.length;i+=5){
        const at=ops[i],kind=ops[i+4];
        if(kind===2){if(time>=ops[i+2]&&time<ops[i+3])values[at]+=ops[i+1];}
        else {
          const v=ops[i+1]*Math.sin(time*ops[i+2]+ops[i+3]);
          if(kind)values[at]*=1+v;else values[at]+=v;
        }
      }
      for(let i=0;i<values.length;++i)if(values[i]-values[i]!==0)throw new TypeError('CharacterMotion: performance overflow');
      for(let i=0;i<values.length;++i)output[names[i]]=values[i];
      return output;
    }
    return {evaluate,timeUnit};
  }
  return {track, sample, performance};
})();
if (typeof module !== 'undefined') module.exports = globalThis.CharacterMotion;
