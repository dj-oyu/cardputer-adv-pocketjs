"""Dump the two generated latin DCFA atlases as raw bytes for the Rust harness.

The blobs are not committed: they come from tools/make_font.py, the same
function main/CMakeLists.txt calls at configure time, so the harness cannot
drift from the fonts the firmware actually loads.
"""
import importlib.util
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("make_font", root / "tools" / "make_font.py")
make_font = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_font)

out = pathlib.Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
# app_session.c loads exactly these two, in this order: slot 0 then slot 1.
(out / "font_small.bin").write_bytes(bytes(make_font.atlas(1, 0)))
(out / "font_large.bin").write_bytes(bytes(make_font.atlas(2, 1)))
