// A small borrowed-key sampler; MEGADEMO need not load the material engine.
'use strict';
const CharacterMotion = (() => {
  function fail(message) { throw new TypeError('CharacterMotion: ' + message); }
  function track(keys, options) {
    const stride = options.stride, column = options.timeColumn === undefined ? 0 : options.timeColumn;
    const interpolation = options.interpolation || 'linear', unit = options.timeUnit;
    if ((!Array.isArray(keys) && !(keys instanceof Float32Array)) ||
        !Number.isInteger(stride) || stride < 2 || !Number.isInteger(column) || column < 0 || column >= stride ||
        keys.length < stride || keys.length % stride ||
        (interpolation !== 'linear' && interpolation !== 'smooth' && interpolation !== 'step') ||
        (unit !== 'ms' && unit !== 'frame')) fail('invalid keys/options');
    for (let i = 0; i < keys.length; ++i) {
      if (!Number.isFinite(keys[i])) fail('non-finite key');
      if (i >= stride && !Number.isFinite(keys[i] - keys[i - stride])) fail('key interval overflow');
      if (i % stride === column && i >= stride && keys[i] <= keys[i - stride]) fail('times must increase');
    }
    // Keys are borrowed, immutable by contract, for this track's lifetime.
    return {keys, stride, column, interpolation, timeUnit: unit};
  }
  function sample(value, time, out) {
    if (!Number.isFinite(time) || !out || out.length !== value.stride) fail('invalid sample');
    const keys = value.keys, stride = value.stride, column = value.column, end = keys.length - stride;
    let a = 0;
    if (time <= keys[column]) {
      for (let i = 0; i < stride; ++i) out[i] = keys[i];
      return;
    }
    if (time >= keys[end + column]) {
      for (let i = 0; i < stride; ++i) out[i] = keys[end + i];
      return;
    }
    while (a < end && keys[a + stride + column] <= time) a += stride;
    let u = (time - keys[a + column]) / (keys[a + stride + column] - keys[a + column]);
    if (value.interpolation === 'smooth') u = u * u * (3 - 2 * u);
    else if (value.interpolation === 'step') u = 0;
    for (let i = 0; i < stride; ++i) out[i] = keys[a + i] + (keys[a + stride + i] - keys[a + i]) * u;
  }
  function performance(def) {
    if (!def || !def.track || !Array.isArray(def.channels) || def.channels.length !== def.track.stride)
      fail('performance channels must match track');
    const channels = def.channels.slice(), output = Object.create(null), work = new Array(channels.length), waves = [], gates = [];
    for (let i = 0; i < channels.length; ++i) {
      if (typeof channels[i] !== 'string' || !channels[i] || Object.hasOwn(output, channels[i])) fail('unique channel names');
      output[channels[i]] = 0;
    }
    for (const w of def.waves || []) {
      if (!w || typeof w.channel !== 'string' || !w.channel ||
          !Number.isFinite(w.amplitude) || !Number.isFinite(w.rate) ||
          (w.phase !== undefined && !Number.isFinite(w.phase)) ||
          (w.mode !== 'add' && w.mode !== 'multiply') ||
          (w.mode === 'multiply' && !channels.includes(w.channel))) fail('invalid wave');
      if (!Object.hasOwn(output,w.channel)) output[w.channel] = 0;
      waves.push({channel:w.channel,amplitude:w.amplitude,rate:w.rate,phase:w.phase || 0,mode:w.mode});
    }
    for (const g of def.gates || []) {
      if (!g || typeof g.channel !== 'string' || !g.channel ||
          !Number.isFinite(g.from) || !Number.isFinite(g.to) || !(g.to > g.from) ||
          !Number.isFinite(g.value) || channels.includes(g.channel) || waves.some(w=>w.channel===g.channel)) fail('invalid gate');
      if (!Object.hasOwn(output,g.channel)) output[g.channel] = 0;
      gates.push({channel:g.channel,from:g.from,to:g.to,value:g.value});
    }
    const resetNames=Object.keys(output), keyTrack=def.track, values=new Array(resetNames.length);
    for(let i=0;i<waves.length;++i)waves[i].index=resetNames.indexOf(waves[i].channel);
    for(let i=0;i<gates.length;++i)gates[i].index=resetNames.indexOf(gates[i].channel);
    function evaluate(time) {
      sample(keyTrack,time,work);
      for(let i=0;i<values.length;++i)values[i]=0;
      for(let i=0;i<channels.length;++i)values[i]=work[i];
      for(let i=0;i<waves.length;++i){
        const w=waves[i], v=w.amplitude*Math.sin(time*w.rate+w.phase);
        if(w.mode==='add')values[w.index]+=v;else values[w.index]*=1+v;
      }
      for(let i=0;i<gates.length;++i){const g=gates[i];if(time>=g.from&&time<g.to)values[g.index]+=g.value;}
      for(let i=0;i<values.length;++i)if(!Number.isFinite(values[i]))fail('performance overflow');
      for(let i=0;i<values.length;++i)output[resetNames[i]]=values[i];
      return output;
    }
    return {evaluate,timeUnit:keyTrack.timeUnit};
  }
  return {track, sample, performance};
})();
if (typeof module !== 'undefined') module.exports = CharacterMotion;
