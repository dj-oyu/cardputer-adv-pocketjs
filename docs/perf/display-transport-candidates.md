# 通常 main への DMA 再提案（2026-10-27）

#37 head `578d552384c992cb852822d40e2635e7b317d498` から DMA の copied submit/finish、
ACK 前 completion、descriptor 所有権、window 管理、単一 3,840 B TX payload を抽出。
通常 main `45a06e0` 上で `KASANE_DMA_OVERLAP` は既定 OFF。
描画 buffer/port ABI と失敗時 repair 契約を保持する。
単一 TX payload は OFF/native UI にも適用され、従来の native copy/DMA overlap は失われる。
その時間影響は未測定であり、実機受入が必要。3,840 B BSS 差は layout の事実であり guest 空き増加の実測ではない。

DD 診断 hook/列挙と transport fault/counter tests は保存した。
旧 fixed workload/telemetry/測定出力・typed proof/manifest consumer の全体はこの提案に接続しない。
旧診断 ON 構成、定量的な受入値、過去 artifact は元 #37 の固定 SHA に保存する。
以下は元候補の履歴であり、この再提案の測定結果ではない。

# Isolated LCD transport candidates

Common source: `d41aadc8a9de106d8290ae5e398288fa29fb7a6d`.
The device comparison source `93d9fc09` is not replaced. These candidates have
not been flashed, registered, or merged into ordinary main.

A and B were validated independently from that common product base, before
assembling the current combined candidate. Frozen A is
`2fd8e9d8eede67be0cb6a57ad00df84149c5e3e4`; frozen B is
`c0d09d759d3037a471ac938d5b7f60ce241598f6` (two TX buffers).
The independent validation head `1e68c10038d8ccced2f4fb6b84be786f9fdcc2f5`
passed all six workflows, including all three complete source suites in
[run 39611144211](https://github.com/dj-oyu/cardputer-adv-pocketjs/actions/runs/39611144211).
The full workflow took 12:21, all CI 13:16; all three object caches missed.
This is independent validation evidence, not a measurement of the combined candidate.

After that phase the temporary third full-suite source and duplicate board-host
matrix are removed. The normal baseline/current-head complete workflow and all
original acceptance conditions remain. There is no fast/full user choice.

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
| Combined | 3,840 B | 3,840 B | 0 B | 4 B | 8,914 B |

A removes 3,840 bytes of TX storage plus the 4-byte selector. B adds two
16-bit horizontal bounds. Alignment of shared/TX remains 16 bytes. Combined board-object static storage
is 3,840 B smaller than the common base in both DMA configurations. Each of the
eight existing firmware builds also checks actual linked ELF symbols, writing
`board-transport.json` with the exact source and ELF digest. The named-storage
comparison includes shared/TX/selector/window bounds; other symbols are unchanged.

The panel/ownership model executes the production functions with DMA OFF and
ON, ASan/UBSan and O2/strict aliasing, queued and polling sends, scalar and PIE
swaps. It compares every modeled pixel, verifies packed narrow rows, preserves
the stable `board_strip()` pointer, and covers full/narrow mixing (including
x=0), changed widths, gaps, a last 7-row band, partial frame end, capture scope,
unaligned scalar fallback, command/parameter failures, queue rejection,
timeout, foreign/unreturned descriptors and failed completions. A separate read-only source pass inventoried every native caller. An additional
3,000-operation seeded fault sequence passed in all eight A/B, DMA OFF/ON and
ASan/O2 configurations without changing the reviewed product sources. The
combined candidate passed the same ten host groups and four additional
3,000-operation DMA OFF/ON, ASan/O2 sequences. Existing
renderer ownership, final ACK, cancellation/exit, source guards, pressure and
session-dispatch tests are retained.

For a 32-column run of all 17 bands, including the final 7 rows:

| Source | CASET/RASET/RAMWR commands | control SPI calls | pixel payload |
| --- | ---: | ---: | ---: |
| A (original window policy) | 51 | 85 | 8,640 B |
| B and combined | 3 | 5 | 8,640 B |

These are model-observed software calls, not a hardware timing measurement.
B keeps the existing narrow threshold and 16-pixel alignment. It extends the
window to the panel bottom and retains it only while x/width/next-row match.
Any command, transport/control failure, capture scope or completed owner frame
invalidates it. Existing full-width invalid-argument early returns preserve the
position; this does not claim that every rejected API call invalidates it.
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

The combined exact head must pass the same complete CI before acceptance.
Normal/m32 games, ASan/O2, capture, baseline/current source, overlay/tree proofs,
all firmware mode proofs and consumer gates remain required. Local m32 setup is
unavailable; complete-suite success is established by CI, without skipping cases.

Real native timing, pixel/capture behavior and the independent DMA OFF/ON memory
gates remain Windows/device work. Native copy/DMA overlap is lost with one TX;
static savings do not establish largest-block or guest-headroom acceptance.
No device action is part of this change.
