"""New synthetic r2 regressions; no hardware, private exports, or build proof.

These cases are newly authored from the public protocol, not a recovery of an
earlier test bundle. They exercise both real parsers and the offline gate.
The proof objects model exported fields; they do not exercise prove(BIN/ELF).
"""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
sys.path.insert(0, str(ROOT/'tools/games'))
from test_derby_fixed_completion import receipts, verify
from derby_fixed_completion import parse_completion
from derby_detail_result import SCHEMA
from verify_derby_recorded_trial import digest, read_json


def synthetic_raw(mode):
    lines = ['APP_ID local.derby', 'DERBY READY tones=true']
    fixture = (ROOT/'tools/games/fixtures/derby_fixed_v1.log').read_text()
    for line in fixture.splitlines():
        if line.startswith('#'):
            continue
        if mode == 'ON' and line.startswith('DERBY_FIXED v='):
            row = dict(word.split('=', 1) for word in line.split()[1:])
            rep = row['rep']
            # Use the documented fixed calls and a small, feasible timing model.
            calls = [120, 120, 240, 120, 120, 120, 720] + [int(row['s3'].split(':')[1])]
            calls += [1]*6 + [calls[7]] + [1]*(len(SCHEMA['stages'])-15)
            for stage, count in enumerate(calls):
                lines.append(f'DERBY_DETAIL v=1 session=1 rep={rep} stage={stage} inc_us=1 self_us=1 calls={count}')
            for event, name in enumerate(SCHEMA['events']):
                amount = 0 if name in ('BRIDGE_SNAPSHOT_BYTES', 'TX_FAILURES') else 1
                lines.append(f'DERBY_DETAIL_BYTES v=1 session=1 rep={rep} event={event} amount={amount}')
            lines.append(f'DERBY_DETAIL_END v=1 session=1 rep={rep} stages={len(calls)} '
                         f'events={len(SCHEMA["events"])} faults=0 pairs={sum(calls)} elapsed=1 logical_bytes=1')
        lines.append(line)
    lines += ['DERBY_DETAIL_EXIT v=1 session=1 clean=1', 'APP_STOPPED', 'HOME_READY']
    return ('\n'.join(lines)+'\n').encode('ascii')


def set_path(args, path, value):
    target = args
    for key in path[:-1]:
        target = target[key]
    target[path[-1]] = value


def refresh_hashes(args):
    # Recompute envelope hashes so nested binding mutations reach their gate.
    args[4]['manifest_sha256'] = digest(args[3])
    args[5]['plan_hash'] = digest(args[4])
    args[8]['plan_hash'] = digest(args[7])
    if type(args[8]['fixed_trial']['binding']) is dict:
        args[8]['fixed_trial']['binding']['plan_hash'] = args[8]['plan_hash']


class R2Tests(unittest.TestCase):
    pass


COUNTS = dict(positive=0, negative=0)


def add(name, positive, run):
    COUNTS['positive' if positive else 'negative'] += 1
    setattr(R2Tests, 'test_'+name, run)


def positive_case(mode, style, ending, tones):
    def run(self):
        raw = synthetic_raw(mode).replace(b'tones=true', b'tones='+tones)
        if style != 'bare':
            for marker, tag in ((b'APP_ID local.derby', b'app'),
                                (b'DERBY READY', b'js'),
                                (b'APP_STOPPED', b'app'), (b'HOME_READY', b'shell')):
                raw = raw.replace(marker, b'I (206206) '+tag+b': '+marker)
        if style == 'ansi':
            raw = b'\n'.join(b'\x1b[0;32m'+line+b'\x1b[0m' if line.startswith(b'I (') else line
                             for line in raw.split(b'\n'))
        raw = raw.replace(b'\n', ending)
        result = verify(receipts(raw, mode), mode)
        self.assertIs(result['recorded_chain_matches'], True)
        self.assertIs(result['hardware_tested'], False)
        self.assertEqual(result['owner']['detail_mode'], mode)
        self.assertEqual(len(result['detail']), 3 if mode == 'ON' else 0)
    return run


def record_case(mode, path, value, reason=None):
    def run(self):
        args = receipts(synthetic_raw(mode), mode)
        set_path(args, path, copy.deepcopy(value))
        refresh_hashes(args)
        if reason:
            with self.assertRaisesRegex(ValueError, '^'+reason+'$'):
                verify(args, mode)
        else:
            with self.assertRaises((ValueError, KeyError)):
                verify(args, mode)
    return run


def raw_case(mode, mutate):
    def run(self):
        raw = mutate(synthetic_raw(mode))
        with self.assertRaises(ValueError):
            verify(receipts(raw, mode), mode)
    return run


