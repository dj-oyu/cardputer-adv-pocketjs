"""Offline build/flash/capture consistency gate; never opens a device.

Inputs must be owner-exported durable broker records. Consistent JSON is not
authentication, flash authority, physical LCD proof, or an installed identity.
The CLI recomputes mode and image proof from the actual local BIN/ELF pair.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from derby_fixed_completion import parse_completion
from derby_fixed_result import parse_collected
from derby_detail_result import parse_detail
from derby_fixed_collector_check import load_collector
from verify_diagnostic_build import prove


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def verify_recorded_trial(raw, transport, proof, manifest, flash_plan, flash_job,
                          flash_summary, collection_plan, collection_job, *, expected_head, expected_detail):
    """Check exported record relationships after local build proof recomputation.

    No latest/current receipt is guessed from a matching app hash. The caller
    supplies the exact prepared flash plan and capture plan for this trial.
    """
    require(re.fullmatch(r'[a-f0-9]{40}', expected_head) is not None, 'expected_full_head')
    require(expected_detail in ('ON', 'OFF'), 'expected_detail_mode')
    require(proof['schema_version'] == 1 and proof['hardware_tested'] is False, 'build_proof_schema')
    require(proof['detail_measurement']['mode'] == expected_detail, 'verified_detail_mode_mismatch')
    require(proof['build_mode']['telemetry_mode'] == 'ON' and
            proof['build_measure']['measure_mode'] == 'ON' and
            proof['fixed_workload']['mode'] == 'ON', 'fixed_build_required')
    completion = proof['fixed_workload']['owner_completion']
    require(completion == dict(contract='fixed-owner-v1', record='DERBY_DETAIL_EXIT', detail_mode=expected_detail),
            'verified_owner_contract_mismatch')
    require(manifest['schema_version'] == 1 and manifest['idf_version'] == '6.0.1' and
            manifest['target'] == 'esp32s3', 'manifest_target')
    source = manifest['source_provenance']
    require(source['repository'] == 'dj-oyu/cardputer-adv-pocketjs' and source['commit'] == expected_head,
            'source_commit_mismatch')
    for key in ('source_receipt', 'source_sha256'):
        require(type(source[key]) is str and re.fullmatch(r'[a-f0-9]{64}', source[key]) is not None,
                'source_receipt_missing')
    artifacts = manifest['artifacts']
    require(type(artifacts) is list and len({item['name'] for item in artifacts}) == len(artifacts),
            'manifest_duplicate_artifact')
    index = {item['name']: item for item in artifacts}
    for name, item in proof['artifacts'].items():
        require(name in index and all(index[name][key] == item[key] for key in ('sha256', 'bytes')),
                'proof_manifest_artifact_mismatch')
    image = proof['artifacts']['cardputer_pocketjs.bin']['sha256']
    elf = proof['artifacts']['cardputer_pocketjs.elf']['sha256']
    require(flash_plan['test'] == 'firmware_flash' and flash_plan['flash_operation'] == 'flash' and
            flash_plan['backend'] == 'serial', 'explicit_flash_plan_required')
    require(flash_plan['build_job_id'] == manifest['build_job_id'] and
            flash_plan['manifest_sha256'] == digest(manifest), 'flash_build_manifest_mismatch')
    require(flash_plan['expected_app_bin_sha256'] == image and flash_plan['expected_elf_sha256'] == elf,
            'flash_image_mismatch')
    require(flash_job['id'] == flash_plan['job_id'] and flash_job['state'] == 'succeeded' and
            flash_job['plan_hash'] == digest(flash_plan), 'flash_job_not_committed')
    require(flash_summary['requested_job_id'] == flash_plan['job_id'], 'flash_summary_job_mismatch')
    flash_summary = flash_summary['flash_summary']
    require(all(flash_summary[key] is True for key in (
        'readback_verified', 'boot_observed', 'passive_boot_verified', 'boot_log_matches_expected_artifact')),
        'flash_readback_or_boot_missing')
    require(flash_summary['verification_basis'] == 'full_segment_sha256_readback_and_boot_elf_prefix' and
            flash_summary['target'] == 'esp32s3' and flash_summary['observed_idf_version'] == '6.0.1',
            'flash_verification_basis')
    prefix = flash_summary['observed_elf_sha256_prefix']
    require(type(prefix) is str and re.fullmatch(r'[a-f0-9]{8,16}', prefix) is not None and elf.startswith(prefix),
            'boot_elf_mismatch')
    require(collection_plan['test'] == 'derby_fixed_workload' and collection_plan['backend'] == 'serial' and
            collection_plan['protocol'] == 'pocketjs-derby-fixed-v2' and
            collection_plan['fixed_workload'] == load_collector().FIXED_PLAN, 'strict_collector_plan_required')
    require(collection_plan['expected_firmware_sha256'] == elf and
            collection_plan['device_fingerprint'] == flash_plan['device_fingerprint'], 'capture_flash_binding_mismatch')
    require(collection_job['id'] == collection_plan['job_id'] and collection_job['state'] == 'succeeded' and
            collection_job['plan_hash'] == digest(collection_plan), 'collection_job_not_committed')
    record = collection_job['fixed_trial']
    require(record['available'] is True and record['valid'] is True and
            record['terminal_success_verified'] is True and record['terminal_contract'] == 'owner_exit_v2' and
            record['validation_error'] is None, 'collection_owner_terminal_not_verified')
    require(base64.b64decode(record['raw_base64'], validate=True) == raw and record['receipt'] == transport,
            'collection_export_raw_mismatch')
    binding = record['binding']
    for key, value in dict(job_id=collection_job['id'], plan_hash=collection_job['plan_hash'],
                           broker_epoch=collection_plan['broker_epoch'],
                           device_fingerprint=collection_plan['device_fingerprint'],
                           configured_expected_firmware_sha256=elf).items():
        require(binding[key] == value, 'collection_export_binding_mismatch')
    # Both independent parsers remain mandatory. Neither an EXIT nor END alone
    # substitutes for collector-owned complete transport and quiet HOME.
    trial = parse_collected(raw, transport)
    load_collector().parse_collected(raw, transport)
    owner = parse_completion(raw, detail_enabled=expected_detail == 'ON')
    detail = parse_detail(raw.decode('ascii'), expected=expected_detail == 'ON')
    require(proof['fixed_workload']['manifest_sha256'] == trial['manifest_sha256'], 'fixture_manifest_mismatch')
    return dict(recorded_chain_matches=True, source_commit=expected_head, detail_mode=expected_detail,
                app_bin_sha256=image, elf_sha256=elf, flash_job_id=flash_job['id'],
                collection_job_id=collection_plan['job_id'], raw_sha256=hashlib.sha256(raw).hexdigest(),
                owner=owner, trial=trial, detail=detail, hardware_tested=False,
                scope='offline consistency of owner-exported records; instrumentation overhead control; no physical acceptance')


def read_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'duplicate_json_key')
            result[key] = value
        return result
    require(path.stat().st_size <= 2 * 1024 * 1024, 'receipt_capacity')
    return json.loads(path.read_text(), object_pairs_hook=unique)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'raw', 'transport', 'manifest', 'flash-plan', 'flash-job', 'flash-summary', 'collection-plan', 'collection-job'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--expect-head', required=True)
    parser.add_argument('--expect-detail', choices=('ON', 'OFF'), required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    require(args.raw.stat().st_size <= 65536, 'capture_capacity')
    result = verify_recorded_trial(args.raw.read_bytes(), read_json(args.transport), prove(args.build),
                                  *(read_json(getattr(args, key)) for key in
                                    ('manifest', 'flash_plan', 'flash_job', 'flash_summary', 'collection_plan', 'collection_job')),
                                  expected_head=args.expect_head, expected_detail=args.expect_detail)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k not in ('trial', 'detail')}))


if __name__ == '__main__':
    main()
