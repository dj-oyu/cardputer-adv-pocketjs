// Benchmark: await loop + then chain (job queue throughput, async frame save/restore).
(async () => {
  let s = 0;
  for (let i = 0; i < 100000; i++) s = (s + await i) % 1000003;
  let p = Promise.resolve(0);
  for (let i = 0; i < 50000; i++) p = p.then((v) => (v + i) % 1000003);
  print("bench_promise", s, await p);
})();