for mode in ('ON', 'OFF'):
    for style in ('bare', 'tagged', 'ansi'):
        for label, ending in (('lf', b'\n'), ('crlf', b'\r\n')):
            for tones in (b'true', b'false'):
                add(f'{mode}_{style}_{label}_tones_{tones.decode()}', True,
                    positive_case(mode, style, ending, tones))

    mutations = [
        ('proof_hardware', (2, 'hardware_tested'), True, 'build_proof_schema'),
        ('proof_hardware_zero', (2, 'hardware_tested'), 0, 'build_proof_schema'),
        ('proof_mode', (2, 'detail_measurement', 'mode'), 'OFF' if mode == 'ON' else 'ON', 'verified_detail_mode_mismatch'),
        ('telemetry_off', (2, 'build_mode', 'telemetry_mode'), 'OFF', 'fixed_build_required'),
        ('measure_off', (2, 'build_measure', 'measure_mode'), 'OFF', 'fixed_build_required'),
        ('fixed_off', (2, 'fixed_workload', 'mode'), 'OFF', 'fixed_build_required'),
        ('owner_contract', (2, 'fixed_workload', 'owner_completion', 'contract'), 'legacy', 'verified_owner_contract_mismatch'),
        ('repository', (3, 'source_provenance', 'repository'), 'example/other', 'source_commit_mismatch'),
        ('source_head', (3, 'source_provenance', 'commit'), 'f'*40, 'source_commit_mismatch'),
        ('source_receipt', (3, 'source_provenance', 'source_receipt'), 'x'*64, 'source_receipt_missing'),
        ('source_sha', (3, 'source_provenance', 'source_sha256'), '', 'source_receipt_missing'),
        ('idf', (3, 'idf_version'), '6.0.0', 'manifest_target'),
        ('target', (3, 'target'), 'esp32', 'manifest_target'),
        ('artifact_hash', (3, 'artifacts', 0, 'sha256'), '0'*64, 'proof_manifest_artifact_mismatch'),
        ('artifact_size', (3, 'artifacts', 0, 'bytes'), 289, 'proof_manifest_artifact_mismatch'),
        ('flash_operation', (4, 'flash_operation'), 'erase', 'explicit_flash_plan_required'),
        ('flash_backend', (4, 'backend'), 'other', 'explicit_flash_plan_required'),
        ('flash_build_job', (4, 'build_job_id'), 'other', 'flash_build_manifest_mismatch'),
        ('flash_elf', (4, 'expected_elf_sha256'), '0'*64, 'flash_image_mismatch'),
        ('flash_bin', (4, 'expected_app_bin_sha256'), '0'*64, 'flash_image_mismatch'),
        ('flash_job_state', (5, 'state'), 'running', 'flash_job_not_committed'),
        ('flash_summary_job', (6, 'requested_job_id'), 'other', 'flash_summary_job_mismatch'),
        ('boot_prefix', (6, 'flash_summary', 'observed_elf_sha256_prefix'), 'f'*12, 'boot_elf_mismatch'),
        ('boot_basis', (6, 'flash_summary', 'verification_basis'), 'boot_text_only', 'flash_verification_basis'),
        ('legacy_protocol', (7, 'protocol'), 'pocketjs-derby-fixed-v1', 'strict_collector_plan_required'),
        ('plan_parser_commit', (7, 'fixed_workload', 'parser_commit'), 'f'*40, 'strict_collector_plan_required'),
        ('capture_elf', (7, 'expected_firmware_sha256'), '0'*64, 'capture_flash_binding_mismatch'),
        ('capture_device', (7, 'device_fingerprint'), '0'*64, 'capture_flash_binding_mismatch'),
        ('capture_state', (8, 'state'), 'failed', 'collection_job_not_committed'),
        ('capture_terminal', (8, 'fixed_trial', 'terminal_contract'), 'end_only', 'collection_owner_terminal_not_verified'),
        ('capture_raw', (8, 'fixed_trial', 'raw_base64'), 'bm90LXRoZS1sb2c=', 'collection_export_raw_mismatch'),
        ('binding_not_dict', (8, 'fixed_trial', 'binding'), [], 'collection_export_binding_schema'),
        ('binding_parser', (8, 'fixed_trial', 'binding', 'protocol_parser_commit'), 'f'*40, 'collection_export_binding_mismatch'),
        ('binding_expected_elf', (8, 'fixed_trial', 'binding', 'expected_firmware_sha256'), '0'*64, 'collection_export_binding_mismatch'),
        ('binding_boot_source', (8, 'fixed_trial', 'binding', 'installed_boot_source_image'), 'verified', 'collection_export_binding_mismatch'),
        ('binding_job', (8, 'fixed_trial', 'binding', 'job_id'), 'other', 'collection_export_binding_mismatch'),
        ('binding_epoch', (8, 'fixed_trial', 'binding', 'broker_epoch'), 'other', 'collection_export_binding_mismatch'),
        ('binding_device', (8, 'fixed_trial', 'binding', 'device_fingerprint'), 'other', 'collection_export_binding_mismatch'),
        ('typed_flash_required', (7, 'firmware_verification'), 'hash_only', 'typed_flash_binding_required'),
        ('typed_flash_job', (7, 'flash_job_id'), 'other', 'typed_flash_binding_required'),
        ('typed_principal', (8, 'fixed_trial', 'binding', 'principal_id'), 'other', 'typed_flash_binding_mismatch'),
        ('typed_epoch', (7, 'broker_epoch'), 'other', 'collection_export_binding_mismatch'),
        ('typed_session', (7, 'session_id'), 'other', 'typed_flash_binding_mismatch'),
        ('reported_session', (8, 'fixed_trial', 'binding', 'device_reported_session'), '2', 'collection_export_session_mismatch'),
        ('reported_session_int', (8, 'fixed_trial', 'binding', 'device_reported_session'), 1, 'collection_export_session_mismatch'),
    ]
    for index, name in ((2, 'proof'), (3, 'manifest')):
        for label, value in (('bool', True), ('float', 1.0), ('string', '1'), ('version', 2)):
            mutations.append((f'{name}_schema_{label}', (index, 'schema_version'), value,
                              'build_proof_schema' if index == 2 else 'manifest_target'))
    for key in ('readback_verified', 'boot_observed', 'passive_boot_verified', 'boot_log_matches_expected_artifact'):
        mutations.append((key, (6, 'flash_summary', key), False, 'flash_readback_or_boot_missing'))
    for key in ('available', 'valid', 'terminal_success_verified'):
        mutations.append((key, (8, 'fixed_trial', key), False, 'collection_owner_terminal_not_verified'))
    for where, prefix in (('planned', (7, 'collector_binding')),
                          ('captured', (8, 'fixed_trial', 'binding', 'flash_receipt'))):
        for label, value in (('absent', None), ('list', []), ('string', 'receipt')):
            mutations.append((where+'_'+label, prefix, value, 'typed_flash_binding_schema'))
        for key in receipts(synthetic_raw(mode), mode)[7]['collector_binding']:
            if key == 'schema_version':
                for label, value in (('bool', True), ('float', 1.0), ('string', '1')):
                    mutations.append((where+'_schema_'+label, prefix+(key,), value, 'typed_flash_binding_schema'))
                value = 2
            else:
                value = 'mismatch'
            reason = 'typed_flash_receipt_schema' if key == 'firmware_receipt' else 'typed_flash_binding_mismatch'
            mutations.append((where+'_'+key, prefix+(key,), value, reason))
    for name, path, value, reason in mutations:
        add(f'{mode}_{name}', False, record_case(mode, path, value, reason))

    raw_mutations = {}
    markers = [b'APP_ID local.derby', b'DERBY READY tones=true',
               b'DERBY_FIXED_END v=1 session=1 reps=3 valid=1 quality=0 independent=0',
               b'DERBY_DETAIL_EXIT v=1 session=1 clean=1', b'APP_STOPPED', b'HOME_READY']
    for i, marker in enumerate(markers):
        raw_mutations[f'missing_marker_{i}'] = lambda raw, m=marker: raw.replace(m+b'\n', b'')
        raw_mutations[f'duplicate_marker_{i}'] = lambda raw, m=marker: raw+m+b'\n'
        raw_mutations[f'malformed_extra_marker_{i}'] = lambda raw, m=marker: raw+b'noise '+m+b' extra\n'
    raw_mutations.update(
        wrong_identity_tag=lambda r: r.replace(b'APP_ID local.derby', b'I (1) js: APP_ID local.derby'),
        wrong_ready_tag=lambda r: r.replace(b'DERBY READY', b'I (1) app: DERBY READY'),
        warning_ready=lambda r: r.replace(b'DERBY READY', b'W (1) js: DERBY READY'),
        unclean_owner=lambda r: r.replace(b'clean=1', b'clean=0'),
        owner_wrong_session=lambda r: r.replace(b'EXIT v=1 session=1', b'EXIT v=1 session=2'),
        owner_after_home=lambda r: r.replace(markers[3]+b'\n', b'')+markers[3]+b'\n',
        partial_tail=lambda r: r[:-1],
        line_limit=lambda r: r+b'x'*1024+b'\n',
        capacity=lambda r: r+b'x'*65536+b'\n',
        bad_quality=lambda r: r.replace(b'quality=0', b'quality=1', 1),
        changed_work=lambda r: r.replace(b'c1=120', b'c1=119', 1),
        mixed_session=lambda r: r.replace(b'session=1 rep=2', b'session=2 rep=2'),
    )
    for fault in (b'InternalError: interrupted', b'FRAME RUNAWAY', b'DERBY LOADSTALL',
                  b'Guru Meditation', b'stop hook did not finish', b'DERBY owner watchdog timeout', b'ESP-ROM:'):
        raw_mutations['fatal_'+fault.decode().replace(' ', '_')] = lambda r, f=fault: r+f+b'\n'
    if mode == 'ON':
        raw_mutations['detail_missing_stage'] = lambda r: r.replace(
            next(line for line in r.splitlines() if line.startswith(b'DERBY_DETAIL v='))+b'\n', b'', 1)
        raw_mutations['detail_time'] = lambda r: r.replace(b'inc_us=1 self_us=1', b'inc_us=0 self_us=1', 1)
        raw_mutations['detail_tx_failure'] = lambda r: r.replace(b'event=12 amount=0', b'event=12 amount=1', 1)
    else:
        raw_mutations['detail_in_off'] = lambda r: r.replace(b'DERBY_FIXED_END', b'DERBY_DETAIL v=1\nDERBY_FIXED_END')
    for name, mutate in raw_mutations.items():
        add(f'{mode}_raw_{name}', False, raw_case(mode, mutate))


