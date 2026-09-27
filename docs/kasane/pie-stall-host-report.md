# Compiled PIE stall check (host only)

The **pre-scheduling** `build_api` object `esp-idf/main/CMakeFiles/__idf_main.dir/ui/kasane/ksn_proc_points_pie.c.obj` was disassembled with Espressif's `xtensa-esp32s3-elf-objdump` on 2026-09-27. Its SHA256 was `e74ec3c5053c65b7bb17214d880da9f1a4ae4c5e6fce1e1d2007a94326b99543`. The `block8` symbol contains 31 actual PIE instructions, from `ee.vld.128.ip q0, a4, 0` through `ee.vst.128.ip q4, a3, 0`. The disassembly, rather than the C assembly string, supplies the sequence in this check.

`esp32s3-hw-mcp`'s `analyze_sequence` reports **2 modelled data stalls**: one each for the two adjacent `EE.VMULAS.S16.QACC -> EE.SRCMB.S16.QACC` pairs. Both carry a separate **measured pair anchor of 0.0 stalls** on silicon against Table 1.7-2's 1-cycle prediction. The upstream model total is thus not a measured kernel total.

The MCP analyzer matches Table 1.7-2's abstract operand names (`qu`, `qx`, `qy`, `qs`) literally. It does not map them onto disassembled `q0`–`q4` operands. An operand-aware supplemental check found **12 adjacent physical `q2` load-use pairs**: 2 `EE.VLDBC.16.IP -> EE.MOV.S16.QACC` and 10 `EE.VLDBC.16.IP -> EE.VMULAS.S16.QACC`. Table stages (load definition at stage 2, consumer use at stage 1) predict one stall for each. These 12 are excluded from the upstream model total. They are predictions, not device measurements. The existing local `tools/pie/stalls.py` now recognizes signed QACC forms as QR consumers and reports all 12 (previously 4).

## Reproduce

After compiling `build_api`, with the Xtensa binutils on `PATH`:

```powershell
uv run --no-project --with git+https://github.com/dj-oyu/esp32s3-hw-mcp python tools/kasane_contract/report_pie_stalls.py --json .cache/kasane-host-no-com3/pie-stalls-report.json
python tools/pie/stalls.py main/ui/kasane/ksn_proc_points_pie.c block8
```

Those commands analyze whichever version is compiled. Rebuilding after
the scheduling change yields the scheduled result in the last section;
the SHA256 above identifies the object used for the baseline report.

Set `XTENSA_OBJDUMP` or pass `--objdump` if the disassembler is not on `PATH`. For this offline run, `--with` pointed at the already cached MCP checkout at `C:\Users\core\AppData\Local\uv\cache\git-v0\checkouts\ac7b1d0665b607ea\fb3561e`, and the disassembler was `C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin\xtensa-esp32s3-elf-objdump.exe`. The JSON contains the decoded instructions, full upstream response, and the physical-register supplemental pairs.

Regression checks after changing `tools/pie/stalls.py`:

```text
python tools/pie/stalls.py main/scene/ocean.c ocean_row_pie  # 0 stalls, 40.6 estimated cycles
python tools/pie/stalls.py main/scene/wave.c wave_row_pie    # 0 stalls, 69.6 estimated cycles
```

The check is first-order adjacent-pair data analysis. It does not simulate longer dependence chains, shared multiplier or other resource occupancy, branch effects, instruction/data cache, memory contention, or interrupt/preemption effects. The local `stalls.py` estimate also omits QACC dependencies; its 44.2 cycles for this body is a heuristic floor (`31` issue + `2 x 0.6` store + `12` predicted load-use), not a measured runtime. In particular, adding the upstream 2 QACC predictions would contradict the available measured pair anchors.

## Scheduled full C function

After the host checks in [the scheduling record](proc-pie-stall-options.md),
the schedule was integrated into `block8`. A fresh ESP32-S3 GCC 15.2.0
`-O2` compilation of the complete C function, with the PIE target gate
enabled, produced 31 contiguous PIE instructions. `objdump` confirmed their
order and actual q/address operands. The corrected local `stalls.py` reports
**0 adjacent QR load-use pairs** and a heuristic issue floor of **32.2
cycles per block**. The change from 44.2 to 32.2 is a model prediction,
not a device speedup measurement. COM3 was not accessed.
