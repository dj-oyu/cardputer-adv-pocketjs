"""Bytes of JS_CallInternal per opcode region (static), from a firmware ELF and
the quickjs.c it was built from (docs/vm/p1-iram-placement.md §2.2).

    python tools/r5sim/case_sizes.py ELF quickjs.c XTENSA_BIN_DIR

Every instruction of JS_CallInternal is mapped with addr2line -i to its
outermost quickjs.c line, and that line to the CASE(OP_x) / OP_CMP(...) /
in-function label whose span contains it. Systematic errors: consecutive
CASE labels all go to the last one, cross-jumped exception tails land in one
region, and inlined helpers count as their handler's bytes (which is what an
IRAM budget wants).
"""
import re, subprocess, sys, collections, bisect
elf, src, B = sys.argv[1:4]
nm = subprocess.run([B+"/xtensa-esp-elf-nm.exe", "-S", elf], capture_output=True, text=True).stdout
start = size = None
for l in nm.splitlines():
    f = l.split()
    if len(f) == 4 and f[3] == "JS_CallInternal":
        start, size = int(f[0], 16), int(f[1], 16)
dis = subprocess.run([B+"/xtensa-esp-elf-objdump.exe", "-d", "--start-address=%#x" % start,
                      "--stop-address=%#x" % (start + size), elf], capture_output=True, text=True).stdout
ins = []
for l in dis.splitlines():
    m = re.match(r"\s*([0-9a-f]+):	([0-9a-f]+)\s", l)
    if m:
        ins.append((int(m.group(1), 16), len(m.group(2)) // 2))
addrs = "\n".join("%#x" % a for a, _ in ins)
out = subprocess.run([B+"/xtensa-esp-elf-addr2line.exe", "-i", "-e", elf], input=addrs, capture_output=True, text=True).stdout
# with -i, each address yields 1+ lines (innermost first); group by reading one address at a time is
# ambiguous, so rerun per-address in batches using -a to get markers.
out = subprocess.run([B+"/xtensa-esp-elf-addr2line.exe", "-a", "-i", "-e", elf], input=addrs, capture_output=True, text=True).stdout
lines = {}
cur = None
for l in out.splitlines():
    if l.strip().startswith("0x"):
        cur = int(l.strip(), 16); lines[cur] = []
    else:
        lines[cur].append(l)
# region table: CASE(OP_x) lines and C labels inside JS_CallInternal
txt = open(src, encoding="utf-8", errors="replace").read().splitlines()
fn0 = [i for i, t in enumerate(txt) if re.match(r"static JSValue JS_CallInternal\(", t)][-1] + 1
fn1 = next(i for i in range(fn0, len(txt)) if txt[i].startswith("}")) + 1
marks = []
for i in range(fn0, fn1):
    t = txt[i - 1]
    m = re.search(r"CASE\((OP_\w+)\)", t) or re.match(r"\s*OP_CMP(?:_NUM)?\((OP_\w+)", t)
    if m: marks.append((i, m.group(1))); continue
    m = re.match(r"\s*([a-z_][a-z0-9_]*):\s*$", t) or re.match(r"^([a-z_][a-z0-9_]*):", t)
    if m and m.group(1) not in ("default",): marks.append((i, "label:" + m.group(1)))
mk = [m[0] for m in marks]
def region(line):
    k = bisect.bisect_right(mk, line) - 1
    return marks[k][1] if k >= 0 else "prologue"
sizes = collections.Counter()
for a, n in ins:
    ls = lines.get(a, [])
    q = [x for x in ls if "quickjs.c:" in x]
    if not q: sizes["?"] += n; continue
    m = re.search(r"quickjs\.c:(\d+)", q[-1])   # outermost quickjs.c frame
    ln = int(m.group(1)) if m else 0
    sizes[region(ln) if fn0 <= ln <= fn1 else "outside:%d" % ln if False else (region(ln) if fn0 <= ln <= fn1 else "inlined-elsewhere")] += n
print("JS_CallInternal", size, "bytes; regions", len(sizes))
for k, v in sizes.most_common():
    print(f"{v:6d} {k}")
