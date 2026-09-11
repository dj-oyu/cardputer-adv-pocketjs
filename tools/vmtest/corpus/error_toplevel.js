// An uncaught exception at top level: the guest dumps it and returns
// ESP_FAIL without draining, so the queued job below never runs.
Promise.resolve().then(() => print("never runs"));
function thrower() { throw new SyntaxError("top-level " + [1, 2].map((x) => x * 2)); }
print("before");
thrower();
print("unreached");
