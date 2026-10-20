"""Post-trial resource observations. No device access or peak-memory claim."""
import re

MARKER = 'DERBY_RESOURCE'
FLOORS = ('ui_stack_free_bytes', 'internal_free_bytes',
          'internal_largest_bytes', 'guest_headroom_bytes')
FIELDS = {'v', 'session', 'reps', 'phase', 'valid', 'task', 'unit', 'source',
          'stack_scope', 'heap_scope', 'stack_free', 'internal_free',
          'internal_largest', 'guest_used', 'guest_cap'}


def need(ok, reason):
    if not ok:
        raise ValueError('runtime_resources_' + reason)


def parse_resources(raw, *, session, required=False, floors=None):
    """Validate any marker; qualification requires explicit positive floors.

    UI stack is ESP-IDF 6.0.1 byte HWM since task creation, including observer
    frames and prior applications. Heap/guest counters are current post-trial
    snapshots; transient allocation peaks are not observed here. A successful
    offline comparison authenticates no archive and is no physical acceptance.
    """
    need(type(required) is bool and type(session) is str and
         re.fullmatch(r'[1-9][0-9]*', session) is not None, 'input')
    need(type(raw) is bytes and 0 < len(raw) <= 65536 and raw.endswith(b'\n'), 'capture')
    need(all(len(line) <= 1024 for line in raw.splitlines(keepends=True)), 'line_limit')
    if required:
        need(type(floors) is dict and set(floors) == set(FLOORS) and
             all(type(x) is int and 0 < x <= 0xffffffff for x in floors.values()), 'explicit_floors')
    else:
        need(floors is None, 'floors_without_requirement')
    lines = raw.decode('ascii').splitlines()
    positions = [i for i, line in enumerate(lines) if MARKER in line]
    if not positions:
        need(not required, 'missing')
        return dict(available=False, resource_verified=False)
    need(len(positions) == 1, 'duplicate')
    pos = positions[0]
    tokens = lines[pos].split(' ')
    need(tokens[0] == MARKER and all('=' in x for x in tokens[1:]), 'schema')
    pairs = [x.split('=', 1) for x in tokens[1:]]
    row = dict(pairs)
    need(len(row) == len(pairs) and set(row) == FIELDS, 'schema')
    expected = dict(v='1', session=session, reps='3', phase='post_trial_tail', valid='1',
                    task='ui', unit='bytes', stack_scope='task_lifetime', heap_scope='current')
    need(all(row[k] == v for k, v in expected.items()) and
         row['source'] in ('esp-idf-6.0.1', 'synthetic'), 'identity_or_scope')
    numbers = {}
    for key in ('stack_free', 'internal_free', 'internal_largest', 'guest_used', 'guest_cap'):
        need(re.fullmatch(r'[1-9][0-9]{0,9}', row[key]) is not None, 'integer')
        numbers[key] = int(row[key])
        need(numbers[key] <= 0xffffffff, 'overflow')
    need(numbers['stack_free'] <= 32768 and
         numbers['internal_largest'] <= numbers['internal_free'] and
         numbers['guest_used'] <= numbers['guest_cap'], 'inconsistent')
    summaries = [i for i, line in enumerate(lines) if line.startswith('DERBY_FIXED ')]
    ends = [i for i, line in enumerate(lines) if 'DERBY_FIXED_END' in line]
    need(len(summaries) == 3 and len(ends) == 1 and summaries[-1] < pos < ends[0], 'order')
    for rep, index in enumerate(summaries, 1):
        need(re.search(r' session=' + re.escape(session) + r' rep=' + str(rep) + r' ', lines[index])
             is not None, 'summary_identity')
    values = dict(ui_stack_free_bytes=numbers['stack_free'], internal_free_bytes=numbers['internal_free'],
                  internal_largest_bytes=numbers['internal_largest'],
                  guest_headroom_bytes=numbers['guest_cap'] - numbers['guest_used'])
    if required:
        need(row['source'] == 'esp-idf-6.0.1', 'synthetic_not_qualifying')
        need(all(values[key] >= floors[key] for key in FLOORS), 'below_floor')
    return dict(available=True, resource_verified=required, source=row['source'], session=session,
                task='ui', unit='bytes', phase='post_trial_tail', stack_scope='task_lifetime',
                heap_scope='current', values=values, guest_used_bytes=numbers['guest_used'],
                guest_cap_bytes=numbers['guest_cap'], floors=floors,
                scope='offline resource thresholds; no transient heap peak or physical acceptance')
