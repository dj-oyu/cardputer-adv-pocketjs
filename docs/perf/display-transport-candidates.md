# Isolated LCD transport candidates

Common source: `d41aadc8a9de106d8290ae5e398288fa29fb7a6d`.
The device comparison source `93d9fc09` is not replaced. These candidates have
not been flashed, registered, or merged into ordinary main.

The local snapshots are independent: A changes TX storage, B retains two TX
buffers and changes narrow addressing. The current review tree is B only.
A/B must each pass the unchanged complete contract script and the board/owner
checks before a combined candidate is assembled. The workflow pins A to `2fd8e9d8eede67be0cb6a57ad00df84149c5e3e4`. The frozen B product tree is
`c0d09d759d3037a471ac938d5b7f60ce241598f6`; the PR head adds only CI/documentation to B and
is tested as the exact current combined-workflow source. There is no fast/full user choice.

## Evidence available locally

The actual `board.c` translation unit was compiled with the retained ESP-IDF
v6.0.1 (`8c19b156084a0753687347cca1f5355782893533`) and Xtensa GCC
`esp-15.2.0_20251204` compile environment, independently with DMA OFF and ON.
The values below are symbol sizes / sum of board-object `.bss` sections, not a
whole-firmware link result and not measured free or contiguous heap.

| Independent source | shared | TX payload | selector | window bounds | board object .bss |
| --- | ---: | ---: | ---: | ---: | ---: |
| Common base | 3,840 B | 7,680 B | 4 B | 0 B | 12,754 B |
| A: single TX | 3,840 B | 3,840 B | 0 B | 0 B | 8,910 B |
| B: narrow continuation | 3,840 B | 7,680 B | 4 B | 4 B | 12,758 B |

A removes 3,840 bytes of TX storage plus the 4-byte selector. B adds two
16-bit horizontal bounds. Alignment of shared/TX remains 16 bytes. A combined
result is deliberately not claimed before independent full validation.

The panel/ownership model executes the production functions with DMA OFF and
ON, ASan/UBSan and O2/strict aliasing, queued and polling sends, scalar and PIE
swaps. It compares every modeled pixel, verifies packed narrow rows, preserves
the stable `board_strip()` pointer, and covers full/narrow mixing (including
x=0), changed widths, gaps, a last 7-row band, partial frame end, capture scope,
unaligned scalar fallback, command/parameter failures, queue rejection,
timeout, foreign/unreturned descriptors and failed completions. A separate read-only source pass inventoried every native caller. An additional
3,000-operation seeded fault sequence passed in all eight A/B, DMA OFF/ON and
ASan/O2 configurations without changing the reviewed product sources. Existing
renderer ownership, final ACK, cancellation/exit, source guards, pressure and
session-dispatch tests are retained.

For a 32-column run of all 17 bands, including the final 7 rows:

| Source | CASET/RASET/RAMWR commands | control SPI calls | pixel payload |
| --- | ---: | ---: | ---: |
| A (original window policy) | 51 | 85 | 8,640 B |
| B | 3 | 5 | 8,640 B |

These are model-observed software calls, not a hardware timing measurement.
B keeps the existing narrow threshold and 16-pixel alignment. It extends the
window to the panel bottom and retains it only while x/width/next-row match.
Any command, failure, capture scope or completed owner frame invalidates it.
The ordinary owner source guard permits exactly one new window-end call,
strictly after both present paths return and before any ACK/error branching;
negative tests reject its removal or movement before present.

## Native UI tradeoff and ownership

Both candidates correct the same ownership prerequisite: DMA OFF as well as
ON retains a descriptor/payload until the driver returns that exact descriptor,
and command failures cannot hide an earlier reap error.

A reaps before every TX write. The native two-strip model changes from one copy
while a previous descriptor is borrowed to zero; this deliberately removes the
native UI's copy/DMA overlap. The Kasane submit path already drains first, so
that model has zero overlapping copies with either storage count. Native UI
throughput must be measured separately on hardware. No elapsed-time saving,
OOM fix, largest-block gain or QuickJS headroom gain is inferred from SRAM.

The frame-end hook invalidates addressing only. It never releases a borrow or
acknowledges a transfer. Capture continues to emit complete, unswapped rows.
The native drawing scratch remains allocated because native UI retains its
pointer for a frame.

## M5GFX comparison

M5GFX is comparison material, not a firmware dependency. Inspected commit:
`a8ebd7bdaae2e9bdc2f3c69c25b83a84d819af72`.

- [Panel_ST7789](https://github.com/m5stack/M5GFX/blob/a8ebd7bdaae2e9bdc2f3c69c25b83a84d819af72/src/lgfx/v1/panel/Panel_ST7789.hpp)
  derives its LCD transfer behavior from Panel_LCD.
- [Panel_LCD window cache](https://github.com/m5stack/M5GFX/blob/a8ebd7bdaae2e9bdc2f3c69c25b83a84d819af72/src/lgfx/v1/panel/Panel_LCD.inl#L457-L506)
  avoids unchanged address registers, while still issuing the selected RAM
  command. Its image path can set a window once then write multiple chunks.
  Our candidate continues the existing firmware's RAMWR stream across
  contiguous narrow bands, rather than copying the library's policy verbatim.
- [Bus_SPI pixel path](https://github.com/m5stack/M5GFX/blob/a8ebd7bdaae2e9bdc2f3c69c25b83a84d819af72/src/lgfx/v1/platforms/esp32/Bus_SPI.inl#L840-L868)
  converts into flip buffers for DMA. This illustrates the copy/overlap tradeoff;
  it does not justify handing a live native scratch buffer to DMA without a
  lifetime barrier.

## Remaining gates

The cloud's local apt index and m32 sysroot are absent. An official Debian
fetch failed with a proxy 503; no auth/proxy/network configuration was changed.
Thus the existing full suite has not been claimed as locally passed. Its same
entry point, normal/m32 games, capture, baseline/combined and source/tree/overlay
proofs will run for the isolated snapshots in CI after publication is allowed.
Publication is authorized for one short-lived branch/Draft PR. Independent
complete CI is pending; neither a combined product nor a hardware result is
claimed at this stage.

A combined candidate remains unassembled. Real native timing, pixel/capture
behavior and the independent DMA OFF/ON memory gates remain Windows/device
work. No device action is part of this change.
