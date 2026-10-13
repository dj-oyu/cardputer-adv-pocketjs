# Clean checkout dependency inventory

All required external source dependencies are formal Git submodules initialized
before configure. The parent repository's mode-160000 Gitlinks are the sole
revision authority; no independent dependency lock or branch tracking is added.

| Path | Upstream | Fixed commit / prior version |
| --- | --- | --- |
| third_party/bmi270 | https://github.com/boschsensortec/BMI270_SensorAPI.git | 41129fcfe39c583ee5462d79195741945d51c1fe, unchanged from prepare_dependencies.py |
| third_party/minimp3 | https://github.com/lieff/minimp3.git | ea99364f61c14656440e8d77e9c233ccf3124633, unchanged from prepare_minimp3.py |
| third_party/opus | https://github.com/xiph/opus.git | 22244de5a79bd1d6d623c32e72bf1954b56235be, existing v1.6.1 annotated tag a5d6c1b6f4e582df97390f9ac5c6e7c51cbffffe |

```sh
git submodule update --init --recursive --checkout
# Equivalent explicit initialization of the required paths:
python tools/prepare_dependencies.py
# Only minimp3:
python tools/prepare_minimp3.py
```

Preparation reads the parent Gitlinks and verifies each resulting HEAD. No
`--remote` or latest branch is used. CMake reads third_party directly and fails
if required sources are absent; it never downloads anything. Existing license
notices in licenses/BMI270.txt, licenses/minimp3.txt and licenses/libopus.txt stay
unchanged, and each upstream submodule retains its own license files.

For broker snapshots, the importer materializes the same fixed Gitlinks without
Git metadata and records repository/commit/tree/archive/content hashes. Register
all three upstream repositories in source_repository.submodule_repositories.
Do not invoke preparation during SDK build.

Other clean-checkout inputs:

| Input | Status and consumer |
| --- | --- |
| QuickJS / PocketJS | Tracked under components/quickjs-ng and components/pocketjs_guest. No historical PocketJS Rust/UI archive is required by current root CMake. |
| dependencies.lock | Tracked historical component-manager metadata still contains old .cache/PocketJS entries. It is not the source dependency authority. Broker builds use IDF_COMPONENT_MANAGER=0; standard builds resolve the current tracked IDF manifests. This PR does not regenerate that metadata. |
| ESP-IDF / tools | External ESP-IDF **6.0.1**, its Python/CMake/Ninja/Xtensa tools; no SDK is bundled. |
| Node | External Node 16+ for tracked DERBY @plan generators. Those generators need no node_modules install. |
| Font / SFX / scene / app chunks | Tracked Python generators use standard-library imports and create outputs under CMAKE_BINARY_DIR; no pre-populated source files are required. |
| sdkconfig / build products / memory log | Generated outputs. Start from tracked sdkconfig.defaults; broker places SDKCONFIG and memory log in its build directory. |
| heapprobe bytecode | Optional .cache/compile_peak/derby.bc for explicit diagnostic heapprobe builds; absence keeps existing no-loader behavior. |

Required BMI270/libopus/minimp3 .cache dependencies are removed. Complete
ESP-IDF cross-compilation and actual hardware acceptance still require the
external SDK and authorized deployment; host contract CI is not device evidence.
No game or device behavior is changed by this dependency relocation.
