# Experimental indexed visibility kernel

This standalone host C11 experiment implements opaque triangle segment queries
and nearest-ray queries. It is not connected to the firmware build, and makes
no claim about device fit, render quality, or execution speed. No scene assets,
captured pixels, display profiles, generated geometry, or application adapters
are needed by the tests.

## Storage contract

Storage contract v3 owns an exact copy of every input double-precision vertex,
including unused vertices and distinct vertex IDs with identical coordinates.
Each persistent triangle stores three uint16 vertex indices and its original
uint16 ordinal. Validation occurs before narrowing. Construction retains only three double-precision center coordinates per
triangle in temporary storage. Bounds are recomputed from owned vertices during
tree construction, and the indexed triangle records are sorted in place. The
temporary centers are released before publication of the scene.

Use `vis_scene_required_bytes_v3(vertices, triangles)` for persistent requested
heap bytes and `vis_scene_build_temporary_bytes_v3(triangles)` for the additional
build-only allocation: `3 * sizeof(double) * triangles` (24 bytes per triangle
on hosts with 8-byte doubles), down from the previous bounds/center/index record
(80 bytes per triangle on those hosts). Persistent storage and traversal are
unchanged. This trades additional construction work for lower temporary memory;
no construction-speed or device-memory-fit claim is made. Equivalence checks
apply only under the tested compiler and floating-point flags. Even the prior
implementation can differ in signed-zero bits across optimization levels;
this is not a cross-optimization or cross-platform bitwise guarantee.
The persistent byte limit does not include temporary
storage, allocator overhead, caller arrays, or retained older scenes. They must
be counted separately for peak-memory admission. Sparse vertex-heavy inputs can
require more memory than representations that retain only triangle positions.

The maximums are 4096 vertices and 2048 triangles. The median tree needs exactly
`C(n) = 1` for `n <= 4`, otherwise `1 + C(floor(n/2)) + C(ceil(n/2))` nodes.
The public API keeps exact cumulative ray/node/triangle budgets. Failures leave
query results unchanged but preserve consumed work. Generation tokens are
caller-issued checks, not proof of input identity. The caller must keep inputs
immutable during each call and use initialized owned scene/binding objects.
The implementation has no global state and is not thread-safe.

## Synthetic tests

Requirements: Python 3, a C11 compiler accepting GCC-style sanitizer flags,
libm, and AddressSanitizer/UndefinedBehaviorSanitizer support.

From this directory, run:

```
python3 run_tests.py --output .test-output
```

The output directory must not exist, so prior evidence cannot be silently
overwritten. Four tests run at O0/O2, both plain and with ASan/UBSan:

- Ownership, generation changes, allocation-failure rollback, source poisoning,
  endpoint and grazing behavior, capacities and cumulative query budgets.
- Known nearest hits, ties, invalid inputs and a 1000-ray independent oracle.
- 1000 synthetic segments compared with an independent long-double oracle.
- All allowed triangle-count capacity formulas, exact-byte admission, unused
  vertex preservation, duplicate-position identity and signed-zero copying.
  Tracked allocations verify actual requested build peak, persistent bytes after
  publication and zero remaining bytes after release.

Fixtures are small procedural or explicitly synthetic triangles. Tests do not
load scene files, contact services, or produce rendered images. Logs, executable
hashes and source hashes are written only in the selected output directory.
Assertions are required; do not add `-DNDEBUG` or unsafe floating-point flags.

LeakSanitizer is enabled by default. On a host whose tracing/runtime environment
prevents it from running, `--disable-leak-sanitizer` records that it was not
tested. The ownership test independently verifies zero live tracked allocations
after its allocation-failure and lifetime cases; that is not a LeakSanitizer pass.

Existing firmware interfaces, ownership layouts, and source lists are unchanged.
Integrating this kernel with a renderer or target firmware requires a separate
API/ownership review and new resource, stack and build verification.

## License

MIT, copyright (c) 2026 dj-oyu. The accompanying LICENSE reproduces the repository
license so the standalone directory retains the required notice.
