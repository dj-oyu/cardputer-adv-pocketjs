# ChangeLog

## Unreleased - 2026-11-20

- `quickjs-ng/` is now a submodule of the fork https://github.com/dj-oyu/quickjs instead of a vendored copy. The fork replays the history of every change made here on top of upstream v0.14.0 plus Espressif's formatting; its first pin is byte-identical to the vendored tree it replaces.

- The fork merges upstream v0.18.0 without its arena allocator (9de2921); see docs/vm/upstream-v018.md.

## v0.14.0 - 2026-04-27

- Vendored upstream [quickjs-ng v0.14.0](https://github.com/quickjs-ng/quickjs/tree/v0.14.0) under `quickjs-ng/`, pruned to library sources only.
- Port layer: `port/stub.c` (weak libc symbols), `port/include` (WASI / `poll.h` / `lstat` glue for `quickjs-libc.c` on newlib).
- Example: `examples/quickjs-ng` — `cd examples/quickjs-ng && idf.py set-target <chip> build`.
