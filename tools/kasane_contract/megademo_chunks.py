"""MEGADEMO's chunk files, in apps/kasane/chunks.txt's order.

The app is an entry (apps/kasane/proc_megademo.js) that pocket.app.load()s
these on the device. The host tests evaluate the same files directly, one
JS_Eval each in one realm, which is what those loads do. The view chunks
need pocket.kasane; a test without it takes act_only=True.
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VIEW = ('view', 'set', 'loop')


def chunk_files(act_only=False):
    out = []
    for raw in (ROOT / 'apps/kasane/chunks.txt').read_text(encoding='utf-8').splitlines():
        words = raw.split('#', 1)[0].split()
        if len(words) != 2 or words[0] == 'app' or act_only and words[0] in VIEW:
            continue
        out.append(ROOT / 'apps/kasane' / words[1])
    # procMegademo (the host tests' form) is not an app chunk: the device
    # does not need it (megademo_host.js).
    out.append(ROOT / 'tools/kasane_contract/megademo_host.js')
    return out


def chunk_arg(act_only=False):
    """The C harnesses' first argument: the files, comma-separated."""
    return ','.join(str(p) for p in chunk_files(act_only))


def rom_table(out):
    """MEGADEMO's built-in plan table (the rom-text templates, as the firmware
    build compiles them: tools/make_app_chunks.py) written to OUT, a .c file
    the host harnesses link; they hand it to pocket_proc_rom_plans(), as
    main/app_session.c does. Needs Node."""
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['node', str(ROOT / 'tools/kasane_ir/emit_rom_plans.mjs'), str(out),
                    'kasane=text:' + str(ROOT / 'apps/kasane/megademo_plans.js')],
                   check=True, stdout=subprocess.DEVNULL)
    return out
