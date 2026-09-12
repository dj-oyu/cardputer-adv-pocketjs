// vmrun-flags: --host-events
// L1 sec.2.1 for the one intake that is not a pump: pocket.app.exit().
//
// exit() sets a flag from inside a job, and the stop it asks for reaches the
// guest as an UNCATCHABLE interrupt on the next call into JavaScript. Read that
// flag at the top of a continuation turn and job k+1 of the drain that carried
// the exit() dies at its first instruction: the .then handlers after it never
// run, the .finally never runs, and WHICH of them ran depends on where the
// budget happened to fall -- the one thing sec.2.1 says a budget boundary must
// never decide. Pre-L1 the flag was read in pocket_app_pump(), i.e. only on a
// turn that began with an empty queue, and the whole chain ran first.
//
// So: exit() in job 3 of a chain of eight, and every later job -- including a
// finally, including a handler on a promise that was resolved before the exit
// -- still runs, at every budget the corpus runs this file at.
print('start');
let p = Promise.resolve();
for (let i = 1; i <= 8; i++) {
  const n = i;
  p = p.then(() => {
    print('job ' + n);
    if (n === 3) { host.exit(); print('exit requested'); }
  });
}
p.finally(() => print('finally')).then(() => print('tail'));
