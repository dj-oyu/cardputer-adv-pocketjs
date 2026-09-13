// vmrun-flags: --module
// yield_module_tla (design sec.12.3's revision, D17r): a module's OWN
// top-level synchronous stretch never gets a MAY_YIELD token -- js_execute_
// {sync,async}_module calls js_async_function_call directly, and neither
// js_async_function_resume nor js_async_function_call is a token writer
// (only js_async_function_resolve_call, the await-RETURN path, is) -- so the
// loop BEFORE the first top-level await must show stops == 0 even once
// L2c's body lands. AFTER the resume (the same await-return token any other
// async function gets) the module is an ordinary ASYNC floor, so the loop
// after the `await` is expected to yield.
function costly(n) {
  let t = 0;
  for (let i = 0; i < n; i++) t += i;
  return t;
}
print("before-tla", costly(2600));
await new Promise((resolve) => resolve());
print("after-tla", costly(2600));
