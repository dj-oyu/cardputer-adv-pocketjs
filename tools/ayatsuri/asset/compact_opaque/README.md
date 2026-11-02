# Experimental compact opaque building blocks

Reusable offline batching and caller-owned numeric packet helpers. These tools
are **not firmware integration**, not a replacement for `Hone.gouraudPatch`, and
not a change to the legacy 1,024-segment quota. No character assets or generated
images are included. Run synthetic-only checks:

```sh
python3 tools/ayatsuri/asset/compact_opaque/run_tests.py
```

## Explicit static batching

`batchStaticSurfaces(surfaces, {scopeBySurface: Map})` accepts bounded static
surfaces with positions, unit normals, oriented triangles, material IDs and
stable IDs. Every source surface needs an explicit scope. Only equal scope and
material may share a packet. Exact position/normal matches can share vertices;
there is no quantization, geometric reduction, material substitution, or
animation inference. Signed zero is distinguished. Output preserves source face
provenance and stays within32 vertices/64 triangles per part.

The caller must choose scopes from its rig/transform/deformation semantics.
Never place independently animated bindings in one scope merely to save memory.
This module deliberately has no GLB importer/AYNM packer dependency: callers may
connect the separately developed asset pipeline without copying it into main.

## Compact owned numeric records

`compact_opaque.h/.c` allocates nothing and accepts caller-owned storage. Appending
an invalid or over-budget record leaves prior bytes/count/size unchanged. Inputs
are borrowed only during append; the output retains no source pointers.

The little-endian record retains exact F32 x/y/view-depth. General vertices also
retain F32 mask and both RGB565 colors (20bytes). Only when all referenced
vertices have the same A=B color may redundant mask/color data be omitted
(12bytes/vertex). Unused vertices are removed and local triangle indices use U8;
the existing32-vertex bound makes that lossless. All triangle ordering is retained.
`co_read` validates counts, indices, finite/ranged coordinates, canonical fields,
padding and byte spans. `co_view` is borrowed from the encoded buffer and must
not outlive or mutate it. Float access uses portable explicit-endian copying.

This format contains no holes, alpha, layer compositing or clipping contract.
Reject unsupported inputs before append; never silently erase those semantics.
Its capacity is a byte count supplied by the caller, **not an assumed physical
RAM ceiling derived from the old segment format**. General varying-color packets
can require more memory; reject transactionally rather than degrade the shader.

## Integration responsibilities intentionally left to the firmware owner

- Obtain evaluated depth through a validated Ayatsuri packet-view helper, not
  duplicated private storage offsets; copy before the instance mutates.
- Preserve JS-facing Hone shader controls and Ayatsuri targets. Old painter APIs
  stay unchanged; a depth backend must be explicit opt-in.
- Own candidate/committed data through failed presentation, retry, ACK and DMA;
  preserve both submit and submittedClipped rollback paths.
- Account candidate, committed, preparation workspace, model arena/cold bytes,
  depth/color bands, allocator overhead and guest baseline. No unaccounted cache.
- Include depth-only mutations in damage identity, or conservatively invalidate.
- Define depth-group boundaries and exact-depth tie policy in the renderer.
- Run product lifecycle/synthetic regressions and formal target build/device
  measurements. These independent helper tests are not those acceptance gates.

No firmware code, memory/security setting, device operation, or public asset is
changed by these files. Tests require only a C11 compiler, Node and Python.
