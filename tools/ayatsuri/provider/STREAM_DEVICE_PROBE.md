# Synthetic streaming device probe (default OFF)

This diagnostic runs the actual `ayatsuri_stream_candidate` API for 32 frames
using a public three-fan fixture. It is **not the recovered model**, a material
validation, a DMA test, or evidence of 20 FPS for that model. The ordinary build
contains no probe invocation or probe sources.

## Enabling and identification

Use the authorized broker to build/flash the same reviewed source head with
variant `ay_stream_probe` (`sdkconfig.variant.ay_stream_probe`). The default-OFF
`CONFIG_AYATSURI_STREAM_DEVICE_PROBE` selects the separate streaming candidate.
Do not invoke a local firmware build or device flasher as a substitute.

The probe runs once on the UI owner, immediately after `vm_wake_bind` and before
`HOME_READY` and the normal event loop. This deliberately delays that diagnostic
boot while the bounded test runs; it does not replace the UI task or register a
guest/model. It yields one tick after every completed frame. No source/model
assets, preferences, NVS, filesystem contents, saved demos or persistent data are
modified. The synthetic fixture is immutable compiled data in the image.

Serial identification is `BEGIN id=AY_STREAM_SYNTH_V1 synthetic=true`, with an
ELF SHA-256 prefix to correlate against the broker's build artifact. The prefix
is not a Git revision: record the source commit, variant, build ID/artifact hash,
and observed prefix together. `RESULT` reports PASS/FAIL; `END` explicitly keeps
`runtime_ready=false model_fps=unmeasured lcd_dma=not_exercised`.

## Measurements and checks

Each frame calls the actual streaming producer and rasterizes the copied pending
snapshot into an eight-row CPU band. It commits only within probe-owned buffers.
It then injects a provider failure on the middle mesh and verifies the prior
ready descriptor and packet bytes are unchanged. After all frames, it disposes
the candidate, overwrites its arena, and verifies ready raster output is still
identical. No LCD, board strip, DMA descriptor, display ACK, or global procedural
frame is borrowed or modified.

Logs report geometry call min/max/sum microseconds, CPU raster microseconds,
frame/provider/rollback counts, arena/workspace/snapshot capacities, internal
8-bit heap free bytes and largest contiguous block, and task stack high-water
marks before/after. Every eighth frame logs heap and stack observations. Timing
excludes the deliberate per-frame yield and progress logging; it includes the
synthetic provider's trigonometry. These are CPU workload timings, not displayed
frame rate. HWM is reported in the SDK's native return units and is a lifetime
low-water mark, including startup/logging frames; it is not an isolated probe
stack measurement. Use target `.su` evidence and the SDK convention alongside it.

Two bounded internal-memory allocations hold the arena and workspace. Allocation
or evaluation failure logs FAIL, frees probe-owned allocations, and returns to
the normal UI startup. No assertion or reboot is used for diagnostic failure.
Reported free/largest values include other task activity and allocator behavior;
a before/after difference alone is not a leak diagnosis.

## Host reproduction

The host test runs the **same portable probe core and fixture**, not a rewritten
streaming implementation. Its clock is deliberately mocked; host time is not
reported as target time.

```sh
CC=cc python tools/ayatsuri/provider/test_stream_device_probe.py --out /tmp/stream-device-host
CC=cc python tools/ayatsuri/provider/test_stream_device_probe.py --out /tmp/stream-device-asan --sanitize
```

Both O0 and O2 run automatically. Use a new output directory. Host checks include
undersized/aliased storage rejection, 32 frames, 160 provider calls, 32 rollbacks,
and arena-independent ready rendering. Observed host capacities: arena 1,960
bytes, workspace 30,144 bytes, snapshot peak 1,464 bytes; final synthetic pixel
hash `b0d19ffd`. These are not physical ESP32 allocation/placement measurements.
The public fixture can be regenerated with:

```sh
node tools/ayatsuri/provider/emit_stream_device_fixture.cjs main/ayatsuri/ayatsuri_stream_probe_fixture.h
```

## Recovery and remaining gates

The diagnostic runs once per boot, so reset under the diagnostic image repeats
it. To remove it, use the broker to restore the prior known-good normal firmware
(or build the same head with this option OFF); do not erase persistent data.
If the diagnostic image cannot reach `HOME_READY`, stop relying on it and restore
that known-good artifact through the broker. The normal variant does not compile
the core/fixture/device wrapper and has no additional boot work.

Physical LCD/DMA completion remains deliberately unexercised. Sending synthetic
pixels safely requires a separately reviewed display-owner acquisition, drain,
submission, final-ACK and screen-restoration path. Calling a drain with no probe
submission would not establish that gate. The full-model geometry, materials,
peak memory, physical DMA lifetime and target-model 20 FPS gates remain separate.
