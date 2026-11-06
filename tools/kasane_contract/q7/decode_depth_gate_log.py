"""Strict QG USB-line reconstruction; incomplete diagnostics never count as pass."""
import argparse
import json
import re

FIELDS = '''first_attempts first_success inexact_before inexact_after inexact_raised
inexact_cleared inexact_clear prepare_attempts error_reject inexact_reject type_reject
coefficient_reject radius_reject scale_reject endpoint_reject integer_rows
integer_pixels integer_uncertain integer_carry_reject first_fsr_changed
first_fcr_changed setup_fcr_changed setup_fsr_changed fsr_before_or fsr_after_or
setup_fsr_before_or setup_fsr_after_or raw_fsr'''.split()
PHASES = [('ready', 0), ('warmup_ack', 30), ('submitted', 60), ('submitted', 90),
          ('submitted', 120), ('submitted', 150), ('end', 150)]
MASKS = {'fsr_before_or', 'fsr_after_or', 'setup_fsr_before_or', 'setup_fsr_after_or'}
METADATA = MASKS | {'raw_fsr'}
PATTERN = re.compile(r'^QG (\w+) f=(\d+) part=(\d+)/(\d+) (.+)$')

def decode(lines):
    samples = {}
    parts = {}
    for original in lines:
        if 'QG ' not in original:
            continue
        line = original[original.index('QG '):].rstrip('\r\n')
        if len(line.encode('ascii')) > 180:
            raise ValueError('Oversized diagnostic line')
        match = PATTERN.fullmatch(line)
        if not match:
            raise ValueError('Malformed diagnostic line')
        phase, frame, part, total, payload = match.groups()
        key = (phase, int(frame)); part = int(part); total = int(total)
        if key not in PHASES or total != (len(FIELDS)+1)//2 or not 0 <= part < total:
            raise ValueError('Unexpected phase/frame/part count')
        seen = parts.setdefault(key, set())
        if part in seen:
            raise ValueError('Duplicate diagnostic part')
        seen.add(part)
        values = samples.setdefault(key, {})
        tokens = payload.split()
        expected = FIELDS[part*2:part*2+2]
        if len(tokens) != len(expected):
            raise ValueError('Missing diagnostic field')
        for token, name in zip(tokens, expected):
            if not token.startswith(name+'=') or name in values:
                raise ValueError('Unexpected or repeated diagnostic field')
            raw = token[len(name)+1:]
            if not raw.isascii() or not raw.isdigit() or int(raw) > 2**53-1:
                raise ValueError('Counter is not an exact nonnegative JS integer')
            values[name] = int(raw)
    if set(samples) != set(PHASES):
        raise ValueError('Incomplete diagnostic phases')
    previous = None
    ordered = []
    for key in PHASES:
        value = samples[key]
        if set(value) != set(FIELDS) or len(parts[key]) != (len(FIELDS)+1)//2:
            raise ValueError('Incomplete diagnostic sample')
        if value['raw_fsr'] not in (0, 1):
            raise ValueError('Invalid FSR encoding')
        if previous:
            if any(value[k] < previous[k] for k in FIELDS if k not in METADATA):
                raise ValueError('Counter reset or backward sample')
            if any(value[k] | previous[k] != value[k] for k in MASKS):
                raise ValueError('Cumulative FSR metadata lost bits')
            if value['raw_fsr'] != previous['raw_fsr']:
                raise ValueError('Mixed FSR encodings')
        delta = None if previous is None else {k:value[k]-previous[k] for k in FIELDS if k not in METADATA}
        ordered.append({'phase':key[0], 'submitted':key[1], 'cumulative':value, 'delta':delta})
        previous = value
    return ordered

def validate_partitions(samples):
    for sample in samples:
        v = sample['cumulative']
        rejects = sum(v[k] for k in ('error_reject','inexact_reject','type_reject',
            'coefficient_reject','radius_reject','scale_reject','endpoint_reject'))
        valid = (v['first_attempts'] == v['inexact_after'] + v['inexact_clear'] and
            v['inexact_after'] + v['inexact_cleared'] == v['inexact_before'] + v['inexact_raised'] and
            v['first_success'] == v['prepare_attempts'] and
            v['prepare_attempts'] == rejects + v['integer_rows'] and
            v['first_success'] <= v['first_attempts'] and
            v['inexact_before'] <= v['first_attempts'] and
            v['inexact_cleared'] <= v['inexact_before'] and
            v['inexact_raised'] <= v['first_attempts'] - v['inexact_before'] and
            v['first_fsr_changed'] >= v['inexact_raised'] + v['inexact_cleared'] and
            v['first_fsr_changed'] <= v['first_attempts'] and
            v['first_fcr_changed'] <= v['first_attempts'] and
            v['setup_fcr_changed'] <= v['prepare_attempts'] and
            v['setup_fsr_changed'] <= v['prepare_attempts'] and
            v['integer_rows'] <= v['inexact_after'])
        if not valid:
            raise ValueError('Diagnostic counter partition failure')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log')
    args = parser.parse_args()
    with open(args.log, encoding='utf-8') as stream:
        samples = decode(stream)
    validate_partitions(samples)
    print(json.dumps(samples, indent=2))
