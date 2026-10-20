"""Exercise the real CLI with actual CI build images and wholly synthetic records.

No broker, device, admitted source or physical trial is represented by these
receipts. Local prove_dma still verifies the actual BIN/ELF/cache/fixture pair.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/games'))
from derby_manifest_export_fixture import project
from test_derby_manifest_export import sample, recommit_schema_fixture
from verify_derby_recorded_trial import digest
from verify_dma_diagnostic_build import prove_dma


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--expect-head', required=True)
    parser.add_argument('--expect-detail', choices=('OFF', 'ON'), required=True)
    parser.add_argument('--expect-dma', choices=('OFF', 'ON'), required=True)
    args = parser.parse_args()
    proof = prove_dma(args.build, dma_overlap=args.expect_dma, detail_measurement=args.expect_detail)
    records, options = sample(args.expect_detail, args.expect_dma)
    records[2] = proof
    original = copy.deepcopy(records[3])
    original['source_provenance']['commit'] = args.expect_head
    original['artifacts'] = [dict(name=name, **item) for name, item in proof['artifacts'].items()]
    original['diagnostic_modeproof'] = dict(requested_mode=args.expect_detail,
        requested_dma_mode=args.expect_dma, proof=proof, proof_sha256=digest(proof),
        cmake_cache=(args.build / 'CMakeCache.txt').read_bytes().decode('utf-8'))
    wrapper, commitment = project(original)
    records[3] = wrapper['manifest']
    options.update(manifest_export=wrapper, manifest_export_sha256=commitment)
    elf = proof['artifacts']['cardputer_pocketjs.elf']['sha256']
    image = proof['artifacts']['cardputer_pocketjs.bin']['sha256']
    records[4].update(expected_elf_sha256=elf, expected_app_bin_sha256=image)
    records[6]['flash_summary']['observed_elf_sha256_prefix'] = elf[:12]
    records[7]['expected_firmware_sha256'] = elf
    records[7]['collector_binding'].update(source_commit=args.expect_head,
        expected_elf_sha256=elf, expected_app_bin_sha256=image)
    records[8]['fixed_trial']['binding']['expected_firmware_sha256'] = elf
    recommit_schema_fixture(records, options)
    anchor = records[5]['plan_hash']
    with tempfile.TemporaryDirectory(prefix='derby-export-cli-') as directory:
        root = Path(directory)
        files = dict(raw=records[0], transport=records[1], **{
            key: records[index] for key, index in (
                ('flash-plan', 4), ('flash-job', 5), ('flash-summary', 6),
                ('collection-plan', 7), ('collection-job', 8))})
        files['manifest-export'] = dict(manifest=records[3], manifest_export=wrapper,
                                       manifest_export_sha256=commitment)
        paths = {}
        for key, value in files.items():
            path = root / (key + ('.log' if key == 'raw' else '.json'))
            path.write_bytes(value if key == 'raw' else (json.dumps(value) + '\n').encode())
            paths[key] = path
        before = {key: hashlib.sha256(path.read_bytes()).hexdigest() for key, path in paths.items()}
        command = [sys.executable, '-B', str(ROOT / 'tools/games/verify_derby_recorded_trial.py'),
            '--build', str(args.build), '--expect-head', args.expect_head,
            '--expect-detail', args.expect_detail, '--expect-dma', args.expect_dma,
            '--expect-entry-sha256', '9' * 64, '--expect-flash-plan-sha256', anchor,
            '--out', str(root / 'result.json')]
        for key, path in paths.items():
            command += ['--' + key, str(path)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError('synthetic-record CLI failed: ' + result.stderr[-4000:])
        report = json.loads((root / 'result.json').read_text())
        assert report['recorded_chain_matches'] is True
        assert report['manifest_export']['original_manifest_recomputed'] is False
        assert report['app_bin_sha256'] == image and report['elf_sha256'] == elf
        assert report['hardware_tested'] is False
        negatives = []
        for damage in ('anchor', 'null_commitment', 'rehashed_export', 'plan_owner_rehash'):
            changed = list(command)
            if damage == 'anchor':
                changed[changed.index('--expect-flash-plan-sha256') + 1] = 'e' * 64
            elif damage in ('null_commitment', 'rehashed_export'):
                value = copy.deepcopy(files['manifest-export'])
                if damage == 'null_commitment':
                    value['manifest_export_sha256'] = None
                else:
                    value['manifest_export']['manifest']['target'] = 'other'
                    value['manifest_export_sha256'] = digest(value['manifest_export'])
                path = root / 'changed-export.json'
                path.write_text(json.dumps(value))
                changed[changed.index('--manifest-export') + 1] = str(path)
            else:
                plan = dict(records[4], flash_principal='other-owner')
                job = dict(records[5], plan_hash=digest(plan))
                for flag, value in (('--flash-plan', plan), ('--flash-job', job)):
                    path = root / (flag[2:] + '-changed.json')
                    path.write_text(json.dumps(value))
                    changed[changed.index(flag) + 1] = str(path)
            denied = subprocess.run(changed, capture_output=True, text=True)
            assert denied.returncode != 0, 'CLI accepted ' + damage
            negatives.append(damage)
        assert before == {key: hashlib.sha256(path.read_bytes()).hexdigest() for key, path in paths.items()}
    print('DERBY_MANIFEST_EXPORT_CLI_PROOF ' + json.dumps(dict(
        source_head=args.expect_head, detail=args.expect_detail, dma=args.expect_dma,
        actual_images=proof['artifacts'], synthetic_records=True, negative_cases=negatives,
        originals_unchanged=True, hardware_tested=False, production_admitted=False), sort_keys=True))


if __name__ == '__main__':
    main()
