# PIE block8 schedule (host verification)

The original `block8` issued twelve
`EE.VLDBC.16.IP` loads of `q2` immediately before an instruction consuming
`q2`: the two `MOV.S16.QACC` and ten `VMULAS.S16.QACC` instructions. The
ESP32-S3 TRM table 1.7-2 places QR load definition in stage 2 and QR use
in stage 1. The project's measured load-use rule says one independent
instruction between producer and consumer removes that specific interlock
([PIE survey §2.2](../perf/pie-simd.md)). The pre-change `build_api` object
disassembled in source order, with no assembler scheduling inserted.

This **host-verified, device-unmeasured schedule** is now used in
`main/ui/kasane/ksn_proc_points_pie.c`. `q5` is a second coefficient register. The
post-increment stream remains exactly `neg_32768, xr, xa, xb, xc, m00,
m01, yr, ya, yb, yc, m10, m11`.

```asm
ee.vld.128.ip q0, %[sx], 0
ee.vld.128.ip q1, %[sy], 0
ee.vldbc.16.ip q3, %[p], 2       // neg_32768
ee.vldbc.16.ip q2, %[p], 2       // xr
ee.vldbc.16.ip q5, %[p], 2       // xa
ee.mov.s16.qacc q2              // xr
ee.vldbc.16.ip q2, %[p], 2       // xb
ee.vmulas.s16.qacc q5, q3       // xa
ee.vldbc.16.ip q5, %[p], 2       // xc
ee.vmulas.s16.qacc q2, q3       // xb
ee.vldbc.16.ip q2, %[p], 2       // m00
ee.vmulas.s16.qacc q5, q3       // xc
ee.vldbc.16.ip q5, %[p], 2       // m01
ee.vmulas.s16.qacc q0, q2       // m00
ee.vmulas.s16.qacc q1, q5       // m01
ee.srcmb.s16.qacc q4, %[shift], 0
ee.vldbc.16.ip q2, %[p], 2       // yr
ee.vst.128.ip q4, %[dx], 0
ee.vldbc.16.ip q5, %[p], 2       // ya
ee.mov.s16.qacc q2              // yr
ee.vldbc.16.ip q2, %[p], 2       // yb
ee.vmulas.s16.qacc q5, q3       // ya
ee.vldbc.16.ip q5, %[p], 2       // yc
ee.vmulas.s16.qacc q2, q3       // yb
ee.vldbc.16.ip q2, %[p], 2       // m10
ee.vmulas.s16.qacc q5, q3       // yc
ee.vldbc.16.ip q5, %[p], 2       // m11
ee.vmulas.s16.qacc q0, q2       // m10
ee.vmulas.s16.qacc q1, q5       // m11
ee.srcmb.s16.qacc q4, %[shift], 0
ee.vst.128.ip q4, %[dy], 0
```

The `yr` load occurs after x extraction, so the subsequent y `MOV`
replaces the modified QACC. Both source vectors are loaded before either
destination store, preserving supported in-place operation. Stores remain
x then y. Each load of `q2`/`q5` now has at least one intervening
instruction before its first consumer. The y `yr` load has an x store and
`ya` load before its `MOV`.

## Host verification (2026-09-27)

I assembled the original and candidate sequences as separate functions in
one temporary Xtensa `.S` file with the project's ESP32-S3 toolchain
(`xtensa-esp32s3-elf-gcc` 15.2.0). `objdump -dr` shows 31 PIE instructions
in each function, in the requested order with the requested q and address
registers. Each function is 100 bytes including `entry`, `movi.n`, and
`retw.n`; the PIE body itself is 93 bytes. In particular, the assembler
accepts `q5` in the coefficient loads and multiplications and does not
reschedule either body.

Running the existing `tools/pie/stalls.py` parser and analyzer on the two
instruction lists reports **12** immediate stage-2 QR load-use pairs for
the original and **0** for the candidate. A separate direct scan of the
assembled instruction order gives the same counts. This is a prediction
about this interlock only; the tool does not model QACC or resources.

A symbolic walk of both instruction lists consumes the same 13 terms in
the same post-increment order and produces identical QACC expressions:
`xr + xa*neg_32768 + xb*neg_32768 + xc*neg_32768 + x*m00 + y*m01`
for x, then the corresponding y expression. Both load source x and y
before the first store and store x before y. `tools/pie/test_proc_points_schedule.py`
also runs the original and candidate through `piesim.py` against the
scalar Q14 reference for 512 deterministic cases, each both out-of-place
and in-place (1,024 input scenarios; both schedules checked), including
full-range random coefficients and signed-int32 translation extremes.
Both tests pass. The simulator gained `EE.MOV.S16.QACC` sign extension
support; the complete PIE host suite now passes all 25 tests.

The host assembly test used fixed Xtensa address registers corresponding
to the inline-assembly operands. The integrated full C function was also
compiled with ESP32-S3 GCC 15.2.0 at `-O2`, with the PIE target gate enabled.
`objdump` shows 31 contiguous PIE instructions in the intended order after
GCC assigned its address registers. The corrected `stalls.py` reports zero
adjacent QR load-use stalls, and the production schedule test matches the
exact verified list. The PIE simulator is a functional model, so device
semantics and timing remain to be checked on hardware.
The C host contract for the point PIE arithmetic model also passes at `-O2`.

This schedule makes **no device-speed claim**. The project's measured
`VMULAS.S16.QACC` to `SRCMB.S16.QACC` pair has zero stalls despite a
one-stall table prediction ([PIE survey §2.4](../perf/pie-simd.md)); no
filler is needed there. QACC timing for other pairs and resource conflicts
are not modelled by `tools/pie/stalls.py`. Confirm PIE semantics and timing
on the device when COM3 is available, then calibrate the dispatch threshold.
