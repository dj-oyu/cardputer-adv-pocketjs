"""Strict completion/receipt-chain mutations; all receipts here are synthetic."""
import argparse
import base64
import copy
import hashlib
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/games'))
from derby_fixed_completion import parse_completion
from derby_fixed_collector_check import check_collector, load_collector
from derby_fixed_workload_chunks import FIXTURE, MANIFEST_SHA256
from verify_derby_recorded_trial import digest, verify_recorded_trial


def receipts(raw, mode):
    """Model only documented broker export fields, never device authority."""
    elf, image, head = 'e'*64, 'b'*64, 'a'*40
    artifacts = {name: dict(sha256=value, bytes=288) for name, value in (
        ('cardputer_pocketjs.bin', image), ('cardputer_pocketjs.elf', elf))}
    proof = dict(schema_version=1, hardware_tested=False, artifacts=artifacts,
                 build_mode=dict(telemetry_mode='ON'), build_measure=dict(measure_mode='ON'),
                 detail_measurement=dict(mode=mode), fixed_workload=dict(mode='ON', fixture=FIXTURE,
                     manifest_sha256=MANIFEST_SHA256, owner_completion=dict(
                         contract='fixed-owner-v1', record='DERBY_DETAIL_EXIT', detail_mode=mode)))
    manifest = dict(schema_version=1, idf_version='6.0.1', target='esp32s3', build_job_id='build1',
                    build_plan_hash='4'*64,
                    source_provenance=dict(repository='dj-oyu/cardputer-adv-pocketjs', commit=head,
                                           source_receipt='1'*64, source_sha256='2'*64),
                    artifacts=[dict(name=name, **item) for name, item in artifacts.items()])
    plan = dict(test='firmware_flash', flash_operation='flash', backend='serial', job_id='flash1',
                build_job_id='build1', manifest_sha256=digest(manifest), expected_app_bin_sha256=image,
                expected_elf_sha256=elf, device_fingerprint='d'*64)
    plan.update(flash_principal='synthetic-owner', device_id='synthetic-device',
                broker_epoch='3'*32, session_id='synthetic-session')
    job = dict(id='flash1', state='succeeded', plan_hash=digest(plan))
    summary = dict(readback_verified=True, boot_observed=True, passive_boot_verified=True,
                   last_known_good_receipt='5'*64,
                   boot_log_matches_expected_artifact=True, target='esp32s3', observed_idf_version='6.0.1',
                   observed_elf_sha256_prefix=elf[:12],
                   verification_basis='full_segment_sha256_readback_and_boot_elf_prefix')
    summary = dict(requested_job_id='flash1', flash_summary=summary)
    collection = dict(test='derby_fixed_workload', backend='serial', protocol='pocketjs-derby-fixed-v2',
                      fixed_workload=load_collector().FIXED_PLAN, expected_firmware_sha256=elf,
                      device_fingerprint='d'*64, job_id='capture1', broker_epoch='3'*32)
    collection.update(session_id=plan['session_id'], firmware_verification='successful_flash_receipt',
                      flash_job_id=job['id'])
    collection['collector_binding'] = dict(schema_version=1, principal_id=plan['flash_principal'],
        device_id=plan['device_id'], device_fingerprint=plan['device_fingerprint'],
        flash_job_id=job['id'], flash_plan_hash=job['plan_hash'],
        firmware_receipt=summary['flash_summary']['last_known_good_receipt'],
        build_job_id=manifest['build_job_id'], build_plan_hash=manifest['build_plan_hash'],
        source_receipt=manifest['source_provenance']['source_receipt'], source_commit=head,
        manifest_sha256=digest(manifest), modeproof_sha256=digest(proof), derby_detail_mode=mode,
        expected_elf_sha256=elf, expected_app_bin_sha256=image)
    receipt = dict(complete=True, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(), duration_ms=10000,
                   **{key: False for key in ('gaps','overflow','truncated','partial_tail','disconnect','reset','refused')})
    collected = dict(id='capture1', state='succeeded', plan_hash=digest(collection))
    collected['fixed_trial'] = dict(available=True, valid=True, terminal_success_verified=True,
        terminal_contract='owner_exit_v2', validation_error=None, receipt=receipt,
        raw_base64=base64.b64encode(raw).decode(), binding=dict(job_id='capture1', plan_hash=collected['plan_hash'],
            broker_epoch=collection['broker_epoch'], device_fingerprint='d'*64,
            expected_firmware_sha256=elf, principal_id=plan['flash_principal'],
            protocol_parser_commit=collection['fixed_workload']['parser_commit'],
            installed_boot_source_image='unverified', device_reported_session='1',
            flash_receipt=copy.deepcopy(collection['collector_binding'])))
    return [raw, receipt, proof, manifest, plan, job, summary, collection, collected]


