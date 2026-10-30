"""Deterministic host capture planning, PNG output and source provenance."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib

MODELS = ('human', 'fox', 'cloud')
MODES = ('BODY', 'LIGHT', 'CAMERA', 'TEXTURE', 'EXPRESSION', 'STILL')


def ticks(text):
    """Comma lists and inclusive START:STOP[:STEP], preserving requested order."""
    result = []
    for item in text.split(','):
        parts = item.split(':')
        if len(parts) == 1:
            value = int(item)
            if value < 0 or value > 1000000000:
                raise ValueError('tick must be in 0..1000000000')
            result.append(value)
        elif len(parts) in (2, 3):
            start, stop = map(int, parts[:2])
            step = int(parts[2]) if len(parts) == 3 else (1 if stop >= start else -1)
            if not (0 <= start <= 1000000000 and 0 <= stop <= 1000000000):
                raise ValueError('range endpoints must be in 0..1000000000')
            if not step or (stop - start) * step < 0:
                raise ValueError('range step must approach its inclusive stop')
            sequence = range(start, stop + (1 if step > 0 else -1), step)
            if len(result) + len(sequence) > 1000:
                raise ValueError('at most 1000 ticks per mode')
            result.extend(sequence)
        else:
            raise ValueError('use ticks such as 0,82,90 or 60:90:3')
        if len(result) > 1000:
            raise ValueError('at most 1000 ticks per mode')
    if not result or any(t < 0 or t > 1000000000 for t in result):
        raise ValueError('tick must be an integer in 0..1000000000')
    return result


def prepare(folder, selected_ticks=None, models=MODELS):
    folder.mkdir(parents=True, exist_ok=True)
    records = []
    for model, name in enumerate(models):
        for mode, default_tick in ((5, 0), (0, 82), (1, 90), (2, 90)):
            for tick in selected_ticks if selected_ticks is not None else [default_tick]:
                i = len(records)
                stem = f'HOST_SIMULATION_{i:05d}_{name}_{MODES[mode]}_tick{tick}'
                records.append(dict(sequence=i, model=name, model_index=model,
                                    mode=MODES[mode], mode_index=mode, tick=tick,
                                    last_submitted_tick=tick, last_submitted_boundary='H.commit; not hardware ACK',
                                    time_ms=tick * 1000 / 30, breathing=False,
                                    ppm=stem + '.ppm', png=stem + '.png'))
    # A failed rerun must not attach previous pixels to this run's provenance.
    for record in records:
        for kind in ('ppm', 'png'):
            (folder / record[kind]).unlink(missing_ok=True)
    plan = folder / 'capture-plan.tsv'
    plan.write_text(''.join(f"{r['model_index']} {r['mode_index']} {r['tick']} {r['ppm']}\n"
                            for r in records), encoding='ascii')
    return plan, records


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git(root, *args):
    try:
        return subprocess.check_output(['git', *args], cwd=root, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        # Windows worktree gitdir is intentionally retained; WSL git cannot read it.
        winroot = subprocess.check_output(['wslpath', '-w', str(root)], text=True).strip()
        return subprocess.check_output(['git.exe', '-C', winroot, *args], stderr=subprocess.DEVNULL)


def provenance(root, binary, flags):
    paths = set()
    for directory in ('apps/character', 'apps/lib', 'main', 'tools/hostshim', 'components/pocketjs_guest/include', 'components/quickjs-ng/quickjs-ng',
                      'apps/kasane', 'apps/cloud'):
        paths.update(p for p in (root / directory).rglob('*') if p.suffix in ('.c', '.h', '.inc', '.js', '.txt'))
    for name in ('tools/kasane_contract/test_character_app_host.c',
                 'tools/kasane_contract/test_megademo_app_host.c',
                 'tools/kasane_contract/run_character_app_host.py',
                 'tools/kasane_contract/run_megademo_app_host.py',
                 'tools/character/capture_character.py', 'tools/build_kasane_test.sh',
                 'tools/kasane_contract/megademo_chunks.py', 'tools/make_font.py'):
        paths.add(root / name)
    result = dict(source_sha256={str(p.relative_to(root)).replace('\\', '/'): sha(p.read_bytes())
                                 for p in sorted(paths)}, binary_sha256=sha(binary.read_bytes()),
                  build_flags=flags)
    try:
        result.update(head=git(root, 'rev-parse', 'HEAD').decode().strip(),
                      dirty=bool(git(root, 'status', '--porcelain').strip()),
                      git_diff_sha256=sha(git(root, 'diff', 'HEAD', '--binary')))
    except (OSError, subprocess.CalledProcessError) as error:
        result.update(head=None, dirty=None, git_error=str(error))
    return result


def finish(folder, records, source, passed, log):
    header = b'P6\n240 135\n255\n'
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    for record in records:
        ppm = folder / record['ppm']
        if not ppm.exists():
            record['missing'] = True
            continue
        data = ppm.read_bytes()
        if not data.startswith(header) or len(data) != len(header) + 240 * 135 * 3:
            raise ValueError(f'invalid capture PPM: {ppm}')
        rgb = data[len(header):]
        scan = b''.join(b'\0' + rgb[y * 720:(y + 1) * 720] for y in range(135))
        png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 135, 8, 2, 0, 0, 0))
               + chunk(b'tEXt', b'Description\0HOST SIMULATION; not a device capture')
               + chunk(b'IDAT', zlib.compress(scan, 9)) + chunk(b'IEND', b''))
        (folder / record['png']).write_bytes(png)
        record.update(rgb_sha256=sha(rgb), ppm_sha256=sha(data), png_sha256=sha(png))
    manifest = dict(kind='HOST SIMULATION', device_capture=False, width=240, height=135,
                    clock='app-owned frame tick; time_ms=tick*1000/30; no sleeping',
                    breathing=False, native_checks_passed=passed, provenance=source,
                    log='capture.log', captures=records)
    (folder / 'capture.log').write_text(log, encoding='utf-8')
    (folder / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return folder / 'manifest.json'
