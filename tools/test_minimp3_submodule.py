"""Host compile of the component with a real initialized Git submodule, no SDK."""

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def configure(source: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["cmake", "-S", str(source), "-B", str(output)],
        capture_output=True, text=True,
    )


def main() -> None:
    entry = subprocess.check_output(
        ["git", "-C", str(ROOT), "ls-tree", "HEAD", "--", "third_party/minimp3"],
        text=True,
    ).split()
    assert entry[0:2] == ["160000", "commit"]
    observed = subprocess.check_output(
        ["git", "-C", str(ROOT / "third_party/minimp3"), "rev-parse", "HEAD"], text=True,
    ).strip()
    assert observed == entry[2], "checkout must match the parent Gitlink"
    component = ROOT / "components/minimp3"
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        harness = root / "host"
        harness.mkdir()
        (harness / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.22)\n"
            "project(minimp3_contract C)\n"
            "function(idf_component_register)\n"
            "  cmake_parse_arguments(ARGS \"\" \"\" \"SRCS;INCLUDE_DIRS\" ${ARGN})\n"
            f'  add_library(codec STATIC "{component.as_posix()}/minimp3.c")\n'
            "  target_include_directories(codec PUBLIC ${ARGS_INCLUDE_DIRS})\n"
            "  set(COMPONENT_LIB codec PARENT_SCOPE)\n"
            "endfunction()\n"
            f'include("{component.as_posix()}/CMakeLists.txt")\n'
        )
        result = configure(harness, root / "build")
        assert result.returncode == 0, result.stdout + result.stderr
        subprocess.run(["cmake", "--build", str(root / "build")], check=True)
        # A missing submodule must fail before any compile or implicit fetch.
        missing = root / "missing"
        (missing / "components/minimp3").mkdir(parents=True)
        (missing / "components/minimp3/CMakeLists.txt").write_text(
            (component / "CMakeLists.txt").read_text()
        )
        (missing / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.22)\n"
            "project(missing_contract C)\n"
            'include("components/minimp3/CMakeLists.txt")\n'
        )
        result = configure(missing, root / "missing-build")
        assert result.returncode != 0 and "minimp3 Git submodule is missing" in result.stderr
    print("Fixed Gitlink verified; initialized codec compiled; missing submodule rejected")


if __name__ == "__main__":
    main()
