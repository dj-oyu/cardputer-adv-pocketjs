# Clean checkout dependency inventory

This change moves only minimp3 to a formal Git submodule. The parent Gitlink at
`third_party/minimp3` pins lieff/minimp3 commit
`ea99364f61c14656440e8d77e9c233ccf3124633`. Initialize it before compilation:

```sh
git submodule update --init --checkout -- third_party/minimp3
# Equivalent explicit preparation:
python tools/prepare_minimp3.py
```

The helper reads the parent Gitlink instead of maintaining a separate revision
lock. No `--remote` or branch tracking is used. CMake reads the initialized
submodule directly and never fetches it. For broker source snapshots, the importer
materializes this same Gitlink into this path and records repository/commit/tree
and content hashes; no Git metadata or preparation command is needed at build time.

Other clean-checkout prerequisites found at main
`6a139780b7eeececfaa17cf2ddee576a10a027f5`:

| Input | Status and consumer |
| --- | --- |
| BMI270 | **Untracked fetched dependency:** `.cache/bmi270/bmi2.c`, `bmi270.c`, headers are required by main/CMakeLists.txt. prepare_dependencies.py pins Bosch commit 41129fcfe39c583ee5462d79195741945d51c1fe. Still a clean snapshot build blocker. |
| libopus | **Untracked fetched dependency:** `.cache/codecs/opus-1.6.1` is required by components/opus/CMakeLists.txt. prepare_dependencies.py fetches xiph/opus release tag v1.6.1. Still a clean snapshot build blocker; a tag is not a fixed Gitlink. |
| QuickJS / PocketJS | Tracked under components/quickjs-ng and components/pocketjs_guest. No old PocketJS Rust/UI archives are needed by the current root CMake. |
| dependencies.lock | Tracked but contains historical .cache/PocketJS component entries. Broker builds disable IDF component-manager fetching; this file is not proof that missing codec/sensor sources exist. |
| ESP-IDF / toolchain | External ESP-IDF **6.0.1** plus its Python, CMake, Ninja and Xtensa tools. No SDK is supplied by this repository. |
| Node | External Node 16+ is required for the tracked DERBY @plan generators. No node_modules installation is required by those generators. |
| Font / SFX / scene tables / app chunks | Tracked generators run during configure and write under the chosen CMake build directory; not required as pre-populated source files. |
| sdkconfig / build products / memory log | Generated outputs. Start from tracked sdkconfig.defaults; broker sets SDKCONFIG in its build directory. memory_log now writes in CMAKE_BINARY_DIR. |
| heapprobe bytecode | Optional .cache/compile_peak/derby.bc only for diagnostic heapprobe builds; absence uses the existing no-loader behavior. |

This PR does not fetch or convert BMI270/libopus, regenerate the historical lock,
or claim that the whole firmware compiles from a clean snapshot. Those changes
need their own reviewed dependency decision before a network-free broker build.
No device operation is part of validation.
