"""Build the actual renderer completion boundary with delayed copied transfers."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="kasane-dma-render-") as temp:
    for flags in [["-O2", "-fstrict-aliasing"], ["-g", "-fsanitize=address,undefined"]]:
        exe = Path(temp) / "test"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", *flags, "-I", str(root / "main/ui/kasane"),
                        "-I", str(root / "tools/kasane_contract"),
                        *[str(root / f) for f in ["main/ui/kasane/ksn_core.c",
                          "main/ui/kasane/ksn_render.c", "main/ui/kasane/ksn_blend_pie.c",
                          "tools/kasane_contract/test_dma_overlap.c"]],
                        "-lm", "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