def input_type_test(self):
    for raw, detail in ((bytearray(b'x\n'), False), (b'x\n', 1), ('x\n', False)):
        with self.assertRaisesRegex(ValueError, '^completion_input_type$'):
            parse_completion(raw, detail_enabled=detail)


add('input_types', False, input_type_test)


def receipt_identity_case(mode, locations, value, missing=False):
    def run(self):
        args = receipts(synthetic_raw(mode), mode)
        targets = [(args[6]['flash_summary'], 'last_known_good_receipt'),
                   (args[7]['collector_binding'], 'firmware_receipt'),
                   (args[8]['fixed_trial']['binding']['flash_receipt'], 'firmware_receipt')]
        for index in locations:
            target, key = targets[index]
            if missing:
                del target[key]
            else:
                target[key] = copy.deepcopy(value)
        refresh_hashes(args)
        with self.assertRaisesRegex(ValueError, '^typed_flash_receipt_schema$'):
            verify(args, mode)
    return run


for mode in ('ON', 'OFF'):
    # Both unilateral damage and mutually matching invalid values must fail.
    for where, locations in (('summary', (0,)), ('planned', (1,)), ('captured', (2,)), ('all', (0, 1, 2))):
        invalid = [('null', None), ('bool', True), ('int', 0), ('float', 1.0),
                   ('list', []), ('dict', {}), ('empty', ''), ('short', 'a'*63),
                   ('long', 'a'*65), ('nonhex', 'g'*64), ('uppercase', 'A'*64),
                   ('whitespace', ' '+'a'*64), ('newline', 'a'*64+'\n')]
        for label, value in invalid:
            add(f'{mode}_receipt_{where}_{label}', False, receipt_identity_case(mode, locations, value))
        add(f'{mode}_receipt_{where}_missing', False, receipt_identity_case(mode, locations, None, missing=True))


