// Execute the actual generated simulation, retaining the old synchronous loop
// as a pure work/result oracle. No app lifecycle, renderer, USB or timing claim.
const fs = require('node:fs'), vm = require('node:vm');
const [watch, manifest] = process.argv.slice(2);
const fixture = JSON.parse(fs.readFileSync(manifest, 'utf8'));
const context = {fixture};
vm.createContext(context);
vm.runInContext(fs.readFileSync(watch, 'utf8'), context, {filename: watch});
const result = vm.runInContext(`(() => {
  const F = field(parseInt(fixture.seed, 16)), rs = race(F);
  for (let i = 0; i < fixture.end; i++) step(F, rs, DT);
  let tail_steps = 0;
  while (rs.done < 8 && rs.t < 200) { step(F, rs, DT); tail_steps++; }
  const o = order(rs), text = o.map(i => i + 1).join(',') + '|' + o.map(i => Math.round(rs.tc[i] * 1e6)).join(',');
  let digest = 2166136261;
  for (let i = 0; i < text.length; i++) digest = Math.imul(digest ^ text.charCodeAt(i), 16777619) >>> 0;
  const finish = digest.toString(16).toUpperCase().padStart(8, '0');
  if (!F.o || finish !== fixture.finish) throw Error('tail reference fixture/result drift');
  return {tail_steps, finish, order: o, times_us: o.map(i => Math.round(rs.tc[i] * 1e6))};
})()`, context);
process.stdout.write(JSON.stringify(result) + '\n');
