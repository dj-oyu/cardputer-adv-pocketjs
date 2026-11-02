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
        # prog: the Shiki programs, built into the firmware; no chunk loads it.
        if len(words) != 2 or words[0] in ('app', 'option', 'prog') or act_only and words[0] in VIEW:
            continue
        out.append(ROOT / 'apps/kasane' / words[1])
    # procMegademo (the host tests' form) is not an app chunk: the device
    # does not need it (megademo_host.js).
    out.append(ROOT / 'tools/kasane_contract/megademo_host.js')
    return out


def chunk_arg(act_only=False):
    """The C harnesses' first argument: the files, comma-separated."""
    return ','.join(str(p) for p in chunk_files(act_only))


def rom_table(out, plans=None):
    """The float VM's built-in plan table written to OUT, a .c file the host
    harnesses link; they hand it to pocket_proc_rom_plans(), as
    main/app_session.c does. PLANS: a baseline's rom-text templates (an app
    from before the move to Shiki, megademo_plans.js); without them the
    table is empty. Needs Node."""
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    texts = ['kasane=text:' + str(plans)] if plans else []
    subprocess.run(['node', str(ROOT / 'tools/kasane_ir/emit_rom_plans.mjs'), str(out), *texts],
                   check=True, stdout=subprocess.DEVNULL)
    return out


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
