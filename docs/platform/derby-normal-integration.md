# DERBY integration with normal defaults (2026-10-19)

This candidate integrates the existing PR7 → PR8 → PR11 → PR13 foundation.
PR7 includes the PR5/6 rendering fixes and the bounded USB console writer;
its device acceptance remains pending. This candidate must stay Draft until
normal-mode device acceptance and its exact-head CI/review are complete.
PR18's OFF experiment containers are retained without enabling them.

The root CMake diagnostic ON presets are removed. Telemetry, stage measure,
fixed hardware seed and finite workload all default OFF in main/CMakeLists.
Only an explicitly enabled finite workload adds the mandatory complete and
incremental diagnostic image proof. The proof continues to reject incorrect
modes, symbols, generated chunks and mismatched BIN/ELF images. Normal source
archives and explicitly enabled finite source archives both retain source
immutability checks. The archive helper uses --fixed-workload to opt in.
No watchdog, session, memory, transport or quality limit is relaxed.

The baseline finite fixture rejects affine/cache/projection rendering
experiments, whose combined fixture work contract is unaccepted. Projection
batching with telemetry consumes the generated telemetry chunks. Its normal
PAN generator retains the existing depth order from the PR7 foundation.

PR14 native clip caching and PR15 runner submission batching remain separate
comparison branches. Their original single-variant device measurements do not
accept the combined normal game. They are not enabled or included here.
PR16/17 device acceptance and simultaneous experiment enablement also remain
separate from OFF code containment. No device operation was performed.

Required device acceptance: normal full demo/race cycle; moving sway and PAN
depth order; WIDE >=2 fps; guest heap/evaluation margin/LOADSTALL stability;
physical LCD appearance; fresh HOME reply after ending DERBY. Render-buffer
captures alone do not attest the LCD. Test the final candidate exact head.
