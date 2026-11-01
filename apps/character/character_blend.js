// Optional smooth-volume authoring; ordinary ellipsoid scenes do not load this chunk.
'use strict';
globalThis.AyatsuriBlend = (() => {
  function fail(text) { throw new TypeError('Ayatsuri: ' + text); }
  function finite(x) { if (typeof x !== 'number' || x - x !== 0) fail('finite number required'); return x; }
  function vector(value, size) {
    if (!value || value.length !== size) fail('vector length');
    for (let i = 0; i < size; ++i) finite(Math.fround(finite(value[i])));
  }
  function count(def) {
    if (!Array.isArray(def.lobes) || def.lobes.length < 1 || def.lobes.length > 8) fail('blendEllipsoids needs 1..8 lobes');
    const blend = def.blend === undefined ? 8 : finite(def.blend);
    if (blend < 0 || blend > 128) fail('blend must be in 0..128 world units');
    for (let i = 0; i < def.lobes.length; ++i) {
      const lobe = def.lobes[i];
      if (!lobe) fail('lobe definition');
      vector(lobe.position, 3); vector(lobe.radii, 3);
      for (let k = 0; k < 3; ++k) if (!(Math.fround(lobe.radii[k]) > 0)) fail('positive radii required');
    }
    return def.lobes.length;
  }
  function bytes(def, backend) {
    if (typeof backend.blendEllipsoids !== 'function') fail('blendEllipsoids backend required');
    return 316 + count(def) * 108;
  }
  function create(def, storage) {
    const count = def.lobes.length, local = new Float32Array(storage, 316, count * 6),
      controls = new Float32Array(storage, 308, 2);
    for (let i = 0; i < count; ++i) for (let k = 0; k < 3; ++k) {
      local[i * 6 + k] = def.lobes[i].position[k]; local[i * 6 + 3 + k] = def.lobes[i].radii[k];
    }
    controls[0] = def.blend === undefined ? 8 : def.blend;
    return {kind: 'blendEllipsoids', count, local, lobes: new Float32Array(storage, 316 + count * 24, count * 21),
      controls, surface: new Uint16Array(storage, 252, 3)};
  }
  function initializeMaterial(data) { data[8] = data[9] = data[10] = 1; }
  function validateMaterial(patch) {
    const c = patch.ambientColor;
    if (c !== undefined && (c !== (c >>> 0) || c > 0xffffff)) fail('RGB24 required');
  }
  function material(node, patch) {
    let changed = false;
    if (patch.ambientColor !== undefined) {
      const c = patch.ambientColor;
      for (let i = 0; i < 3; ++i) {
        const v = Math.fround(((c >> ((2 - i) * 8)) & 255) / 255);
        if (v !== node.materialData[8 + i]) { node.materialData[8 + i] = v; changed = true; }
      }
    }
    return changed;
  }
  function evaluate(node, geometryChanged, cameraChanged, lightChanged, context) {
      const {cameraState, right, down, forward, counts} = context;
      const patch = node.analytic, m = node.matrix, local = patch.local, packet = patch.lobes;
      if (geometryChanged || cameraChanged) {
        const zoom = cameraState[6];
        for (let i = 0; i < patch.count; ++i) {
          const p = i * 21, l = i * 6, x = local[l], y = local[l + 1], z = local[l + 2],
            a = m[0] * local[l + 3], b = m[1] * local[l + 4], c = m[2] * local[l + 5],
            d = m[4] * local[l + 3], e = m[5] * local[l + 4], f = m[6] * local[l + 5],
            g = m[8] * local[l + 3], h = m[9] * local[l + 4], j = m[10] * local[l + 5],
            wx = m[0] * x + m[1] * y + m[2] * z + m[3] - cameraState[3],
            wy = m[4] * x + m[5] * y + m[6] * z + m[7] - cameraState[4],
            wz = m[8] * x + m[9] * y + m[10] * z + m[11] - cameraState[5];
          if (geometryChanged) {
            const determinant = a * (e * j - f * h) - b * (d * j - f * g) + c * (d * h - e * g);
            if (determinant - determinant !== 0 || Math.abs(determinant) < 1e-12) fail('singular transform');
            packet[p + 8] = (e * j - f * h) / determinant; packet[p + 9] = (f * g - d * j) / determinant; packet[p + 10] = (d * h - e * g) / determinant;
            packet[p + 11] = (c * h - b * j) / determinant; packet[p + 12] = (a * j - c * g) / determinant; packet[p + 13] = (b * g - a * h) / determinant;
            packet[p + 14] = (b * f - c * e) / determinant; packet[p + 15] = (c * d - a * f) / determinant; packet[p + 16] = (a * e - b * d) / determinant;
          }
          packet[p] = zoom * (right[0] * a + right[1] * d + right[2] * g);
          packet[p + 1] = zoom * (right[0] * b + right[1] * e + right[2] * h);
          packet[p + 2] = zoom * (right[0] * c + right[1] * f + right[2] * j);
          packet[p + 3] = cameraState[7] + zoom * (right[0] * wx + right[1] * wy + right[2] * wz);
          packet[p + 4] = zoom * (down[0] * a + down[1] * d + down[2] * g);
          packet[p + 5] = zoom * (down[0] * b + down[1] * e + down[2] * h);
          packet[p + 6] = zoom * (down[0] * c + down[1] * f + down[2] * j);
          packet[p + 7] = cameraState[8] + zoom * (down[0] * wx + down[1] * wy + down[2] * wz);
          packet[p + 17] = forward[0] * a + forward[1] * d + forward[2] * g;
          packet[p + 18] = forward[0] * b + forward[1] * e + forward[2] * h;
          packet[p + 19] = forward[0] * c + forward[1] * f + forward[2] * j;
          packet[p + 20] = forward[0] * wx + forward[1] * wy + forward[2] * wz;
          for (let k = 0; k < 21; ++k) if (Math.abs(packet[p + k]) > (k >= 8 && k < 17 ? 1e6 : 4096) || packet[p + k] - packet[p + k] !== 0) fail('blendEllipsoids packet range');
        }
        if (geometryChanged) ++counts.normalUpdates;
        ++counts.projectionUpdates;
      }
      const albedoChanged = node.evaluatedAlbedo !== node.albedoVersion, shadingChanged = node.evaluatedShading !== node.shadingVersion;
      if (albedoChanged || shadingChanged || geometryChanged) {
        patch.surface[0] = context.pack(node.color); patch.surface[1] = Math.round(node.opacity * 255);
        patch.surface[2] = Math.round(Math.min(1, node.thickness * Math.hypot(m[2], m[6], m[10])) * 255);
        patch.controls[1] = node.edgeSoftness;
        if (albedoChanged) ++counts.albedoUpdates;
      }
      if (geometryChanged || cameraChanged || lightChanged || albedoChanged || shadingChanged) { ++counts.shadeUpdates; ++counts.analyticUpdates; }
      node.evaluatedAlbedo = node.albedoVersion; node.evaluatedShading = node.shadingVersion;
    }
  function render(node, lighting, backend) {
    const patch = node.analytic;
    backend.blendEllipsoids(patch.lobes, lighting, node.materialData, patch.surface, patch.controls);
  }
  return {kind: 'blendEllipsoids', materialOffset: 264, materialLength: 11,
    bytes, create, initializeMaterial, validateMaterial, material, evaluate, render};
})();
if (typeof module !== 'undefined') module.exports = globalThis.AyatsuriBlend;
