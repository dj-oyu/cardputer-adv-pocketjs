"""Verify all public fixed source dependencies with the merged broker importer."""

import io
import json
import zipfile
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from cardputer_mcp.config import BuildProject, Settings, SourceRepository
from cardputer_mcp.source_snapshot import SourceSnapshots, download, file_receipt

ROOT = Path(__file__).resolve().parents[1]
REPOSITORIES = [
    "boschsensortec/BMI270_SensorAPI",
    "lieff/minimp3",
    "xiph/opus",
]


def main() -> None:
    commit = os.environ["SOURCE_COMMIT"]
    checked_out = subprocess.check_output(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True,
    ).strip()
    assert commit == checked_out, "source contract must use the exact reviewed firmware head"
    expected_tree = subprocess.check_output(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD^{tree}"], text=True,
    ).strip()
    expected = {}
    for path in ("third_party/bmi270", "third_party/minimp3", "third_party/opus"):
        entry = subprocess.check_output(
            ["git", "-C", str(ROOT), "ls-tree", "HEAD", "--", path], text=True,
        ).split()
        assert entry[0:2] == ["160000", "commit"]
        expected[path] = entry[2]
    root = Path(tempfile.mkdtemp(prefix="source-contract-"))
    try:
        profile = root / "profile.ps1"
        profile.write_text("source CI only; never executed")
        settings = Settings(
            state_dir=root / "state",
            build_projects={
                "cardputer": BuildProject(
                    project_dir=root / "worktree",
                    trusted_profile=profile,
                    build_root=root / "output",
                    source_repository=SourceRepository(
                        github_repository="dj-oyu/cardputer-adv-pocketjs",
                        cache_root=root / "cache",
                        submodule_repositories=REPOSITORIES,
                    ),
                )
            },
        )
        def transport(url, deadline, live, limit):
            data = download(url, deadline, live, limit)
            for path, repository in zip(
                ("third_party/bmi270", "third_party/minimp3", "third_party/opus"), REPOSITORIES
            ):
                if not url.startswith("https://codeload.github.com/" + repository + "/zip/"):
                    continue
                tracked = {}
                for row in subprocess.check_output(
                    ["git", "-C", str(ROOT / path), "ls-tree", "-rz", "HEAD"], text=True,
                ).split("\0"):
                    if not row:
                        continue
                    header, name = row.split("\t", 1)
                    mode, kind, sha = header.split()
                    tracked[name] = (mode, sha)
                archived = {}
                with zipfile.ZipFile(io.BytesIO(data)) as archive:
                    for item in archive.infolist():
                        if item.is_dir():
                            continue
                        name = item.filename.split("/", 1)[1]
                        mode = "100755" if (item.external_attr >> 16) & 0o111 else "100644"
                        archived[name] = (mode, file_receipt(archive.read(item), mode)["git_sha1"])
                missing = sorted(set(tracked) - set(archived))
                changed = sorted(name for name in archived if tracked.get(name) != archived[name])
                print(json.dumps({
                    "archive_repository": repository,
                    "missing_paths": missing[:12], "missing_count": len(missing),
                    "changed_paths": changed[:12], "changed_count": len(changed),
                }), flush=True)
            return data

        sources = SourceSnapshots(settings, transport)
        plan = sources.prepare("cardputer", commit, "ci-owner") | {"job_id": "all-public-sources"}
        result = sources.fetch(plan, lambda: True, lambda _: None)
        source, metadata = sources.resolve("cardputer", result["source_receipt"], "ci-owner")
        modules = {item["path"]: item["commit"] for item in metadata["submodules"]}
        assert modules == expected
        assert metadata["tree_sha1"] == expected_tree
        for required in (
            "third_party/bmi270/bmi2.c", "third_party/bmi270/bmi270.c",
            "third_party/minimp3/minimp3.h", "third_party/opus/include/opus.h",
        ):
            assert (source / required).is_file(), required
        assert not (root / "worktree").exists()
        assert not (source / ".cache").exists()
        print(json.dumps(result | {"sdk_build": False, "usb": False}, sort_keys=True))
    finally:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
