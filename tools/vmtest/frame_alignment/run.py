#!/usr/bin/env python3
"""Build the VM and run the real segmented-frame alignment contract."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[3]
QJS = ROOT / "components/quickjs-ng/quickjs-ng"
GUEST = ROOT / "components/pocketjs_guest"
OUT_ROOT = ROOT / ".cache/vm_frame_alignment"


def run(cmd, env):
    print("+ " + " ".join(map(str, cmd)), flush=True)
    subprocess.run(cmd, cwd=ROOT, env=env, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--arch", choices=["native", "m32"], default="native")
    ap.add_argument("--variant", choices=["o2", "asan"], default="o2")
    ap.add_argument("--reloc", action="store_true",
                    help="also run the existing native ASan relocation corpus")
    a = ap.parse_args()
    if a.reloc and (a.arch != "native" or a.variant != "asan"):
        ap.error("--reloc is supported only with --arch native --variant asan")
    inherited = os.environ.get("VMTEST_CFLAGS", "").strip()
    if a.arch == "m32" and not inherited:
        ap.error("m32 needs VMTEST_CFLAGS from tools/vmtest/m32_sysroot.sh")

    out = OUT_ROOT / f"{a.arch}-{a.variant}"
    env = dict(os.environ, VMTEST_OUT=str(out))
    run(["bash", "tools/vmtest/build.sh", a.variant], env)

    cflags = (["-O2", "-g"] if a.variant == "o2" else
              ["-O1", "-g", "-fno-omit-frame-pointer",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"])
    cflags += shlex.split(inherited)
    cmd = [
        "gcc", "-std=gnu11", "-w", *cflags,
        "-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE",
        "-I", str(out / "include"), "-I", str(GUEST / "include"), "-I", str(QJS),
    ]
    if a.arch == "m32":
        cmd.append("-DFA_EXPECT_ALIGN8")
    cmd += [
        "tools/vmtest/frame_alignment/frame_alignment.c",
        *[str(out / f"obj-{a.variant}" / (name + ".o"))
          for name in ["dtoa", "libregexp", "libunicode", "quickjs-libc", "quickjs-vm"]],
        "-lm", "-lpthread", "-ldl", "-o", str(out / "frame-alignment"),
    ]
    run(cmd, env)
    run([str(out / "frame-alignment")], env)
    run(["bash", "tools/vmtest/run.sh", "--variant", a.variant], env)

    if a.reloc:
        reloc_out = OUT_ROOT / "native-asan-reloc"
        reloc_env = dict(os.environ, VMTEST_OUT=str(reloc_out))
        run(["bash", "tools/vmtest/build.sh", "asan-reloc"], reloc_env)
        reloc_env["VMTEST_VMRUN_FLAGS"] = "--force-reloc"
        run(["bash", "tools/vmtest/run.sh", "--variant", "asan-reloc"], reloc_env)


if __name__ == "__main__":
    main()
