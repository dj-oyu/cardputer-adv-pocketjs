# Isolated ESP32-S3 / QEMU generated-skin diagnostic

Base app contract: public 31013b0e, mesh0,6vertices,2joints,rest/child_y6 only.
This project does not alter HOME, main firmware registration or default chunks.
It links the real `pocket_app_load.c` and generated chunk table; an isolated
registry returns `local.skinasset`. Existing test namespace/admission/peripheral
stubs remain, so success is not a full production session/admission test.

Build owner supplies ESP-IDF/toolchain and a fresh successful `run_generated_app.py`
output directory. `SKIN_ASSET_INPUT` is mandatory and the generator rejects any
receipt/model/arrays other than the fixed public hashes. Configure example:

```
idf.py -C tools/skinasset_sim -B build-skinasset \
  -D SKIN_ASSET_INPUT=/absolute/path/to/generated-app-final build
```

The app generator is reused. The diagnostic bootstrap additionally exposes the
existing sample.stats via `skinAssetStats`; no library or app source is edited.
Generated input-hashes.json records fixture and actual embedded bootstrap bytes.

UART logs must contain `SKINASSET_DONE PASS`, 120 frames, oom0, coverage1590/1680,
32400 pixels each, two distinct stable hashes, skinWork480 and successful pending
snapshot after stop. QuickJS heap limit160KiB, four-row strip. Image is scalar:
QEMU cannot execute S3 PIE instructions. No target hardware FPS/PIE claim.
Build/run output and final ELF/flash hashes are recorded by the build owner.
No AF_UNIX or real device required; use the established serial-file/stdout capture.
