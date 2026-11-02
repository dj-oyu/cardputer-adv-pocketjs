"""MEGADEMO's built-in Shiki table for the host harnesses.

The app (apps/kasane/proc_megademo.js and the chunks in apps/kasane/chunks.txt)
registers its programs by name, 'kasane.<name>', from the firmware's built-in
table; the harnesses link the same table, generated here from
apps/kasane/megademo_prog.js.
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def shiki_table(out):
    """MEGADEMO's built-in Shiki table (megademo_prog.js's @shiki functions as
    'kasane.<name>', as the firmware build compiles them) written to OUT, a .c
    file the host harnesses link. Needs Node."""
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['node', str(ROOT / 'tools/kasane_ir/emit_shiki_rom.mjs'), str(out),
                    'kasane=' + str(ROOT / 'apps/kasane/megademo_prog.js')],
                   check=True, stdout=subprocess.DEVNULL)
    return out
