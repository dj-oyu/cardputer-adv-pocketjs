// Visual authoring handles own the typed packets and their dependency graph.
'use strict';
const Character = (() => {
  const PI = Math.PI, TAU = PI * 2;
  function fail(text) { throw new TypeError('Character: ' + text); }
  function finite(x) { if (!Number.isFinite(x)) fail('finite number required'); return x; }
  function unit(x) { finite(x); if (x < 0 || x > 1) fail('coefficient outside 0..1'); return x; }
  function rgb(x) { if (!Number.isInteger(x) || x < 0 || x > 0xffffff) fail('RGB24 required'); return x; }
  function vector(src, size) {
    if (!src || src.length !== size) fail('vector length');
    for (let i = 0; i < size; ++i) if (!Number.isFinite(src[i]) || !Number.isFinite(Math.fround(src[i]))) fail('finite float32 vector required');
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
  function compose(out, pose, parent) {
    const x = pose[3], y = pose[4], z = pose[5], cx = Math.cos(x), sx = Math.sin(x),
      cy = Math.cos(y), sy = Math.sin(y), cz = Math.cos(z), sz = Math.sin(z);
    const a = cz * cy * pose[6], b = (cz * sy * sx - sz * cx) * pose[7], c = (cz * sy * cx + sz * sx) * pose[8],
      d = sz * cy * pose[6], e = (sz * sy * sx + cz * cx) * pose[7], f = (sz * sy * cx - cz * sx) * pose[8],
      g = -sy * pose[6], h = cy * sx * pose[7], j = cy * cx * pose[8];
    if (!parent) {
      out[0] = a; out[1] = b; out[2] = c; out[3] = pose[0];
      out[4] = d; out[5] = e; out[6] = f; out[7] = pose[1];
      out[8] = g; out[9] = h; out[10] = j; out[11] = pose[2];
    } else for (let k = 0; k < 12; k += 4) {
      out[k] = parent[k] * a + parent[k + 1] * d + parent[k + 2] * g;
      out[k + 1] = parent[k] * b + parent[k + 1] * e + parent[k + 2] * h;
      out[k + 2] = parent[k] * c + parent[k + 1] * f + parent[k + 2] * j;
      out[k + 3] = parent[k] * pose[0] + parent[k + 1] * pose[1] + parent[k + 2] * pose[2] + parent[k + 3];
    }
    for (let i = 0; i < 12; ++i) if (!Number.isFinite(out[i])) fail('transform overflow');
  }
  function fallbackShade(normals, albedo, thickness, lighting, material, output) {
    let lx = lighting[0], ly = lighting[1], lz = lighting[2], q = Math.hypot(lx, ly, lz);
    lx /= q; ly /= q; lz /= q;
    let vx = lighting[3], vy = lighting[4], vz = lighting[5]; q = Math.hypot(vx, vy, vz);
    vx /= q; vy /= q; vz /= q;
    let hx = lx + vx, hy = ly + vy, hz = lz + vz; q = Math.hypot(hx, hy, hz);
    if (q) { hx /= q; hy /= q; hz /= q; }
    for (let i = 0; i < albedo.length; ++i) {
      let nx = normals[3 * i], ny = normals[3 * i + 1], nz = normals[3 * i + 2];
      const length = Math.hypot(nx, ny, nz); nx /= length; ny /= length; nz /= length;
      const dot = nx * lx + ny * ly + nz * lz, front = Math.max(0, dot), back = Math.max(0, -dot),
        spec = front > 0 && q ? Math.pow(Math.max(0, nx * hx + ny * hy + nz * hz), material[3]) : 0,
        trans = material[4] * (1 - thickness[i] / 255) * back, base = albedo[i];
      let color = 0;
      for (let c = 0; c < 3; ++c) {
        const max = c === 1 ? 63 : 31, shift = c === 0 ? 11 : c === 1 ? 5 : 0,
          value = ((base >> shift) & max) / max * (material[0] + material[1] * front * lighting[6 + c]) +
            lighting[6 + c] * (material[2] * spec + trans * material[5 + c]);
        color += Math.round(Math.min(1, Math.max(0, value)) * max) * (1 << shift);
      }
      output[i] = color;
    }
  }
  function geometry(def, compact) {
    if (!def) return null;
    if (def.kind !== 'ellipsoid') fail('geometry must be ellipsoid or null');
    const columns = def.columns === undefined ? 6 : def.columns, rows = def.rows === undefined ? 4 : def.rows,
      radii = def.radii;
    if (!Number.isInteger(columns) || columns < 3 || !Number.isInteger(rows) || rows < 2 || columns * rows > 256 ||
        !radii || radii.length !== 3) fail('mesh capacity');
    vector(radii, 3);
    for (let i = 0; i < 3; ++i) if (!(Math.fround(radii[i]) > 0)) fail('positive radii required');
    if (compact) return {kind: 'ellipsoid', radii: new Float32Array(radii), screen: new Float32Array(8),
      normalMatrix: new Float32Array(9), surface: new Uint16Array(5), texture: new Float32Array(9)};
    const n = rows * columns, local = new Float32Array(n * 12), world = new Float32Array(n * 12),
      points = new Float32Array(n * 8), baseNormals = new Float32Array(n * 3), normals = new Int16Array(n * 3),
      albedo = new Uint16Array(n), colors = new Uint16Array(n), thickness = new Uint8Array(n),
      visible = new Uint8Array(n), uv = new Float32Array(n * 2), shape = new Array(n);
    for (let row = 0, face = 0; row < rows; ++row) for (let col = 0; col < columns; ++col, ++face) {
      for (let corner = 0; corner < 4; ++corner) {
        const theta = PI * (row + (corner > 1 ? 1 : 0)) / rows,
          phi = TAU * (col + (corner === 1 || corner === 2 ? 1 : 0)) / columns, p = face * 12 + corner * 3;
        local[p] = radii[0] * Math.sin(theta) * Math.cos(phi);
        local[p + 1] = -radii[1] * Math.cos(theta);
        local[p + 2] = radii[2] * Math.sin(theta) * Math.sin(phi);
      }
      const theta = PI * (row + 0.5) / rows, phi = TAU * (col + 0.5) / columns;
      baseNormals[3 * face] = Math.sin(theta) * Math.cos(phi) / radii[0];
      baseNormals[3 * face + 1] = -Math.cos(theta) / radii[1];
      baseNormals[3 * face + 2] = Math.sin(theta) * Math.sin(phi) / radii[2];
      uv[2 * face] = Math.atan2(Math.cos(phi), Math.sin(phi)) / TAU + 0.5;
      uv[2 * face + 1] = -Math.cos(theta) * 0.5 + 0.5;
      shape[face] = [face * 4, 4];
    }
    return {n, local, world, points, baseNormals, normals, albedo, colors, thickness, visible, uv, shape};
  }
  function scene(options) {
    options = options || {};
    const backend = options.backend || (typeof pocket !== 'undefined' ? pocket.shapes : null), objects = [],
      lightState = new Float32Array([-0.5, -0.4, 0.8, 1, 1, 1]),
      cameraState = new Float32Array([0, 0, 400, 0, 0, 0, 1, 120, 75]),
      right = new Float32Array(3), down = new Float32Array(3), forward = new Float32Array(3),
      counts = {geometryUpdates: 0, normalUpdates: 0, projectionUpdates: 0, albedoUpdates: 0,
        shadeUpdates: 0, shadeSamples: 0, analyticUpdates: 0, renderedFaces: 0, renderedPatches: 0, copiedBytes: 0,
        typedBytes: lightState.byteLength + cameraState.byteLength + right.byteLength + down.byteLength + forward.byteLength};
    let lightVersion = 1, cameraVersion = 1, evaluatedLight = 0, evaluatedCamera = 0, evaluated = false, disposed = false;
    function alive() { if (disposed) fail('disposed scene'); }
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
      if (change) ++lightVersion;
    }
    function camera(patch) {
      alive(); let change = false;
      if (patch.position) vector(patch.position, 3);
      if (patch.target) vector(patch.target, 3);
      if (patch.center) vector(patch.center, 2);
      if (patch.zoom !== undefined && (!(Math.fround(finite(patch.zoom)) > 0) || !Number.isFinite(Math.fround(patch.zoom)))) fail('positive float32 zoom');
      if (patch.position) change = vec(cameraState, 0, patch.position, 3) || change;
      if (patch.target) change = vec(cameraState, 3, patch.target, 3) || change;
      if (patch.zoom !== undefined) {
        const v = Math.fround(finite(patch.zoom)); if (!(v > 0)) fail('positive zoom');
        if (v !== cameraState[6]) { cameraState[6] = v; change = true; }
      }
      if (patch.center) change = vec(cameraState, 7, patch.center, 2) || change;
      if (change) ++cameraVersion;
    }
    function add(def) {
      alive();
      if (!def || typeof def.name !== 'string' || !def.name || objects.some(x => x.name === def.name)) fail('unique object name');
      const parent = def.parent === undefined || def.parent === null ? null : def.parent._node;
      if (def.parent !== undefined && def.parent !== null && (!parent || !objects.includes(parent))) fail('parent must be a handle in this scene');
      const analytic = !!(def.geometry && backend && backend.ellipsoid);
      const node = {name: def.name, parent, pose: new Float32Array([0, 0, 0, 0, 0, 0, 1, 1, 1]),
        matrix: new Float32Array(12), mesh: analytic ? null : geometry(def.geometry), analytic: analytic ? geometry(def.geometry, true) : null,
        materialData: new Float32Array([0.25, 0.75, 0, 32, 0, 1, 1, 1]),
        lighting: new Float32Array(9), color: 0xffffff, thickness: 0.5, opacity: 1, edgeSoftness: 0.04, pattern: null,
        textureData: new Float32Array([0, 0, 1, 1, 0]), enabled: true, worldEnabled: true,
        transformVersion: 1, evaluatedTransform: 0, worldVersion: 0, parentVersion: -1,
        albedoVersion: 1, evaluatedAlbedo: 0, shadingVersion: 1, evaluatedShading: 0};
      function place(patch) {
        alive(); let changed = false;
        if (patch.position) vector(patch.position, 3);
        if (patch.rotation) vector(patch.rotation, 3);
        if (patch.scale) {
          vector(patch.scale, 3);
          for (let i = 0; i < 3; ++i) if (Math.fround(patch.scale[i]) === 0) fail('nonzero finite scale');
        }
        if (patch.position) changed = vec(node.pose, 0, patch.position, 3) || changed;
        if (patch.rotation) changed = vec(node.pose, 3, patch.rotation, 3) || changed;
        if (patch.scale) changed = vec(node.pose, 6, patch.scale, 3) || changed;
        if (changed) ++node.transformVersion;
      }
      function material(patch) {
        alive();
        if (patch.color !== undefined) rgb(patch.color);
        if (patch.transmissionColor !== undefined) rgb(patch.transmissionColor);
        for (const key of ['ambient', 'diffuse', 'specular', 'roughness', 'transmission', 'thickness', 'opacity', 'edgeSoftness']) if (patch[key] !== undefined) unit(patch[key]);
        if (patch.color !== undefined && rgb(patch.color) !== node.color) { node.color = patch.color; ++node.albedoVersion; }
        let changed = false;
        changed = coefficient(node.materialData, 0, patch, 'ambient') || changed;
        changed = coefficient(node.materialData, 1, patch, 'diffuse') || changed;
        changed = coefficient(node.materialData, 2, patch, 'specular') || changed;
        changed = coefficient(node.materialData, 4, patch, 'transmission') || changed;
        if (patch.roughness !== undefined) {
          const r = unit(patch.roughness), exponent = Math.fround(1 + 127 * (1 - r) * (1 - r));
          if (exponent !== node.materialData[3]) { node.materialData[3] = exponent; changed = true; }
        }
        if (patch.transmissionColor !== undefined) {
          const c = rgb(patch.transmissionColor);
          for (let i = 0; i < 3; ++i) {
            const v = Math.fround(((c >> ((2 - i) * 8)) & 255) / 255);
            if (v !== node.materialData[5 + i]) { node.materialData[5 + i] = v; changed = true; }
          }
        }
        if (patch.thickness !== undefined && unit(patch.thickness) !== node.thickness) { node.thickness = patch.thickness; changed = true; }
        if (patch.opacity !== undefined && patch.opacity !== node.opacity) { node.opacity = patch.opacity; changed = true; }
        if (patch.edgeSoftness !== undefined && patch.edgeSoftness !== node.edgeSoftness) { node.edgeSoftness = patch.edgeSoftness; changed = true; }
        if (changed) ++node.shadingVersion;
      }
      function texture(patch) {
        alive(); let changed = false;
        if (patch.offset) vector(patch.offset, 2);
        if (patch.scale) vector(patch.scale, 2);
        if (patch.rotation !== undefined && !Number.isFinite(Math.fround(finite(patch.rotation)))) fail('finite float32 texture rotation');
        if (patch.offset) changed = vec(node.textureData, 0, patch.offset, 2) || changed;
        if (patch.scale) changed = vec(node.textureData, 2, patch.scale, 2) || changed;
        if (patch.rotation !== undefined) {
          const v = Math.fround(finite(patch.rotation)); if (v !== node.textureData[4]) { node.textureData[4] = v; changed = true; }
        }
        if (changed) ++node.albedoVersion;
      }
      function visible(value) { alive(); if (typeof value !== 'boolean') fail('visible boolean'); node.enabled = value; }
      const handle = {place, material, texture, visible, _node: node};
      if (def.position || def.rotation || def.scale) place(def);
      if (def.material) material(def.material);
      if (def.pattern) {
        const p = def.pattern;
        if (p.kind !== 'band' && p.kind !== 'checker' && p.kind !== 'image') fail('pattern kind');
        if (p.kind === 'image') {
          if (analytic) fail('image pattern requires explicit mesh backend; analytic supports procedural patterns');
          if (!Number.isInteger(p.width) || !Number.isInteger(p.height) || p.width < 1 || p.height < 1 ||
              !(p.pixels instanceof Uint16Array) || p.pixels.length !== p.width * p.height) fail('image pattern');
          node.pattern = {kind: p.kind, width: p.width, height: p.height, pixels: p.pixels};
        } else {
          node.pattern = {kind: p.kind, axis: p.axis || 'v', start: p.start === undefined ? 0.5 : unit(p.start),
            end: p.end === undefined ? 1 : unit(p.end), color: rgb(p.color), repeats: p.repeats === undefined ? 4 : finite(p.repeats)};
          if ((node.pattern.axis !== 'u' && node.pattern.axis !== 'v') || node.pattern.start > node.pattern.end || node.pattern.repeats <= 0) fail('pattern range');
        }
      }
      if (def.textureTransform) texture(def.textureTransform);
      if (def.visible !== undefined) visible(def.visible);
      objects.push(node);
      counts.typedBytes += node.pose.byteLength + node.matrix.byteLength + node.materialData.byteLength + node.lighting.byteLength + node.textureData.byteLength;
      if (node.analytic) for (const key of ['radii', 'screen', 'normalMatrix', 'surface', 'texture']) counts.typedBytes += node.analytic[key].byteLength;
      if (node.mesh) for (const key of ['local', 'world', 'points', 'baseNormals', 'normals', 'albedo', 'colors', 'thickness', 'visible', 'uv']) counts.typedBytes += node.mesh[key].byteLength;
      return handle;
    }
    function evaluateWork(timeMs) {
      alive(); finite(timeMs);
      const lightChanged = evaluatedLight !== lightVersion, cameraChanged = evaluatedCamera !== cameraVersion;
      let fx = cameraState[0] - cameraState[3], fy = cameraState[1] - cameraState[4], fz = cameraState[2] - cameraState[5], distance = Math.hypot(fx, fy, fz);
      if (!(distance > 0)) fail('camera position equals target');
      fx /= distance; fy /= distance; fz /= distance;
      forward[0] = fx; forward[1] = fy; forward[2] = fz;
      let rx = fz, rz = -fx, length = Math.hypot(rx, rz);
      if (length < 1e-6) { rx = 1; rz = 0; length = 1; }
      rx /= length; rz /= length; right[0] = rx; right[1] = 0; right[2] = rz;
      down[0] = fy * rz; down[1] = fz * rx - fx * rz; down[2] = -fy * rx;
      for (let i = 0; i < objects.length; ++i) {
        const node = objects[i], parent = node.parent, m = node.matrix, mesh = node.mesh,
          geometryChanged = node.evaluatedTransform !== node.transformVersion || (parent && node.parentVersion !== parent.worldVersion);
        node.worldEnabled = node.enabled && (!parent || parent.worldEnabled);
        if (geometryChanged) {
          compose(m, node.pose, parent ? parent.matrix : null);
          node.evaluatedTransform = node.transformVersion; node.parentVersion = parent ? parent.worldVersion : 0; ++node.worldVersion;
          ++counts.geometryUpdates;
        }
        if (node.analytic) {
          const patch = node.analytic;
          if (geometryChanged) {
            const r = patch.radii, a = m[0] * r[0], b = m[1] * r[1], c = m[2] * r[2],
              d = m[4] * r[0], e = m[5] * r[1], f = m[6] * r[2], g = m[8] * r[0], h = m[9] * r[1], j = m[10] * r[2],
              determinant = a * (e * j - f * h) - b * (d * j - f * g) + c * (d * h - e * g), n = patch.normalMatrix;
            if (!Number.isFinite(determinant) || Math.abs(determinant) < 1e-12) fail('singular transform');
            n[0] = (e * j - f * h) / determinant; n[1] = (f * g - d * j) / determinant; n[2] = (d * h - e * g) / determinant;
            n[3] = (c * h - b * j) / determinant; n[4] = (a * j - c * g) / determinant; n[5] = (b * g - a * h) / determinant;
            n[6] = (b * f - c * e) / determinant; n[7] = (c * d - a * f) / determinant; n[8] = (a * e - b * d) / determinant;
            ++counts.normalUpdates;
          }
          if (geometryChanged || cameraChanged) {
            const r = patch.radii, s = patch.screen, zoom = cameraState[6];
            for (let k = 0; k < 3; ++k) {
              s[k] = zoom * (right[0] * m[k] + right[1] * m[4 + k] + right[2] * m[8 + k]) * r[k];
              s[4 + k] = zoom * (down[0] * m[k] + down[1] * m[4 + k] + down[2] * m[8 + k]) * r[k];
            }
            const x = m[3] - cameraState[3], y = m[7] - cameraState[4], z = m[11] - cameraState[5];
            s[3] = cameraState[7] + zoom * (right[0] * x + right[1] * y + right[2] * z);
            s[7] = cameraState[8] + zoom * (down[0] * x + down[1] * y + down[2] * z);
            ++counts.projectionUpdates;
          }
          const albedoChanged = node.evaluatedAlbedo !== node.albedoVersion;
          if (albedoChanged || node.evaluatedShading !== node.shadingVersion || geometryChanged) {
            const surface = patch.surface, t = patch.texture, uv = node.textureData, cs = Math.cos(uv[4]), sn = Math.sin(uv[4]), pattern = node.pattern;
            surface[0] = pack(node.color); surface[1] = pattern ? pack(pattern.color) : surface[0];
            surface[2] = Math.round(node.opacity * 255);
            surface[3] = Math.round(Math.min(1, node.thickness * Math.hypot(m[2], m[6], m[10])) * 255);
            surface[4] = !pattern ? 0 : pattern.kind === 'checker' ? 3 : pattern.axis === 'u' ? 1 : 2;
            t[0] = cs * uv[2]; t[1] = -sn * uv[3]; t[2] = 0.5 + uv[0] - (t[0] + t[1]) * 0.5;
            t[3] = sn * uv[2]; t[4] = cs * uv[3]; t[5] = 0.5 + uv[1] - (t[3] + t[4]) * 0.5;
            if (pattern && pattern.kind === 'checker') for (let k = 0; k < 6; ++k) t[k] *= pattern.repeats;
            t[6] = node.edgeSoftness; t[7] = pattern ? pattern.start : 0; t[8] = pattern ? pattern.end : 1;
            if (albedoChanged) ++counts.albedoUpdates;
          }
          if (geometryChanged || cameraChanged || lightChanged || albedoChanged || node.evaluatedShading !== node.shadingVersion) {
            for (let k = 0; k < 3; ++k) { node.lighting[k] = lightState[k]; node.lighting[6 + k] = lightState[3 + k]; }
            // Orthographic rays share one world view direction.
            node.lighting[3] = fx; node.lighting[4] = fy; node.lighting[5] = fz;
            ++counts.shadeUpdates; ++counts.analyticUpdates;
          }
          node.evaluatedAlbedo = node.albedoVersion; node.evaluatedShading = node.shadingVersion;
          continue;
        }
        if (!mesh) continue;
        if (geometryChanged) {
          // Inverse transpose preserves normals under nonuniform parent scale.
          const a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], j = m[10],
            det = a * (e * j - f * h) - b * (d * j - f * g) + c * (d * h - e * g);
          if (!Number.isFinite(det) || Math.abs(det) < 1e-12) fail('singular transform');
          for (let p = 0; p < mesh.local.length; p += 3) {
            const x = mesh.local[p], y = mesh.local[p + 1], z = mesh.local[p + 2];
            mesh.world[p] = a * x + b * y + c * z + m[3];
            mesh.world[p + 1] = d * x + e * y + f * z + m[7];
            mesh.world[p + 2] = g * x + h * y + j * z + m[11];
            if (!Number.isFinite(mesh.world[p]) || !Number.isFinite(mesh.world[p + 1]) || !Number.isFinite(mesh.world[p + 2])) fail('geometry overflow');
          }
          for (let p = 0; p < mesh.n; ++p) {
            const x = mesh.baseNormals[3 * p], y = mesh.baseNormals[3 * p + 1], z = mesh.baseNormals[3 * p + 2],
              nx = ((e * j - f * h) * x + (f * g - d * j) * y + (d * h - e * g) * z) / det,
              ny = ((c * h - b * j) * x + (a * j - c * g) * y + (b * g - a * h) * z) / det,
              nz = ((b * f - c * e) * x + (c * d - a * f) * y + (a * e - b * d) * z) / det,
              n = Math.hypot(nx, ny, nz);
            mesh.normals[3 * p] = Math.round(nx / n * 32767); mesh.normals[3 * p + 1] = Math.round(ny / n * 32767); mesh.normals[3 * p + 2] = Math.round(nz / n * 32767);
          }
          ++counts.normalUpdates;
        }
        const albedoChanged = node.evaluatedAlbedo !== node.albedoVersion;
        if (albedoChanged) {
          const t = node.textureData, cs = Math.cos(t[4]), sn = Math.sin(t[4]), pat = node.pattern, base = pack(node.color);
          for (let p = 0; p < mesh.n; ++p) {
            const x = (mesh.uv[2 * p] - 0.5) * t[2], y = (mesh.uv[2 * p + 1] - 0.5) * t[3];
            let u = cs * x - sn * y + 0.5 + t[0], v = sn * x + cs * y + 0.5 + t[1], color = base;
            u -= Math.floor(u); v -= Math.floor(v);
            if (pat) {
              if (pat.kind === 'image') color = pat.pixels[Math.min(pat.height - 1, Math.floor(v * pat.height)) * pat.width + Math.min(pat.width - 1, Math.floor(u * pat.width))];
              else if (pat.kind === 'checker') { if ((Math.floor(u * pat.repeats) + Math.floor(v * pat.repeats)) & 1) color = pack(pat.color); }
              else { const q = pat.axis === 'u' ? u : v; if (q >= pat.start && q <= pat.end) color = pack(pat.color); }
            }
            mesh.albedo[p] = color;
          }
          node.evaluatedAlbedo = node.albedoVersion; ++counts.albedoUpdates;
        }
        if (geometryChanged || cameraChanged) {
          for (let p = 0; p < mesh.world.length / 3; ++p) {
            const x = mesh.world[3 * p] - cameraState[3], y = mesh.world[3 * p + 1] - cameraState[4], z = mesh.world[3 * p + 2] - cameraState[5], w = distance - x * fx - y * fy - z * fz;
            mesh.points[2 * p] = w > 0 ? cameraState[7] + (x * right[0] + y * right[1] + z * right[2]) * cameraState[6] : NaN;
            mesh.points[2 * p + 1] = w > 0 ? cameraState[8] + (x * down[0] + y * down[1] + z * down[2]) * cameraState[6] : NaN;
          }
          ++counts.projectionUpdates;
        }
        if (geometryChanged || cameraChanged || lightChanged || albedoChanged || node.evaluatedShading !== node.shadingVersion) {
          for (let k = 0; k < 3; ++k) { node.lighting[k] = lightState[k]; node.lighting[6 + k] = lightState[3 + k]; }
          node.lighting[3] = cameraState[0] - m[3]; node.lighting[4] = cameraState[1] - m[7]; node.lighting[5] = cameraState[2] - m[11];
          const v = Math.hypot(node.lighting[3], node.lighting[4], node.lighting[5]);
          if (!(v > 0)) fail('camera at object center');
          for (let k = 3; k < 6; ++k) node.lighting[k] /= v;
          const depthScale = Math.hypot(m[2], m[6], m[10]);
          for (let p = 0; p < mesh.n; ++p) {
            mesh.thickness[p] = Math.round(Math.min(1, node.thickness * depthScale * Math.sin(mesh.uv[2 * p + 1] * PI)) * 255);
            mesh.visible[p] = mesh.normals[3 * p] * node.lighting[3] + mesh.normals[3 * p + 1] * node.lighting[4] + mesh.normals[3 * p + 2] * node.lighting[5] > 0 ? 1 : 0;
          }
          if (backend && backend.shade) backend.shade(mesh.normals, mesh.albedo, mesh.thickness, node.lighting, node.materialData, mesh.colors);
          else fallbackShade(mesh.normals, mesh.albedo, mesh.thickness, node.lighting, node.materialData, mesh.colors);
          node.evaluatedShading = node.shadingVersion; ++counts.shadeUpdates; counts.shadeSamples += mesh.n;
        }
      }
      evaluatedCamera = cameraVersion; evaluatedLight = lightVersion; evaluated = true;
    }
    function evaluate(timeMs) {
      try { evaluateWork(timeMs); }
      catch (error) {
        // Never render partially reevaluated packets. A later valid input can
        // recover, while the app keeps its previously committed image.
        evaluated = false; evaluatedCamera = 0; evaluatedLight = 0;
        for (let i = 0; i < objects.length; ++i) {
          objects[i].evaluatedTransform = 0; objects[i].evaluatedAlbedo = 0; objects[i].evaluatedShading = 0;
        }
        throw error;
      }
    }
    function visit(visitor) {
      alive(); if (!evaluated) fail('evaluate before render');
      for (let i = 0; i < objects.length; ++i) if (objects[i].worldEnabled && (objects[i].mesh || objects[i].analytic)) visitor(objects[i].mesh || objects[i].analytic, objects[i].name);
    }
    const drawOptions = {step: 1};
    function render() {
      alive(); if (!evaluated) fail('evaluate before render');
      if (!backend || (!backend.fill && !backend.ellipsoid)) fail('shapes backend required');
      counts.renderedFaces = 0; counts.renderedPatches = 0;
      for (let i = 0; i < objects.length; ++i) {
        const node = objects[i], mesh = node.mesh;
        if (node.analytic && node.worldEnabled) {
          const patch = node.analytic;
          backend.ellipsoid(patch.screen, patch.normalMatrix, node.lighting, node.materialData, patch.surface, patch.texture);
          ++counts.renderedPatches; continue;
        }
        if (!mesh || !node.worldEnabled) continue;
        for (let p = 0; p < mesh.n; ++p) if (mesh.visible[p]) {
          backend.fill(mesh.points, mesh.shape[p], undefined, mesh.colors[p], drawOptions); ++counts.renderedFaces;
        }
      }
    }
    function stats(out) { alive(); for (const key in counts) out[key] = counts[key]; out.objects = objects.length; }
    function dispose() { if (!disposed) { objects.length = 0; disposed = true; } }
    light(options.light || {direction: [-0.5, -0.4, 0.8]}); if (options.camera) camera(options.camera);
    return {add, light, camera, evaluate, render, visit, stats, dispose};
  }
  return {scene, pack, shadeReference: fallbackShade};
})();
if (typeof module !== 'undefined') module.exports = Character;
