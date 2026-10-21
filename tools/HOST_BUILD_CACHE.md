# Host compiler reuse

Run the existing commands (`bash tools/kasane_contract/run.sh`,
`python3 tools/games/run_derby.py`, `python3 tools/games/run_big_wave.py`, or
`bash tools/build_kasane_test.sh`). There is one complete contract-suite entry
point; CI and local development use the same implementation.

The suite schedules five independent lanes. C tests retain both ASan/UBSan and
O2, both games retain their normal and m32 runs, and every lane must pass. The
QuickJS adapter tests retain their fixture-producing order. ROM capture and
DERBY use one lane because capture builds the same `derby_host/m32` binary.
The ROM cases are generated afresh in that invocation's temporary directory.
C lane temporary outputs are separate from model lane outputs. Logs include
elapsed seconds and exit status per lane; CI uploads each log even on failure.

`tools/host_build.py` compiles each translation unit with the original flags
before linking in the original order. It reuses only objects under
`.cache/host-objects-v1`. The key includes compiler path/content, working
directory, source path, ordered compile flags and compiler environment. This
separates sanitizers, optimization, m32, diagnostic defines and generated include
locations. Every request rescans the current include graph (including system
headers) and hashes its contents, so preserved mtimes and newly shadowing
headers do not bypass invalidation. Objects have integrity hashes. Per-object
locks prevent competing lanes from publishing partial objects. Date/time
macros disable direct object reuse. All executable links, generated assets,
workloads, test executions, and source/tree/overlay proofs run again.

An installed `ccache` is automatically used for actual `-c` invocations with
compiler-content checking, preprocessor mode and no sloppiness. It is optional;
local dependency-checked object reuse also works without it. CI persists only
the checked host object directory, not existing harness `.cache` directories.
The baseline overlay explicitly lists and hashes each shared build helper;
product source remains at the selected baseline commit.

The same helper can be called by another host build frontend using its original
compiler argument list. A broker/MCP build can invoke these existing commands
without server changes, provided it uses the same checkout and can retain the
compiler object directory. No broker or device operation is part of this change.

Firmware builds already use CMake/Ninja's dependency graph. Extending reuse
there requires enabling ESP-IDF's ccache support in the normal `idf.py` command
and retaining only ccache storage, partitioned by IDF/toolchain/configuration.
Do not restore build directories, final images or proof JSON. Default-selection
and explicit-selection builds remain distinct acceptance cases. This PR does
not claim measured firmware cache savings or remove any firmware builds.
