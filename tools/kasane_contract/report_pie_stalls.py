"""Analyze compiled block8 PIE instructions with esp32s3-hw-mcp.

Run after building build_api (host only):
    uv run --no-project --with git+https://github.com/dj-oyu/esp32s3-hw-mcp \
      python tools/kasane_contract/report_pie_stalls.py

For an existing offline checkout, replace the --with argument with its path.
Set XTENSA_OBJDUMP or pass --objdump to choose the Espressif disassembler.
"""

import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from esp32s3_hw_mcp.server import build_server


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OBJECT = (ROOT / "build_api/esp-idf/main/CMakeFiles/__idf_main.dir/"
                  "ui/kasane/ksn_proc_points_pie.c.obj")
LINE = re.compile(r"^\s*([0-9a-f]+):\s*([0-9a-f]+)\s+([a-z0-9.]+)\s*(.*?)\s*$")
QREG = re.compile(r"\bq[0-7]\b")


def disassemble(objdump, obj):
    output = subprocess.check_output(
        [objdump, "-d", "--disassemble=block8", str(obj)], text=True)
    instructions = []
    for line in output.splitlines():
        match = LINE.match(line)
        if match:
            address, encoding, mnemonic, args = match.groups()
            instructions.append({"address": int(address, 16), "encoding": encoding,
                                 "mnemonic": mnemonic, "operands": args,
                                 "registers": QREG.findall(args)})
    pie_positions = [i for i, ins in enumerate(instructions)
                     if ins["mnemonic"].startswith("ee.")]
    if not pie_positions:
        raise RuntimeError("No PIE instructions in compiled block8; check object and symbol")
    body = instructions[pie_positions[0]:pie_positions[-1] + 1]
    if any(not ins["mnemonic"].startswith("ee.") for ins in body):
        raise RuntimeError("Non-PIE instruction inside block8 PIE sequence; "
                           "adjacent-pair analysis would skip a gap")
    return body


def physical_load_use(entries, records):
    """Supplement abstract TRM operand roles with actual q-register aliases.

    This is an adjacent pair check only. It does not model QACC forwarding,
    resources, branches, memory, cache, or longer dependency chains.
    """
    pairs = []
    for index, (producer, consumer) in enumerate(zip(entries, entries[1:])):
        # The upstream call sees a PIE-only stream. Preserve physical
        # adjacency if a later compiler inserts a native Xtensa instruction.
        if consumer["address"] != producer["address"] + len(producer["encoding"]) // 2:
            continue
        p, c = records[index:index + 2]
        if p["status"] != "ok" or c["status"] != "ok":
            continue
        # These kernel instructions each have one physical q destination in
        # operand position zero. TRM's abstract 'qu' def is mapped to it.
        defs = p["operands_def"]
        if not defs or not producer["registers"]:
            continue
        physical_def = producer["registers"][0]
        for item in defs:
            if item["reg"] != "qu":
                continue
            stage = item["stage"]
            if stage is None or physical_def not in consumer["registers"]:
                continue
            # MOV.S16.QACC reads qs; VMULAS.S16.QACC reads qx and qy.
            # For this kernel every q operand of either is an input.
            if consumer["mnemonic"] not in ("ee.mov.s16.qacc", "ee.vmulas.s16.qacc"):
                continue
            use_stage = min((x["stage"] for x in c["operands_use"]
                             if x["reg"] in ("qs", "qx", "qy")), default=None)
            if use_stage is not None and stage > use_stage:
                pairs.append({"from_index": index, "to_index": index + 1,
                              "register": physical_def, "from": producer["mnemonic"],
                              "to": consumer["mnemonic"],
                              "table_stall_cycles": stage - use_stage})
    return pairs


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--object", type=Path, default=DEFAULT_OBJECT)
    parser.add_argument("--objdump", default=os.environ.get("XTENSA_OBJDUMP") or
                        shutil.which("xtensa-esp32s3-elf-objdump"))
    parser.add_argument("--json", type=Path, help="write full evidence JSON")
    args = parser.parse_args()
    if not args.objdump:
        parser.error("pass --objdump or set XTENSA_OBJDUMP")
    entries = disassemble(args.objdump, args.object)
    sequence = [e["mnemonic"] + " " + e["operands"] for e in entries]
    response = await build_server().call_tool(
        "analyze_sequence", {"instructions": sequence, "include_pairs": True})
    if response.is_error:
        raise RuntimeError(str(response.content))
    model = json.loads(response.content[0].text)
    extra = physical_load_use(entries, model["records"])
    report = {"object": str(args.object), "sha256": hashlib.sha256(args.object.read_bytes()).hexdigest(),
              "symbol": "block8", "pie_instruction_count": len(entries),
              "compiled_instructions": entries, "upstream_analyze_sequence": model,
              "physical_q_adjacent_load_use": extra}
    print(f"object SHA256: {report['sha256']}")
    print(f"compiled block8: {len(entries)} PIE instructions")
    print(f"upstream abstract-role data stalls: {model['stall_cycles_total']}")
    print(f"physical q adjacent load-use pairs missed by upstream: {len(extra)}")
    for pair in extra:
        print(f"  {pair['from_index']:2d}->{pair['to_index']:2d} "
              f"{pair['from']} -> {pair['to']} ({pair['register']}): "
              f"table {pair['table_stall_cycles']}")
    for pair in model.get("measured_pairs", []):
        print(f"measured anchor: {pair['from']} -> {pair['to']}: "
              f"table {pair['table_stall_cycles']}, "
              f"silicon {pair['measured_stall_cycles_at_distance_1']}")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    asyncio.run(main())