def verify(args, mode):
    return verify_recorded_trial(*args, expected_head='a'*40, expected_detail=mode)


class CompletionTests(unittest.TestCase):
    def test_synthetic_chain_and_failures(self):
        fixture = (ROOT/'tools/games/fixtures/derby_fixed_v1.log').read_bytes()
        raw = (b'APP_ID local.derby\nDERBY READY tones=true\n' + fixture +
               b'DERBY_DETAIL_EXIT v=1 session=1 clean=1\nAPP_STOPPED\nHOME_READY\n')
        args = receipts(raw, 'OFF')
        self.assertTrue(verify(args, 'OFF')['recorded_chain_matches'])
        check_collector(raw)
        cases = [(2, ('detail_measurement','mode'), 'ON'),
                 (2, ('fixed_workload','owner_completion','detail_mode'), 'ON'),
                 (2, ('fixed_workload','owner_completion','contract'), 'legacy'),
                 (3, ('source_provenance','commit'), 'f'*40),
                 (3, ('artifacts',0,'sha256'), '0'*64),
                 (4, ('build_job_id',), 'other'), (4, ('manifest_sha256',), '0'*64),
                 (4, ('expected_app_bin_sha256',), '0'*64), (4, ('expected_elf_sha256',), '0'*64),
                 (5, ('state',), 'running'), (5, ('plan_hash',), '0'*64),
                 (6, ('requested_job_id',), 'other'),
                 (6, ('flash_summary','readback_verified'), False), (6, ('flash_summary','passive_boot_verified'), False),
                 (6, ('flash_summary','verification_basis'), 'boot_text_only'),
                 (6, ('flash_summary','observed_elf_sha256_prefix'), 'f'*12),
                 (7, ('protocol',), 'pocketjs-derby-fixed-v1'),
                 (7, ('device_fingerprint',), '0'*64),
                 (7, ('expected_firmware_sha256',), '0'*64),
                 (8, ('state',), 'failed'), (8, ('plan_hash',), '0'*64),
                 (8, ('fixed_trial','terminal_success_verified'), False),
                 (8, ('fixed_trial','raw_base64'), base64.b64encode(raw+b'noise\n').decode()),
                 (8, ('fixed_trial','binding','job_id'), 'other')]
        for index, keys, value in cases:
            damaged = copy.deepcopy(args)
            item = damaged[index]
            for key in keys[:-1]: item = item[key]
            item[keys[-1]] = value
            with self.subTest(keys=keys), self.assertRaises((ValueError, KeyError)):
                verify(damaged, 'OFF')
        for flag in ('gaps','overflow','truncated','partial_tail','disconnect','reset','refused'):
            damaged = copy.deepcopy(args)
            damaged[1][flag] = True
            with self.subTest(flag=flag), self.assertRaises(ValueError): verify(damaged, 'OFF')

    def test_actual_mode_logs(self):
        for mode, path in ACTUAL_LOGS:
            raw = path.read_bytes()
            verify(receipts(raw, mode), mode)
            check_collector(raw, detail_enabled=mode == 'ON')
            owner = next(line for line in raw.splitlines() if line.startswith(b'DERBY_DETAIL_EXIT'))
            cases = [raw.replace(owner+b'\n', b''), raw.replace(owner,owner+b'\n'+owner),
                     raw.replace(owner,owner.replace(b'clean=1',b'clean=0')),
                     raw.replace(owner,owner+b' extra=0'), raw[:-1],
                     raw.replace(b'APP_STOPPED',b'InternalError: interrupted\nAPP_STOPPED'),
                     raw+b'TypeError: after HOME\n', raw+b'DERBY owner watchdog timeout\n',
                     raw.replace(b'APP_STOPPED',b'W (1) app: APP_STOPPED'),
                     raw.replace(b'HOME_READY',b'I (1) app: HOME_READY')]
            for bad in cases:
                with self.assertRaises(ValueError): verify(receipts(bad, mode), mode)


def load_tests(loader, tests, pattern):
    # Keep the new synthetic gate on the existing completion CI entry point.
    from test_derby_recorded_trial_r2 import R2Tests
    tests.addTests(loader.loadTestsFromTestCase(R2Tests))
    return tests


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--off-log', type=Path)
    parser.add_argument('--on-log', type=Path)
    args, remaining = parser.parse_known_args()
    ACTUAL_LOGS = [(mode, path) for mode, path in (('OFF',args.off_log),('ON',args.on_log)) if path]
    unittest.main(argv=[sys.argv[0], *remaining])
