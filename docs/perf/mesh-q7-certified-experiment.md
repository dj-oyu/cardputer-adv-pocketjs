# Default-OFF Q7 certified scalar experiment

This opt-in renderer changes coverage to Q7 and colors to Q16 affine increments. It is not image-identical to the original float renderer. Its depth/mask optimization certifies original F32 depth bits and original mask sign, falling back to original equations near ambiguous boundaries. No PIE leaf, private model or asset is included.

## Build selection

Default: `KASANE_MESH_Q7_CERTIFIED_PROTOTYPE=OFF` (no added source, global state or behavior).

Opt-in requires `KASANE_MESH_PREPARED_PROTOTYPE=ON` and `KASANE_MESH_EDGE4_PROTOTYPE=OFF`. CMake rejects incompatible combinations. Do not change default sdkconfig or existing benchmark fixture to enable it. The experiment is not a device-performance claim.

Q7, prepared setup and original depth source compile with strict, noncontracting, nonreassociating, nonreciprocal-approximating arithmetic. Compile guards require binary32/binary64 size, mantissa, exponent and evaluation properties. Both certificate preparation and render check nearest rounding and gradual-underflow probes. A failed environment/certificate check uses original depth/mask, not an unchecked approximation. No floating-point control register is changed by production code. Arithmetic environment and snapshot must remain stable throughout each call.

For attribute plane P, setup computes outward nonnegative M=sum((abs(a)*239+abs(b)*134+abs(c))*inverse*abs(attribute)). It uses E>=128*DBL_EPSILON*M+1e-250. The original edge/barycentric and regrouped-plane forward errors are conservatively below32*DBL_EPSILON*M+1e-260; slack also covers inward rounding of P±E endpoints. Depth accepts only identical finite, nonsubnormal F32 endpoint bits; mask accepts only an unambiguous sign. Original signed-zero, near-tie and palette-boundary cases otherwise use original expressions. This reasoning assumes strict IEEE behavior and the asserted240x135 coordinate domain; compilation alone does not prove target runtime/libc/FPU assumptions.

## Ownership and fallback

Q7 owns a count-sized copy of the original retained triangle data plus integer/color/certificate setup, bounded to64 faces. Candidate and committed snapshots follow the existing frame ACK swap. Failed publication, backdrop repair and image retry retain the corresponding original lifetime; no new ACK/token protocol is introduced.

On candidate size change, only unsubmitted candidate Q7 storage is freed before replacement allocation, preventing a third Q7 snapshot. Committed storage is never mutated. Failed Q7 allocation/preparation keeps the original prepared or packet fallback. Successful Q7 preparation frees its now-duplicated prepared candidate. Any unsafe snapped geometry makes the Q7 snapshot use exact original geometry/color for the whole frame. More than64 faces falls back without dropping faces.

Owner scratch is lazily allocated once as1920 bytes, reused by the serialized renderer and freed at reset. It is not a DMA loan. Statistics are native fixed storage. Memory reporting includes live Q7 capacities, scratch and metadata. Temporary retained preparation can coexist with the two Q7 slots; packet, VM/model, task stack, allocator and display costs remain additional. Target ABI and peak memory must be measured, not inferred from host sizes.

## Host verification commands

- `python tools/kasane_contract/run_mesh_q7.py --plain`
- `python tools/kasane_contract/run_mesh_q7.py` (ASan/UBSan; LSan disabled for host compatibility)
- `python tools/kasane_contract/run_mesh_q7_lifecycle.py` (generic product QJS lifecycle/OOM/repair/retry plus existing Gouraud/proc suites)

Numeric tests compare the certificate backend byte-for-byte against frozen scalar Q7 for RGB, depth, owner and partial failure outputs. Original float rendering remains a separate oracle with explicit Q7 differences. Tests cover rounding-midpoint/F32-adjacent depths, signed zero/subnormals/extremes, cancellation, masks, overflow, arbitrary spans and preparation under DAZ followed by normal rendering. The DAZ test is host-SSE-specific; target environment behavior remains a separate gate.

Lifecycle tests cover allocation failures in prepared/scratch/Q7 setup, committed-byte immutability, count64/65 fallback, backdrop repair, blocked pending mutations, image IO retry, ACK swap, count-sized replacement and reset. Existing default tests are not weakened to accept approximate pixels. The opt-in lifecycle fixture uses exact flat-color geometry; approximate numerical error reporting is in the independent numeric suite.

## S3 full-build handoff gate

Use the same base toolchain/configuration for default OFF and explicit ON, with separate build directories. Verify emitted flags for all three arithmetic TUs, newlib fenv/libm resolution, no unwanted PIE references, firmware/map size, `.su` stack reports and target structure sizes. Confirm default OFF omits Q7 definitions/symbols. Recheck dependency rejection for prepared OFF/edge4 ON. Preserve normal application entry and assets. Build success does not authorize flashing, monitoring the device or publication, and is not evidence of runtime20fps or image acceptance.
