"""Compile every initialized source dependency on host without SDK or device."""

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATHS = ("third_party/bmi270", "third_party/minimp3", "third_party/opus")


def configure(source: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["cmake", "-S", str(source), "-B", str(output)],
        capture_output=True, text=True,
    )


def main() -> None:
    for path in PATHS:
        entry = subprocess.check_output(
            ["git", "-C", str(ROOT), "ls-tree", "HEAD", "--", path], text=True,
        ).split()
        assert entry[0:2] == ["160000", "commit"]
        observed = subprocess.check_output(
            ["git", "-C", str(ROOT / path), "rev-parse", "HEAD"], text=True,
        ).strip()
        assert observed == entry[2], "checkout must match each parent Gitlink"
    assert not (ROOT / ".cache").exists(), "contract requires a clean source checkout"
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        harness = root / "host"
        harness.mkdir()
        cmake = """
cmake_minimum_required(VERSION 3.22)
project(source_dependencies_contract C)
macro(idf_component_register)
    cmake_parse_arguments(ARGS "" "" "SRCS;INCLUDE_DIRS;PRIV_INCLUDE_DIRS" ${ARGN})
    set(component_sources "")
    foreach(src IN LISTS ARGS_SRCS)
        if(IS_ABSOLUTE "${src}")
            list(APPEND component_sources "${src}")
        else()
            list(APPEND component_sources "${COMPONENT_SOURCE_DIR}/${src}")
        endif()
    endforeach()
    add_library(${COMPONENT_NAME} STATIC ${component_sources})
    target_include_directories(${COMPONENT_NAME}
        PUBLIC ${ARGS_INCLUDE_DIRS} PRIVATE ${ARGS_PRIV_INCLUDE_DIRS})
    set(COMPONENT_LIB ${COMPONENT_NAME})
endmacro()
set(COMPONENT_NAME codec_minimp3)
set(COMPONENT_SOURCE_DIR "@ROOT@/components/minimp3")
include("@ROOT@/components/minimp3/CMakeLists.txt")
set(COMPONENT_NAME codec_opus)
set(COMPONENT_SOURCE_DIR "@ROOT@/components/opus")
include("@ROOT@/components/opus/CMakeLists.txt")
add_library(bmi270 STATIC "@ROOT@/third_party/bmi270/bmi2.c" "@ROOT@/third_party/bmi270/bmi270.c")
target_include_directories(bmi270 PUBLIC "@ROOT@/third_party/bmi270")
add_executable(dependency_smoke smoke.c)
target_link_libraries(dependency_smoke PRIVATE codec_minimp3 codec_opus bmi270 m)
"""
        (harness / "CMakeLists.txt").write_text(cmake.replace("@ROOT@", ROOT.as_posix()))
        (harness / "smoke.c").write_text("""
#include "minimp3.h"
#include "opus.h"
#include "bmi270.h"
int main(void) {
    mp3dec_t decoder;
    mp3dec_init(&decoder);
    int error = 0;
    OpusDecoder *opus = opus_decoder_create(16000, 1, &error);
    if (!opus || error != OPUS_OK) return 1;
    opus_decoder_destroy(opus);
    return sizeof(struct bmi2_dev) ? 0 : 1;
}
""")
        result = configure(harness, root / "build")
        assert result.returncode == 0, result.stdout + result.stderr
        subprocess.run(["cmake", "--build", str(root / "build"), "--parallel", "2"], check=True)
        subprocess.run([str(root / "build/dependency_smoke")], check=True)
        for name, message in (
            ("minimp3", "minimp3 Git submodule is missing"),
            ("opus", "libopus Git submodule is missing"),
            ("main", "BMI270 Git submodule is missing"),
        ):
            missing = root / ("missing-" + name)
            component = "main" if name == "main" else "components/" + name
            (missing / component).mkdir(parents=True)
            (missing / component / "CMakeLists.txt").write_text(
                (ROOT / component / "CMakeLists.txt").read_text()
            )
            (missing / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.22)\nproject(missing_contract C)\n'
                + 'include("' + component + '/CMakeLists.txt")\n'
            )
            result = configure(missing, root / ("missing-build-" + name))
            assert result.returncode != 0 and message in result.stderr
    print("All fixed Gitlinks verified; codecs/sensor compiled; missing sources rejected")


if __name__ == "__main__":
    main()
