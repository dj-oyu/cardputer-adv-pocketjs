"""Explicitly initialize reviewed submodules at the parent Gitlinks."""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATHS = ("third_party/bmi270", "third_party/minimp3", "third_party/opus")


def prepare_submodule(path: str) -> None:
    if path not in PATHS:
        raise ValueError("unregistered preparation path")
    entry = subprocess.run(
        ["git", "-C", str(ROOT), "ls-tree", "HEAD", "--", path],
        check=True, capture_output=True, text=True,
    ).stdout.strip().split()
    if len(entry) != 4 or entry[0:2] != ["160000", "commit"] or entry[3] != path:
        raise SystemExit("required Gitlink is missing; use a checkout with its submodules")
    subprocess.run(
        ["git", "-C", str(ROOT), "submodule", "update", "--init", "--checkout", "--", path],
        check=True,
    )
    observed = subprocess.run(
        ["git", "-C", str(ROOT / path), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    if observed != entry[2]:
        raise SystemExit("submodule checkout does not match the parent Gitlink")
    print("Prepared", path, observed)
