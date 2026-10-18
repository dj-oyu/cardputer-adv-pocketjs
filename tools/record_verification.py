"""Bind verification artifacts to the actual checked-out source, never a label."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def command(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()


def verify_build(build, head):
    description = json.loads((build / "project_description.json").read_text())
    if (Path(description["project_path"]).resolve() != ROOT.resolve()
            or description["project_version"] != head[:12]):
        raise ValueError("build source/version does not match the exact verification head")
    image = (build / "cardputer_pocketjs.bin").read_bytes()
    # ESP32-S3 app image: 24-byte image header, 8-byte first segment header,
    # then IDF 6.0.1's esp_app_desc_t. Validate its magic before reading fields.
    if len(image) < 288 or image[0] != 0xE9 or struct.unpack_from("<I", image, 32)[0] != 0xABCD5432:
        raise ValueError("invalid application descriptor")
    if image[48:80].split(b"\0")[0] != head[:12].encode():
        raise ValueError("BIN contains another source version")
    if image[144:176].split(b"\0")[0] != b"v6.0.1":
        raise ValueError("ESP-IDF 6.0.1 required in the actual BIN")
    elf_hash = hashlib.sha256((build / "cardputer_pocketjs.elf").read_bytes()).digest()
    if image[176:208] != elf_hash:
        raise ValueError("BIN and ELF do not belong to the same build")
    return description, elf_hash.hex()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    head = command("git", "rev-parse", "HEAD")
    expected = os.environ.get("SOURCE_HEAD", head)
    if head != expected or command("git", "status", "--porcelain", "--untracked-files=no"):
        raise SystemExit("verification requires the exact clean source head")
    report = dict(source_commit=head, source_tree=command("git", "rev-parse", "HEAD^{tree}"),
                  python=platform.python_version(), node=command("node", "--version"),
                  hardware_tested=False)
    report["submodule_gitlinks"] = command("git", "ls-tree", "HEAD", "third_party/")
    if args.build:
        build = args.build.resolve()
        description, elf_hash = verify_build(build, head)
        report["idf_version"] = "v6.0.1"
        report["embedded_elf_sha256"] = elf_hash
        report["project_version"] = description["project_version"]
        names = ["cardputer_pocketjs.bin", "cardputer_pocketjs.elf", "cardputer_pocketjs.map",
                 "sdkconfig", "project_description.json", "bootloader/bootloader.bin",
                 "partition_table/partition-table.bin"]
        report["artifacts"] = {name: dict(bytes=(build / name).stat().st_size,
            sha256=hashlib.sha256((build / name).read_bytes()).hexdigest()) for name in names}
        (build / "SHA256SUMS").write_text("".join(
            f"{item['sha256']}  {name}\n" for name, item in report["artifacts"].items()))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
