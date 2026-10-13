"""Initialize minimp3 at the parent repository's Gitlink; never follows a branch."""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SUBMODULE = "third_party/minimp3"


def prepare() -> None:
    # Git's checked-in mode-160000 entry is the sole revision authority.
    entry = subprocess.run(
        ["git", "-C", str(ROOT), "ls-tree", "HEAD", "--", SUBMODULE],
        check=True, capture_output=True, text=True,
    ).stdout.strip().split()
    if len(entry) != 4 or entry[0:2] != ["160000", "commit"] or entry[3] != SUBMODULE:
        raise SystemExit("minimp3 Gitlink is missing; use a checkout with its submodule")
    subprocess.run(
        ["git", "-C", str(ROOT), "submodule", "update", "--init", "--checkout", "--", SUBMODULE],
        check=True,
    )
    observed = subprocess.run(
        ["git", "-C", str(ROOT / SUBMODULE), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    if observed != entry[2]:
        raise SystemExit("minimp3 checkout does not match the parent Gitlink")
    print("Prepared minimp3", observed)


if __name__ == "__main__":
    prepare()