def valid_receipt_case(mode, locations, accepted):
    def run(self):
        args = receipts(synthetic_raw(mode), mode)
        targets = [(args[6]['flash_summary'], 'last_known_good_receipt'),
                   (args[7]['collector_binding'], 'firmware_receipt'),
                   (args[8]['fixed_trial']['binding']['flash_receipt'], 'firmware_receipt')]
        for index in locations:
            target, key = targets[index]
            target[key] = 'abcdef0123456789'*4
        refresh_hashes(args)
        if accepted:
            self.assertIs(verify(args, mode)['recorded_chain_matches'], True)
        else:
            with self.assertRaisesRegex(ValueError, '^typed_flash_binding_mismatch$'):
                verify(args, mode)
    return run


for mode in ('ON', 'OFF'):
    add(f'{mode}_receipt_valid_hex', True, valid_receipt_case(mode, (0, 1, 2), True))
    for label, locations in (('planned', (1,)), ('captured', (2,)), ('both', (1, 2))):
        add(f'{mode}_receipt_valid_mismatch_{label}', False, valid_receipt_case(mode, locations, False))


def duplicate_json_test(self):
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary)/'synthetic.json'
        path.write_text('{"schema_version":1,"schema_version":1}')
        with self.assertRaisesRegex(ValueError, '^duplicate_json_key$'):
            read_json(path)


add('duplicate_json_keys', False, duplicate_json_test)


if __name__ == '__main__':
    print(json.dumps(dict(scope='new synthetic cases; no actual BIN/ELF prove or hardware', **COUNTS)), flush=True)
    unittest.main()
