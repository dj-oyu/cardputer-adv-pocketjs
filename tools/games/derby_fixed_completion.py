"""Shared fixed-fixture owner completion; no serial or device operations."""
import re
from derby_fixed_result import parse_trial

CONTRACT = 'fixed-owner-v1'
OWNER = 'DERBY_DETAIL_EXIT'
FATAL = re.compile(r'InternalError:|FRAME RUNAWAY|RUNAWAY|LOADSTALL|Guru Meditation|'
                   r'assert failed|stack overflow|watchdog|\bOOM\b|abort\(\)|'
                   r'hook (?:rejected|threw):|stop hook did not finish|rst:0x|ESP-ROM:', re.I)


def parse_completion(raw, *, detail_enabled):
    """Require END -> owner return -> STOP -> HOME in complete raw capture.

    This checks content only. The collector receipt and build/flash chain must
    also be checked before accepting a recorded device comparison.
    """
    if type(raw) is not bytes or type(detail_enabled) is not bool:
        raise ValueError('completion_input_type')
    if not raw.endswith(b'\n') or not 0 < len(raw) <= 65536:
        raise ValueError('completion_partial_or_capacity')
    if any(len(line) > 1024 for line in raw.splitlines(keepends=True)):
        raise ValueError('completion_line_limit')
    text = raw.decode('ascii')
    plain = re.sub(r'\x1b\[[0-9;]*m', '', text)
    from derby_fixed_collector_check import load_collector
    if FATAL.search(plain) or load_collector().fault_category(raw, fixed_workload=True):
        raise ValueError('completion_runtime_fault')
    trial = parse_trial(raw, transport_complete=True)
    lines = plain.splitlines()

    def positions(pattern):
        return [i for i, line in enumerate(lines) if re.fullmatch(pattern, line)]

    identity = positions(r'(?:I \([0-9]+\) app: )?APP_ID local\.derby')
    ready = positions(r'(?:I \([0-9]+\) js: )?DERBY READY tones=(?:true|false)')
    end = positions(r'DERBY_FIXED_END .*')
    owner = positions(r'DERBY_DETAIL_EXIT v=1 session=' + re.escape(trial['session']) + r' clean=1')
    stop = positions(r'(?:I \([0-9]+\) app: )?APP_STOPPED')
    home = positions(r'(?:I \([0-9]+\) shell: )?HOME_READY')
    if any(len(p) != 1 for p in (identity, ready, end, owner, stop, home)):
        raise ValueError('completion_missing_or_duplicate_marker')
    if not identity[0] < ready[0] < end[0] < owner[0] < stop[0] < home[0]:
        raise ValueError('completion_marker_order')
    # Reject all malformed/legacy owner records, even alongside a valid one.
    if sum(OWNER in line for line in lines) != 1:
        raise ValueError('completion_owner_schema')
    if sum('APP_ID' in line for line in lines) != 1 or sum('DERBY READY' in line for line in lines) != 1:
        raise ValueError('completion_relaunch')
    # A malformed extra terminal record must not hide beside a valid one.
    if any(sum(marker in line for line in lines) != 1
           for marker in ('DERBY_FIXED_END', 'APP_STOPPED', 'HOME_READY')):
        raise ValueError('completion_terminal_schema')
    if not detail_enabled and any('DERBY_DETAIL' in line and not line.startswith(OWNER+' ') for line in lines):
        raise ValueError('completion_detail_present_in_OFF')
    return dict(contract=CONTRACT, detail_mode='ON' if detail_enabled else 'OFF',
                session=trial['session'], owner_completed=True, stop_home=True)
