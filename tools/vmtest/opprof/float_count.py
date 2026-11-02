"""Float work per turn from an opprof dump (docs/apps/derby-integer.md sec.1):
the (op, type) pairs with a float operand, summed per op, and their total.

    python tools/vmtest/opprof/float_count.py .cache/opprof/straight_side_mid.txt
"""
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
out = subprocess.run([sys.executable, str(HERE / "opprof_ty.py"), sys.argv[1], "--top", "100000"],
                     capture_output=True, text=True, check=True).stdout
pairs = re.findall(r"^  (\S+)\s+(\S+)\s+([\d.]+)\s+[\d.]+%$", out, re.M)
per, total = {}, 0.0
for op, ty, n in pairs:
    if "float" in ty:
        per[op] = per.get(op, 0.0) + float(n)
        total += float(n)
head = out.splitlines()[0]
print(head)
for op, n in sorted(per.items(), key=lambda kv: -kv[1]):
    print(f"  {op:16s} {n:8.1f}/turn")
print(f"float pairs total {total:.1f}/turn")
