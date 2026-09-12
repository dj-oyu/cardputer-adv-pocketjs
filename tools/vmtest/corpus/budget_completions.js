// vmrun-flags: --frames 1 --budget-jobs 4 --host-events
// L1 invariant 6 (docs/vm-L1-design.md sec.7): a completion recorded while the
// queue is still being worked off is neither lost nor delivered early.
//
// host.request(k) is vmrun's stand-in for pocket_api_complete(): the host
// records the completion at the k-th job boundary, from outside JavaScript,
// which on the device can happen in an ISR or another task at any moment. What
// settles it is the host's pump -- and the design's rule is that the pump does
// not run on a continuation turn. So all eight must:
//   - be delivered, none dropped;
//   - be delivered exactly once, in the order they were requested (seq= is the
//     host's own delivery counter, so seq=1..8 in request order is the proof);
//   - be delivered only AFTER the 40-step chain below has finished, i.e. after
//     the drain actually emptied -- "chain-end" comes first in the output.
// The k values 0..7 all fall due during the continuation turns the chain
// forces, so this is the interesting case and not an accident of timing.
const log = [];
for (let i = 0; i < 8; i++)
  host.request(i).then((seq) => log.push('done ' + i + ' seq=' + seq));

let c = Promise.resolve();
for (let i = 0; i < 40; i++) c = c.then(() => {});
c.then(() => log.push('chain-end'));

function frame() {
  for (const line of log) print(line);
  print('total ' + log.length);
}
