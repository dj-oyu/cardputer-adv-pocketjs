// Visual authoring handles own the typed packets and their dependency graph.
'use strict';
globalThis.Ayatsuri = (() => {
  const PI = Math.PI, TAU = PI * 2;
  function fail(text) { throw new TypeError('Ayatsuri: ' + text); }
  function finite(x) { if (typeof x !== 'number' || x - x !== 0) fail('finite number required'); return x; }
  function unit(x) { finite(x); if (x < 0 || x > 1) fail('coefficient outside 0..1'); return x; }
  function rgb(x) { if (x !== (x >>> 0) || x > 0xffffff) fail('RGB24 required'); return x; }
  function vector(src, size) {
    if (!src || src.length !== size) fail('vector length');
    for (let i = 0; i < size; ++i) { const v = Math.fround(finite(src[i])); if (v - v !== 0) fail('finite float32 vector required'); }
  }
  function pack(x) { return Math.round((x >>> 16) * 31 / 255) * 2048 + Math.round((x >> 8 & 255) * 63 / 255) * 32 + Math.round((x & 255) * 31 / 255); }
  function vec(dst, at, src, size) {
    vector(src, size);
    let changed = false;
    for (let i = 0; i < size; ++i) if (dst[at + i] !== Math.fround(src[i])) changed = true;
    if (changed) for (let i = 0; i < size; ++i) dst[at + i] = src[i];
    return changed;
  }
  function coefficient(target, index, patch, name) {
    if (patch[name] === undefined) return false;
    const value = Math.fround(unit(patch[name]));
    if (target[index] === value) return false;
    target[index] = value; return true;
  }
  function geometry(def, storage) {
    if (!def) return null;
    if (def.kind !== 'ellipsoid') fail('geometry must be ellipsoid or null');
    vector(def.radii, 3);
    for (let i = 0; i < 3; ++i) if (!(Math.fround(def.radii[i]) > 0)) fail('positive radii required');
    const work = new Float32Array(storage);
    for (let i = 0; i < 3; ++i) work[21 + i] = def.radii[i];
    return {kind: 'ellipsoid', work, surface: new Uint16Array(storage, 232, 5)};
  }
  function compatibility(patch) {
    if (patch.kind === 'ellipsoid' && !patch.screen) {
      const storage = patch.work.buffer, radii = new Float32Array(storage, 84, 3),
        screen = new Float32Array(storage, 96, 8), normalMatrix = new Float32Array(storage, 128, 9),
        texture = new Float32Array(storage, 196, 9), material = new Float32Array(storage, 164, 8);
      patch.radii = radii; patch.screen = screen; patch.normalMatrix = normalMatrix; patch.texture = texture; patch.material = material;
    }
    return patch;
  }
  function nodeAlive(node) { if (node.owner.disposed || node.owner.busy) fail(node.owner.disposed ? 'disposed scene' : 'busy scene'); }
      function place(patch) {
        const node = this._node; nodeAlive(node); let changed = false;
        if (patch.position) vector(patch.position, 3);
        if (patch.rotation) vector(patch.rotation, 3);
        if (patch.scale) {
          vector(patch.scale, 3);
          for (let i = 0; i < 3; ++i) if (Math.fround(patch.scale[i]) === 0) fail('nonzero finite scale');
        }
        if (patch.position) changed = vec(node.matrix, 12, patch.position, 3) || changed;
        if (patch.rotation) changed = vec(node.matrix, 15, patch.rotation, 3) || changed;
        if (patch.scale) changed = vec(node.matrix, 18, patch.scale, 3) || changed;
        if (changed) { ++node.transformVersion; node.owner.dirty = true; }
      }
      function material(patch) {
        const node = this._node; nodeAlive(node);
        if (!node.analytic) fail('group has no material');
        if (patch.color !== undefined) rgb(patch.color);
        if (patch.transmissionColor !== undefined) rgb(patch.transmissionColor);
        if (node.geometryExtension) node.geometryExtension.validateMaterial(patch);
        else if (patch.ambientColor !== undefined) fail('ambientColor requires blendEllipsoids');
        for (const key of ['ambient', 'diffuse', 'specular', 'roughness', 'transmission', 'thickness', 'opacity', 'edgeSoftness']) if (patch[key] !== undefined) unit(patch[key]);
        if (patch.color !== undefined && rgb(patch.color) !== node.color) { node.color = patch.color; ++node.albedoVersion; node.owner.dirty = true; }
        const data = node.geometryExtension ? node.materialData : node.analytic.work, base = node.geometryExtension ? 0 : 41;
        let changed = false;
        changed = coefficient(data, base, patch, 'ambient') || changed;
        changed = coefficient(data, base + 1, patch, 'diffuse') || changed;
        changed = coefficient(data, base + 2, patch, 'specular') || changed;
        changed = coefficient(data, base + 4, patch, 'transmission') || changed;
        if (patch.roughness !== undefined) {
          const r = unit(patch.roughness), exponent = Math.fround(1 + 127 * (1 - r) * (1 - r));
          if (exponent !== data[base + 3]) { data[base + 3] = exponent; changed = true; }
        }
        if (patch.transmissionColor !== undefined) {
          const c = rgb(patch.transmissionColor);
          for (let i = 0; i < 3; ++i) {
            const v = Math.fround(((c >> ((2 - i) * 8)) & 255) / 255);
            if (v !== data[base + 5 + i]) { data[base + 5 + i] = v; changed = true; }
          }
        }
        if (node.geometryExtension) changed = node.geometryExtension.material(node, patch) || changed;
        if (patch.thickness !== undefined && unit(patch.thickness) !== node.thickness) { node.thickness = patch.thickness; changed = true; }
        if (patch.opacity !== undefined && patch.opacity !== node.opacity) { node.opacity = patch.opacity; changed = true; }
        if (patch.edgeSoftness !== undefined && patch.edgeSoftness !== node.edgeSoftness) { node.edgeSoftness = patch.edgeSoftness; changed = true; }
        if (changed) { ++node.shadingVersion; node.owner.dirty = true; }
      }
      function texture(patch) {
        const node = this._node; nodeAlive(node); let changed = false;
        if (!node.analytic) fail('group has no texture');
        if (node.geometryExtension) fail('geometry has no texture');
        if (node.pattern && node.pattern.kind === 'localPlane') fail('localPlane has no UV transform');
        if (patch.offset) vector(patch.offset, 2);
        if (patch.scale) vector(patch.scale, 2);
        if (patch.rotation !== undefined) finite(Math.fround(finite(patch.rotation)));
        if (patch.offset) changed = vec(node.analytic.work, 61, patch.offset, 2) || changed;
        if (patch.scale) changed = vec(node.analytic.work, 63, patch.scale, 2) || changed;
        if (patch.rotation !== undefined) {
          const v = Math.fround(finite(patch.rotation)); if (v !== node.analytic.work[65]) { node.analytic.work[65] = v; changed = true; }
        }
        if (changed) { ++node.albedoVersion; node.owner.dirty = true; }
      }
      function visible(value) { const node = this._node; nodeAlive(node); if (typeof value !== 'boolean') fail('visible boolean'); if (node.enabled !== value) { node.enabled = value; node.owner.dirty = true; } }
  const HandlePrototype = {place, material, texture, visible};
  function scene(options) {
    options = options || {};
    if (options.geometries && !Array.isArray(options.geometries)) fail('geometry extensions');
    let geometries = options.geometries ? options.geometries.slice() : null;
    const backend = options.backend || (typeof pocket !== 'undefined' ? pocket.hone : null),
      evaluator = options.evaluator || (typeof pocket !== 'undefined' ? pocket.ayatsuri : null), objects = [],
      lightState = new Float32Array([-0.5, -0.4, 0.8, 1, 1, 1]),
      cameraState = new Float32Array([0, 0, 400, 0, 0, 0, 1, 120, 75]),
      lighting = new Float32Array(9),
      right = new Float32Array(3), down = new Float32Array(3), forward = new Float32Array(3),
      counts = {geometryUpdates: 0, normalUpdates: 0, projectionUpdates: 0, albedoUpdates: 0,
        shadeUpdates: 0, shadeSamples: 0, analyticUpdates: 0, renderedFaces: 0, renderedPatches: 0, copiedBytes: 0,
        typedBytes: lightState.byteLength + cameraState.byteLength + lighting.byteLength + right.byteLength + down.byteLength + forward.byteLength};
    if (!backend || (typeof backend.ellipsoid !== 'function' && typeof backend.ellipsoidPacked !== 'function')) fail('analytic hone backend required');
    if (!evaluator || (typeof evaluator.compose !== 'function' && typeof evaluator.composeNode !== 'function')) fail('shared ayatsuri evaluator required');
    const owner = {disposed: false, busy: false, dirty: true};
    let geometryContext = geometries ? {cameraState, right, down, forward, counts, pack} : null;
    if (geometries && geometries.some(g => !g || typeof g.kind !== 'string')) fail('geometry extensions');
    let lightVersion = 1, cameraVersion = 1, evaluatedLight = 0, evaluatedCamera = 0, evaluated = false;
    function alive() { if (owner.disposed || owner.busy) fail(owner.disposed ? 'disposed scene' : 'busy scene'); }
    function light(patch) {
      alive(); let change = false;
      if (patch.color !== undefined) rgb(patch.color);
      if (patch.direction) {
        vector(patch.direction, 3);
        const length = Math.hypot(...patch.direction);
        if (!(length > 0)) fail('nonzero light direction');
        for (let i = 0; i < 3; ++i) {
          const v = Math.fround(patch.direction[i] / length);
          if (v !== lightState[i]) { lightState[i] = v; change = true; }
        }
      }
      if (patch.color !== undefined) {
        const c = rgb(patch.color);
        for (let i = 0; i < 3; ++i) {
          const v = Math.fround(((c >> ((2 - i) * 8)) & 255) / 255);
          if (v !== lightState[3 + i]) { lightState[3 + i] = v; change = true; }
        }
      }
      if (change) { ++lightVersion; owner.dirty = true; }
    }
    function camera(patch) {
      alive(); let change = false;
      if (patch.position) vector(patch.position, 3);
      if (patch.target) vector(patch.target, 3);
      if (patch.center) vector(patch.center, 2);
      if (patch.zoom !== undefined && !(finite(Math.fround(finite(patch.zoom))) > 0)) fail('positive float32 zoom');
      if (patch.position) change = vec(cameraState, 0, patch.position, 3) || change;
      if (patch.target) change = vec(cameraState, 3, patch.target, 3) || change;
      if (patch.zoom !== undefined) {
        const v = Math.fround(finite(patch.zoom)); if (!(v > 0)) fail('positive zoom');
        if (v !== cameraState[6]) { cameraState[6] = v; change = true; }
      }
      if (patch.center) change = vec(cameraState, 7, patch.center, 2) || change;
      if (change) { ++cameraVersion; owner.dirty = true; }
    }
    function add(def) {
      alive();
      const dirtyBefore = owner.dirty;
      try {
      if (!def || typeof def.name !== 'string' || !def.name || objects.some(x => x.name === def.name)) fail('unique object name');
      const parent = def.parent === undefined || def.parent === null ? null : def.parent._node;
      if (def.parent !== undefined && def.parent !== null && (!parent || !objects.includes(parent))) fail('parent must be a handle in this scene');
      const analytic = !!def.geometry;
      let extension = null;
      if (analytic && geometries) for (let i = 0; i < geometries.length; ++i) if (geometries[i].kind === def.geometry.kind) extension = geometries[i];
      const bytes = extension ? extension.bytes(def.geometry, backend) : analytic ? 264 : 84;
      if (extension && (def.pattern || def.textureTransform)) fail('geometry has no pattern or texture');
      // One backing store removes per-field ArrayBuffer headers. The typed
      // views are stable for synchronous native calls and never escape as API.
      const storage = new ArrayBuffer(bytes);
      const node = {name: def.name, parent, owner, matrix: new Float32Array(storage, 0, 21),
        analytic: extension ? extension.create(def.geometry, storage) : geometry(def.geometry, storage),
        enabled: true, worldEnabled: true,
        transformVersion: 1, evaluatedTransform: 0, worldVersion: 0, parentVersion: -1};
      if (extension) node.geometryExtension = extension;
      node.matrix[18] = node.matrix[19] = node.matrix[20] = 1;
      if (typeof evaluator.composeNode !== 'function') {
        node.pose = new Float32Array(storage, 48, 9); node.compatMatrix = new Float32Array(storage, 0, 12);
      }
      if (analytic) {
        if (extension) node.materialData = new Float32Array(storage, extension.materialOffset, extension.materialLength);
        const data = extension ? node.materialData : node.analytic.work, base = extension ? 0 : 41;
        node.color = 0xffffff; node.thickness = 0.5; node.opacity = 1; node.edgeSoftness = 0.04; node.pattern = null;
        node.albedoVersion = 1; node.evaluatedAlbedo = 0; node.shadingVersion = 1; node.evaluatedShading = 0;
        data[base] = 0.25; data[base + 1] = 0.75; data[base + 3] = 32;
        data[base + 5] = data[base + 6] = data[base + 7] = 1;
        if (extension) extension.initializeMaterial(node.materialData);
        else data[63] = data[64] = 1;
      }
      const handle = Object.create(HandlePrototype); handle._node = node;
      if (def.position || def.rotation || def.scale) handle.place(def);
      if (def.material) handle.material(def.material);
      if (def.pattern) {
        const p = def.pattern;
        if (!analytic) fail('group has no texture');
        if (p.kind === 'image') fail('image pattern unsupported');
        if (p.kind === 'localPlane') {
          vector(p.normal, 3);
          const length = Math.hypot(p.normal[0], p.normal[1], p.normal[2]),
            offset = p.offset === undefined ? 0 : finite(p.offset), feather = p.feather === undefined ? 0 : finite(p.feather);
          if (!(length > 0) || Math.abs(offset) > 2 || feather < 0 || feather > 2 || (feather > 0 && feather < 1e-4)) fail('localPlane range');
          node.pattern = {kind: p.kind, normal: [p.normal[0] / length, p.normal[1] / length, p.normal[2] / length], offset, feather, color: rgb(p.color)};
        } else {
        if (p.kind !== 'band' && p.kind !== 'checker') fail('pattern kind');
          node.pattern = {kind: p.kind, axis: p.axis || 'v', start: p.start === undefined ? 0.5 : unit(p.start),
            end: p.end === undefined ? 1 : unit(p.end), color: rgb(p.color), repeats: p.repeats === undefined ? 4 : finite(p.repeats)};
          if ((node.pattern.axis !== 'u' && node.pattern.axis !== 'v') || node.pattern.start > node.pattern.end || node.pattern.repeats <= 0) fail('pattern range');
        }
      }
      if (def.textureTransform) handle.texture(def.textureTransform);
      if (def.visible !== undefined) handle.visible(def.visible);
      objects.push(node);
      counts.typedBytes += storage.byteLength;
      owner.dirty = true;
      return handle;
      } catch (error) { owner.dirty = dirtyBefore; throw error; }
    }
    function evaluateWork(timeMs) {
      finite(timeMs);
      const lightChanged = evaluatedLight !== lightVersion, cameraChanged = evaluatedCamera !== cameraVersion;
      let fx = cameraState[0] - cameraState[3], fy = cameraState[1] - cameraState[4], fz = cameraState[2] - cameraState[5], distance = Math.hypot(fx, fy, fz);
      if (!(distance > 0)) fail('camera position equals target');
      fx /= distance; fy /= distance; fz /= distance;
      forward[0] = fx; forward[1] = fy; forward[2] = fz;
      let rx = fz, rz = -fx, length = Math.hypot(rx, rz);
      if (length < 1e-6) { rx = 1; rz = 0; length = 1; }
      rx /= length; rz /= length; right[0] = rx; right[1] = 0; right[2] = rz;
      down[0] = fy * rz; down[1] = fz * rx - fx * rz; down[2] = -fy * rx;
      if (lightChanged || cameraChanged) {
        for (let k = 0; k < 3; ++k) { lighting[k] = lightState[k]; lighting[6 + k] = lightState[3 + k]; }
        // Every orthographic patch shares the same world view direction.
        lighting[3] = fx; lighting[4] = fy; lighting[5] = fz;
      }
      for (let i = 0; i < objects.length; ++i) {
        const node = objects[i], parent = node.parent, m = node.matrix,
          geometryChanged = node.evaluatedTransform !== node.transformVersion || (parent && node.parentVersion !== parent.worldVersion);
        node.worldEnabled = node.enabled && (!parent || parent.worldEnabled);
        if (geometryChanged) {
          if (typeof evaluator.composeNode === 'function') evaluator.composeNode(m, parent ? parent.matrix : null);
          else evaluator.compose(node.pose, parent ? parent.compatMatrix : null, node.compatMatrix);
          node.evaluatedTransform = node.transformVersion; node.parentVersion = parent ? parent.worldVersion : 0; ++node.worldVersion;
          ++counts.geometryUpdates;
        }
        if (node.analytic) {
          const patch = node.analytic;
          if (node.geometryExtension) {
            node.geometryExtension.evaluate(node, geometryChanged, cameraChanged, lightChanged, geometryContext);
            continue;
          }
          if (geometryChanged) {
            const r = patch.work, a = m[0] * r[21], b = m[1] * r[22], c = m[2] * r[23],
              d = m[4] * r[21], e = m[5] * r[22], f = m[6] * r[23], g = m[8] * r[21], h = m[9] * r[22], j = m[10] * r[23],
              determinant = a * (e * j - f * h) - b * (d * j - f * g) + c * (d * h - e * g), n = patch.work;
            if (determinant - determinant !== 0 || Math.abs(determinant) < 1e-12) fail('singular transform');
            n[32] = (e * j - f * h) / determinant; n[33] = (f * g - d * j) / determinant; n[34] = (d * h - e * g) / determinant;
            n[35] = (c * h - b * j) / determinant; n[36] = (a * j - c * g) / determinant; n[37] = (b * g - a * h) / determinant;
            n[38] = (b * f - c * e) / determinant; n[39] = (c * d - a * f) / determinant; n[40] = (a * e - b * d) / determinant;
            ++counts.normalUpdates;
          }
          if (geometryChanged || cameraChanged) {
            const r = patch.work, s = patch.work, zoom = cameraState[6];
            for (let k = 0; k < 3; ++k) {
              s[24 + k] = zoom * (right[0] * m[k] + right[1] * m[4 + k] + right[2] * m[8 + k]) * r[21 + k];
              s[28 + k] = zoom * (down[0] * m[k] + down[1] * m[4 + k] + down[2] * m[8 + k]) * r[21 + k];
            }
            const x = m[3] - cameraState[3], y = m[7] - cameraState[4], z = m[11] - cameraState[5];
            s[27] = cameraState[7] + zoom * (right[0] * x + right[1] * y + right[2] * z);
            s[31] = cameraState[8] + zoom * (down[0] * x + down[1] * y + down[2] * z);
            ++counts.projectionUpdates;
          }
          const albedoChanged = node.evaluatedAlbedo !== node.albedoVersion;
          if (albedoChanged || node.evaluatedShading !== node.shadingVersion || geometryChanged) {
            const surface = patch.surface, t = patch.work, cs = Math.cos(t[65]), sn = Math.sin(t[65]), pattern = node.pattern;
            surface[0] = pack(node.color); surface[1] = pattern ? pack(pattern.color) : surface[0];
            surface[2] = Math.round(node.opacity * 255);
            surface[3] = Math.round(Math.min(1, node.thickness * Math.hypot(m[2], m[6], m[10])) * 255);
            surface[4] = !pattern ? 0 : pattern.kind === 'localPlane' ? 4 : pattern.kind === 'checker' ? 3 : pattern.axis === 'u' ? 1 : 2;
            if (pattern && pattern.kind === 'localPlane') {
              for (let k = 0; k < 3; ++k) t[49+k] = pattern.normal[k];
              t[52] = pattern.offset; t[53] = pattern.feather; t[54] = 0;
              t[56] = 0; t[57] = 1;
            } else {
            t[49] = cs * t[63]; t[50] = -sn * t[64]; t[51] = 0.5 + t[61] - (t[49] + t[50]) * 0.5;
            t[52] = sn * t[63]; t[53] = cs * t[64]; t[54] = 0.5 + t[62] - (t[52] + t[53]) * 0.5;
            if (pattern && pattern.kind === 'checker') for (let k = 0; k < 6; ++k) t[49 + k] *= pattern.repeats;
            t[56] = pattern ? pattern.start : 0; t[57] = pattern ? pattern.end : 1;
            }
            t[55] = node.edgeSoftness;
            if (albedoChanged) ++counts.albedoUpdates;
          }
          if (geometryChanged || cameraChanged || lightChanged || albedoChanged || node.evaluatedShading !== node.shadingVersion) {
            ++counts.shadeUpdates; ++counts.analyticUpdates;
          }
          node.evaluatedAlbedo = node.albedoVersion; node.evaluatedShading = node.shadingVersion;
          continue;
        }
      }
      evaluatedCamera = cameraVersion; evaluatedLight = lightVersion; evaluated = true;
    }
    function evaluate(timeMs) {
      alive(); owner.busy = true;
      try { evaluateWork(timeMs); owner.dirty = false; }
      catch (error) {
        // Never render partially reevaluated packets. A later valid input can
        // recover, while the app keeps its previously committed image.
        evaluated = false; evaluatedCamera = 0; evaluatedLight = 0;
        for (let i = 0; i < objects.length; ++i) {
          const node = objects[i]; node.evaluatedTransform = 0;
          if (node.analytic) { node.evaluatedAlbedo = 0; node.evaluatedShading = 0; }
        }
        throw error;
      } finally { owner.busy = false; }
    }
    function visit(visitor) {
      alive(); if (!evaluated || owner.dirty) fail('evaluate before render');
      owner.busy = true;
      try { for (let i = 0; i < objects.length; ++i) if (objects[i].worldEnabled && objects[i].analytic) visitor(compatibility(objects[i].analytic), objects[i].name); }
      finally { owner.busy = false; }
    }

    function render() {
      alive(); if (!evaluated || owner.dirty) fail('evaluate before render');
      if (!backend || (!backend.ellipsoid && !backend.ellipsoidPacked)) fail('analytic hone backend required');
      counts.renderedFaces = 0; counts.renderedPatches = 0;
      owner.busy = true;
      try {
      for (let i = 0; i < objects.length; ++i) {
        const node = objects[i];
        if (node.analytic && node.worldEnabled) {
          const patch = node.analytic;
          if (node.geometryExtension) node.geometryExtension.render(node, lighting, backend);
          else if (typeof backend.ellipsoidPacked === 'function') backend.ellipsoidPacked(patch.work.buffer, 96, lighting);
          else {
            compatibility(patch);
            backend.ellipsoid(patch.screen, patch.normalMatrix, lighting, patch.material, patch.surface, patch.texture);
          }
          ++counts.renderedPatches; continue;
        }
      }
      } finally { owner.busy = false; }
    }
    function stats(out) { alive(); for (const key in counts) out[key] = counts[key]; out.objects = objects.length; }
    function dispose() {
      if (owner.busy) fail('busy scene');
      if (!owner.disposed) {
        for (let i = 0; i < objects.length; ++i) if (objects[i].geometryExtension) objects[i].geometryExtension = null;
        objects.length = 0; geometries = geometryContext = null; owner.disposed = true;
      }
    }
    light(options.light || {direction: [-0.5, -0.4, 0.8]}); if (options.camera) camera(options.camera);
    return {add, light, camera, evaluate, render, visit, stats, dispose};
  }
  return {scene, pack};
})();
globalThis.Character = globalThis.Ayatsuri; // Compatibility alias; one shared implementation.
if (typeof module !== 'undefined') module.exports = globalThis.Ayatsuri;


