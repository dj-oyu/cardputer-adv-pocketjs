// One controller owns APP submissions. Domain state belongs to the caller;
// candidate refs stay private until LCD acknowledgement. No timer or queue.
(function (view, options) {
  if (!options || typeof options.build !== 'function' ||
      (options.patch !== undefined && typeof options.patch !== 'function'))
    throw TypeError('createScene requires build and optional patch callbacks');
  const build = options.build, patch = options.patch;
  let refs = null, candidate = null, pending = false, pendingReplace = false;
  let dirty = true, rebuild = true, running = false, model;
  function replace(tx) {
    candidate = build(tx, model);
    if (!candidate || typeof candidate !== 'object')
      throw TypeError('scene build must return an object containing candidate refs');
    return candidate; // Let the native builder reject thenables too.
  }
  function update(tx) { const result = patch(tx, refs, model); candidate = refs; return result; }
  return {
    invalidate: function (topologyChanged) {
      dirty = true;
      if (topologyChanged) rebuild = true;
    },
    flush: function (state) {
      if (running) throw Error('scene flush is not reentrant');
      running = true;
      try {
        if (pending) {
          const outcome = view.poll();
          if (outcome.status === 'SUBMITTED') return false;
          if (outcome.status === 'PRESENTED') refs = candidate;
          else if (outcome.status === 'DISCARDED') {
            dirty = true;
            if (pendingReplace) rebuild = true;
          } else throw Error('scene lost its submission');
          candidate = null;
          pending = false;
        }
        if (!dirty) return true;
        const replacing = rebuild || refs === null || !patch;
        dirty = false;
        rebuild = false;
        model = state;
        try {
          if (replacing) view.replace(replace);
          else view.patch(update);
          pendingReplace = replacing;
          pending = true;
        } catch (error) {
          candidate = null;
          dirty = true;
          if (replacing) rebuild = true;
          if (!error || error.code !== 'BUSY') throw error;
        } finally { model = undefined; }
        return false;
      } finally { running = false; }
    }
  };
})
