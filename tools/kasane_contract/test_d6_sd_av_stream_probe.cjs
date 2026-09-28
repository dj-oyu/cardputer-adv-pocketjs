const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = name => fs.readFileSync(`apps/kasane/${name}.js`, 'utf8');
const tick = () => new Promise(resolve => setImmediate(resolve));

async function stage(collision = false) {
  const writes = [], logs = [], events = [];
  const pocket = {
    kasane: {replace: () => {}},
    fs: {
      requestFolder: async () => 'sd:/',
      stat: async () => {
        if (collision) return {sizeBytes: 7};
        throw {code: 'NOT_FOUND'};
      },
      open: async (_, options) => {
        assert.equal(options.mode, 'create');
        events.push('open');
        return {
          write: async data => {writes.push(Buffer.from(data)); return data.length;},
          commit: async () => {events.push('commit');},
          close: () => {events.push('close');},
        };
      },
    },
    time: {sleep: tick},
  };
  const context = {pocket, console: {log: line => logs.push(line)}};
  vm.runInNewContext(source('video_sd_av_stage'), context);
  context.frame();
  for (let i = 0; i < 1500 && !events.includes('commit') &&
       !logs.some(line => line.includes('ERROR')); ++i) await tick();
  if (collision) {
    assert.deepEqual(events, []);
    assert(logs.some(line => line.includes('collision')));
    return null;
  }
  assert.deepEqual(events, ['open', 'commit']);
  assert(writes.every(data => data.length <= 524));
  const bytes = Buffer.concat(writes);
  assert.equal(writes.length, 301);
  assert.equal(bytes.length, 157216);
  assert.equal(bytes.subarray(0, 4).toString(), 'KSV1');
  return bytes;
}

async function overlay(bytes) {
  const events = [], logs = [];
  let onKey, now = 1, clock = -1;
  const pocket = {
    kasane: {
      video: {
        open: () => 1,
        streamStart: () => {events.push('streamStart');},
        streamState: () => 'end',
        streamPoll: value => {clock = value; return true;},
        streamStop: () => {events.push('streamStop'); return true;},
      },
      replace: () => {},
    },
    overlay: {onKey: callback => {onKey = callback;}},
    fs: {
      requestFolder: async () => 'sd:/',
      stat: async () => ({sizeBytes: bytes.length}),
      open: () => {throw Error('overlay opened a JS file');},
      remove: () => {throw Error('overlay removed staged file');},
    },
    audio: {player: {open: async () => {
      events.push('playerOpen');
      return {
        info: () => ({codec: 'mp3', durationMs: null}),
        play: async () => {events.push('play');},
        status: () => ({state: 'playing', positionMs: 5000, underruns: 0}),
        close: () => {events.push('playerClose');},
      };
    }}},
    time: {now: () => now},
  };
  const context = {pocket, console: {log: line => logs.push(line)}};
  vm.runInNewContext(source('overlay_video_sd_stream_probe'), context);
  onKey({action: 'accept'});
  for (let i = 0; i < 100 && !events.includes('play'); ++i) await tick();
  assert.deepEqual(events, ['streamStart', 'playerOpen', 'play']);
  now = 11501; context.frame();
  assert.equal(clock, 5000000);
  for (let i = 0; i < 100 && !logs.some(line => line.includes('DONE')); ++i)
    await tick();
  assert.deepEqual(events.slice(-2), ['playerClose', 'streamStop']);
  assert(logs.some(line => line.includes('FILE_RETAINED')));
  assert(logs.some(line => line.includes('DONE reason=end')));
}

async function cleanup(bytes) {
  const logs = [];
  let at = 0, removed = 0, closed = 0;
  const pocket = {
    kasane: {replace: () => {}},
    fs: {
      requestFolder: async () => 'sd:/',
      stat: async () => ({sizeBytes: bytes.length}),
      open: async () => ({
        read: async count => {
          if (at === bytes.length) return null;
          const chunk = bytes.subarray(at, at + Math.min(count, 257));
          at += chunk.length;
          return chunk;
        },
        close: () => {++closed;},
      }),
      remove: async () => {assert.equal(closed, 1); ++removed;},
    },
    time: {sleep: tick},
  };
  const context = {pocket, console: {log: line => logs.push(line)}};
  vm.runInNewContext(source('video_sd_av_cleanup'), context);
  context.frame();
  for (let i = 0; i < 1500 && !logs.some(line =>
       line.includes('REMOVED') || line.includes('REFUSED')); ++i) await tick();
  return {removed, logs};
}

(async () => {
  assert.equal(await stage(true), null);
  const bytes = await stage();
  await overlay(bytes);
  assert.equal((await cleanup(bytes)).removed, 1);
  assert.equal((await cleanup(bytes.subarray(0, 49152))).removed, 1);
  assert.equal((await cleanup(bytes.subarray(0, 0))).removed, 0);
  const altered = Buffer.from(bytes); altered[8192] ^= 1;
  const refused = await cleanup(altered);
  assert.equal(refused.removed, 0);
  assert(refused.logs.some(line => line.includes('REFUSED')));
  console.log('D6 stage, audio-clock overlay, exact full/prefix cleanup PASS');
})().catch(error => {console.error(error); process.exitCode = 1;});
