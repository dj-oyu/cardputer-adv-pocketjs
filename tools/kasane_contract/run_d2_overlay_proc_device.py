"""Gate two procedural image surfaces over the live FLOWER home overlay."""
import argparse
from pathlib import Path
import re
import time


ORDER = (
    'D2_OVERLAY START surfaces=2 image_nodes=2',
    'D2_OVERLAY COMMIT tick=0 slot=0',
    'D2_OVERLAY: ARMED pending=1 after=3',
    'D2_OVERLAY: FAIL pending=1',
    'D2_OVERLAY: PRESENT result=',
    'D2_OVERLAY: RETRY_OK bytes=',
    'D2_OVERLAY COMMIT tick=1 slot=1',
    'D2_OVERLAY COMMIT tick=2 slot=0',
    'D2_OVERLAY COMMIT tick=3 slot=1',
    'D2_OVERLAY FRAME 8',
)


def check_run(lines: list[str], run: int) -> None:
    joined = '\n'.join(lines)
    cursor = 0
    for marker in ORDER:
        found = joined.find(marker, cursor)
        if found < 0:
            raise RuntimeError(f'run {run}: missing ordered {marker!r}')
        cursor = found + len(marker)
    for fault in ('OVERLAY_STOPPED', 'OVERLAY_REFUSED', 'START_FAILED',
                  'Guru Meditation', 'CORRUPT HEAP'):
        if fault in joined:
            raise RuntimeError(f'run {run}: unexpected {fault}')
    if joined.count('D2_OVERLAY: FAIL pending=1') != 1:
        raise RuntimeError(f'run {run}: LCD fault was not exactly once')
    rows = [(int(a), int(b), int(c), int(d)) for a, b, c, d in re.findall(
        r'D2_OVERLAY: PRESENT result=(\d+) bytes=(\d+) pending=(\d+) flower_mode=(\d+)',
        joined)]
    if not rows or not all(flower == 1 for _, _, _, flower in rows):
        raise RuntimeError(f'run {run}: native FLOWER backdrop not confirmed')
    if len([row for row in rows if row[0] != 0]) != 1:
        raise RuntimeError(f'run {run}: expected one KSN_IO present, got {rows}')
    match = re.search(r'D2_OVERLAY: RETRY_OK bytes=(\d+)', joined)
    if not match or int(match.group(1)) <= 0:
        raise RuntimeError(f'run {run}: LCD repair did not transfer pixels')
    print(f'D2_OVERLAY_RUN_{run} PASS presents={len(rows)} '
          f'retry_bytes={match.group(1)} flower=1')


def live(port_name: str, out: Path) -> None:
    import serial

    lines: list[str] = []
    original = None
    original_background = None
    restore_error = None
    port = serial.Serial(port=None, baudrate=115200, timeout=0.15)
    port.dtr = False
    port.rts = False
    port.port = port_name
    try:
        port.open()
        time.sleep(0.6)
        port.reset_input_buffer()

        def read_line() -> str:
            line = port.readline().decode(errors='replace').strip()
            if line:
                lines.append(line)
            return line

        def await_line(marker: str, seconds: float = 12,
                       since: int | None = None) -> str:
            if since is not None:
                for line in lines[since:]:
                    if marker in line:
                        return line
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                line = read_line()
                if marker in line:
                    return line
                if any(fault in line for fault in ('OVERLAY_REFUSED',
                                                  'Guru Meditation',
                                                  'CORRUPT HEAP')):
                    raise RuntimeError(line)
            raise RuntimeError(f'missing {marker!r}: {lines[-12:]}')

        def key(code: str, marker: str) -> str:
            start = len(lines)
            port.write(code.encode())
            return await_line(marker, since=start)

        def background_choice() -> int:
            key('q', 'HOME_READY')
            time.sleep(0.15)
            key('>', 'CATEGORY 1')
            for _ in range(8):
                key('&', 'SELECT')
            match = re.search(r'choice=(\d+)', key('e', 'OPEN 0 choice='))
            if not match:
                raise RuntimeError('BACKGROUND choice missing')
            value = int(match.group(1))
            key('q', 'HOME_READY')
            return value

        def select_background(value: int) -> None:
            key('q', 'HOME_READY')
            time.sleep(0.15)
            key('>', 'CATEGORY 1')
            for _ in range(8):
                key('&', 'SELECT')
            match = re.search(r'choice=(\d+)', key('e', 'OPEN 0 choice='))
            if not match:
                raise RuntimeError('BACKGROUND choice missing')
            current = int(match.group(1))
            step = 'd' if value > current else '&'
            for index in range(current + (1 if value > current else -1),
                               value + (1 if value > current else -1),
                               1 if value > current else -1):
                key(step, f'CHOICE {index}')
            key('e', f'background={value}')
            key('q', 'HOME_READY')

        def row() -> None:
            key('q', 'HOME_READY')
            time.sleep(0.15)
            key('>', 'CATEGORY 1')
            for _ in range(8):
                key('&', 'SELECT')
            for index in range(1, 5):
                key('d', f'SELECT {index}')

        def choice() -> int:
            row()
            match = re.search(r'choice=(\d+)', key('e', 'OPEN 4 choice='))
            if not match:
                raise RuntimeError('HOME OVERLAY choice missing')
            value = int(match.group(1))
            key('q', 'HOME_READY')
            return value

        def select(value: int) -> None:
            row()
            match = re.search(r'choice=(\d+)', key('e', 'OPEN 4 choice='))
            if not match:
                raise RuntimeError('HOME OVERLAY choice missing')
            current = int(match.group(1))
            step = 'd' if value > current else '&'
            for index in range(current + (1 if value > current else -1),
                               value + (1 if value > current else -1),
                               1 if value > current else -1):
                key(step, f'CHOICE {index}')
            start = len(lines)
            key('e', 'VALUE')
            if value:
                await_line('OVERLAY running state=', since=start)

        original = choice()
        select(0)
        original_background = background_choice()
        if original_background != 3:
            select_background(3)
        for run in (1, 2):
            select(0)
            start = len(lines)
            select(1)
            await_line('D2_OVERLAY FRAME 8', seconds=20, since=start)
            check_run(lines[start:], run)
    finally:
        if original is not None and port.is_open:
            try:
                select(0)
                if original_background is not None and original_background != 3:
                    select_background(original_background)
                if original:
                    select(original)
                else:
                    key('q', 'HOME_READY')
            except Exception as error:
                restore_error = error
        if port.is_open:
            port.close()
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        if restore_error is not None:
            raise RuntimeError('HOME OVERLAY choice restoration failed') from restore_error


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    live(args.port, args.out)


if __name__ == '__main__':
    main()
