"""Synthetic v1 export regressions with a separately fixed successful-plan anchor."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/games'))
from derby_manifest_export import ABSENT, CONTRACT, MAX_CACHE_BYTES, export_digest, read_export_response
from derby_manifest_export_fixture import project
from test_derby_fixed_completion import receipts
from test_derby_recorded_trial_r2 import synthetic_raw
from verify_derby_recorded_trial import digest, read_json, verify_recorded_trial


def recommit_schema_fixture(args, options):
    """Build a fresh synthetic committed plan for semantic/schema cases only."""
    wrapper = options['manifest_export']
    options['manifest_export_sha256'] = export_digest(wrapper)
    args[4]['manifest_export_sha256'] = options['manifest_export_sha256']
    args[4]['manifest_sha256'] = wrapper['original_manifest_sha256']
    args[5]['plan_hash'] = digest(args[4])
    binding = args[7]['collector_binding']
    binding.update(manifest_sha256=args[4]['manifest_sha256'], flash_plan_hash=args[5]['plan_hash'],
                   modeproof_sha256=digest(args[2]))
    args[8]['fixed_trial']['binding']['flash_receipt'] = copy.deepcopy(binding)
    args[8]['plan_hash'] = digest(args[7])
    args[8]['fixed_trial']['binding']['plan_hash'] = args[8]['plan_hash']


def sample(detail='OFF', dma='ON'):
    args = receipts(synthetic_raw(detail), detail)
    diagnostic = args[2]
    if dma is not None:
        cache = f'POCKET_DERBY_DETAIL_MEASURE:BOOL={detail}\nKASANE_DMA_OVERLAP:BOOL={dma}\n'
        cache_hash = hashlib.sha256(cache.encode()).hexdigest()
        for key in ('build_mode', 'build_measure'):
            diagnostic[key]['cmake_cache_sha256'] = cache_hash
        axes = dict(dma_overlap=dma, detail_measurement=detail)
        args[2] = dict(schema_version=1, proof_contract='derby-dma-diagnostic-v1',
                       requested_axes=axes, verified_axes=dict(axes), diagnostic=diagnostic,
                       dma_overlap=dict(mode=dma, cmake_cache_sha256=cache_hash,
                           symbol_contract='board-present-submit-finish-v1', native_symbols=(
                               ['board_present_finish', 'board_present_submit'] if dma == 'ON' else [])),
                       artifacts=diagnostic['artifacts'], hardware_tested=False)
    manifest = args[3]
    manifest.update(derby_detail_mode=detail, diagnostic_entry_sha256='9' * 64)
    envelope = dict(requested_mode=detail, proof=args[2], proof_sha256=digest(args[2]))
    if dma is not None:
        manifest['derby_dma_mode'] = dma
        envelope.update(requested_dma_mode=dma, cmake_cache=cache)
    manifest['diagnostic_modeproof'] = envelope
    args[7]['collector_binding']['diagnostic_entry_sha256'] = '9' * 64
    if dma is not None:
        args[7]['collector_binding']['derby_dma_mode'] = dma
    wrapper, commitment = project(manifest)
    args[3] = wrapper['manifest']
    options = dict(expected_head='a' * 40, expected_detail=detail, expected_dma=dma,
                   expected_entry_sha256='9' * 64, manifest_export=wrapper,
                   manifest_export_sha256=commitment)
    if dma is not None:
        options['expected_cache_bytes'] = len(cache.encode())
    recommit_schema_fixture(args, options)
    options['expected_flash_plan_hash'] = args[5]['plan_hash']
    return args, options


class ManifestExportTests(unittest.TestCase):
    def rejected(self, args, options):
        with self.assertRaises((ValueError, KeyError, TypeError)):
            verify_recorded_trial(*args, **options)

    def test_six_choices_and_private_preimage_claim(self):
        for detail in ('OFF', 'ON'):
            for dma in (None, 'OFF', 'ON'):
                with self.subTest(detail=detail, dma=dma):
                    args, options = sample(detail, dma)
                    result = verify_recorded_trial(*args, **options)
                    self.assertTrue(result['recorded_chain_matches'])
                    self.assertIs(result['manifest_export']['original_manifest_recomputed'], False)
                    self.assertIs(result['hardware_tested'], False)
                    self.assertEqual(result['manifest_export']['original_manifest_sha256'], args[4]['manifest_sha256'])

    def test_legacy_j1_remains_original_hash_path(self):
        for detail in ('OFF', 'ON'):
            args = receipts(synthetic_raw(detail), detail)
            result = verify_recorded_trial(*args, expected_head='a' * 40, expected_detail=detail)
            self.assertNotIn('manifest_export', result)

    def test_unknown_schema_contract_fields_and_types(self):
        cases = [('schema_version', x) for x in (None, True, False, 1.0, '1', 0, 2)]
        cases += [('contract', x) for x in (None, False, '', CONTRACT + '-unknown')]
        cases += [('original_manifest_sha256', x) for x in (None, True, 'A' * 64, '', 'x' * 64)]
        cases += [('manifest', x) for x in (None, [], True, '')]
        for key, value in cases:
            with self.subTest(key=key, value=value):
                args, options = sample()
                options['manifest_export'][key] = value
                self.rejected(args, options)
        for key in ('extra', 'manifest_export_sha256'):
            args, options = sample()
            options['manifest_export'][key] = 'e' * 64
            self.rejected(args, options)

    def test_commitment_presence_is_not_legacy_default(self):
        for where in ('wrapper', 'response', 'plan'):
            for value in (ABSENT, None, True, False, '', 'A' * 64, 'e' * 64):
                with self.subTest(where=where, value=value):
                    args, options = sample()
                    key = {'wrapper': 'manifest_export', 'response': 'manifest_export_sha256'}
                    if where == 'plan':
                        if value is ABSENT:
                            args[4].pop('manifest_export_sha256')
                        else:
                            args[4]['manifest_export_sha256'] = value
                    elif value is ABSENT:
                        options.pop(key[where])
                    else:
                        options[key[where]] = value
                    self.rejected(args, options)

    def test_original_digest_never_becomes_hash_of_redacted_manifest(self):
        args, options = sample()
        self.assertNotEqual(digest(args[3]), args[4]['manifest_sha256'])
        options['manifest_export']['original_manifest_sha256'] = digest(args[3])
        options['manifest_export_sha256'] = export_digest(options['manifest_export'])
        self.rejected(args, options)

    def test_missing_or_wrong_independent_successful_plan_anchor(self):
        for value in (None, True, '', 'A' * 64, 'e' * 64):
            args, options = sample()
            options['expected_flash_plan_hash'] = value
            self.rejected(args, options)
        for state in ('running', 'failed', 'cancelled', None, True):
            args, options = sample()
            args[5]['state'] = state
            self.rejected(args, options)

    def test_export_rehash_and_entire_mutable_chain_rehash_keep_fixed_anchor(self):
        for target in ('export', 'owner', 'job', 'device', 'source', 'commitment_deleted'):
            with self.subTest(target=target):
                args, options = sample()
                anchor = options['expected_flash_plan_hash']
                if target == 'export':
                    args[3]['source_provenance']['source_sha256'] = 'e' * 64
                elif target == 'source':
                    args[3]['source_provenance']['commit'] = 'e' * 40
                elif target == 'owner':
                    args[4]['flash_principal'] = 'other-owner'
                    args[7]['collector_binding']['principal_id'] = 'other-owner'
                    args[8]['fixed_trial']['binding']['principal_id'] = 'other-owner'
                elif target == 'job':
                    args[4]['job_id'] = args[5]['id'] = 'other-flash'
                    args[7]['flash_job_id'] = args[7]['collector_binding']['flash_job_id'] = 'other-flash'
                    args[6]['requested_job_id'] = 'other-flash'
                elif target == 'device':
                    args[4]['device_id'] = args[7]['collector_binding']['device_id'] = 'other-device'
                if target == 'commitment_deleted':
                    args[4].pop('manifest_export_sha256')
                    args[5]['plan_hash'] = digest(args[4])
                else:
                    recommit_schema_fixture(args, options)
                self.assertEqual(options['expected_flash_plan_hash'], anchor)
                self.rejected(args, options)

    def test_cache_evidence_schema_and_local_hash(self):
        cases = [('retained_private', x) for x in (None, False, 0, 1, 'true')]
        cases += [('bytes', x) for x in (None, True, 0, -1, 1.0, '1', MAX_CACHE_BYTES + 1)]
        cases += [('sha256', x) for x in (None, True, '', 'A' * 64, 'e' * 64)]
        for key, value in cases:
            with self.subTest(key=key, value=value):
                args, options = sample()
                args[3]['diagnostic_modeproof']['cache_evidence'][key] = value
                recommit_schema_fixture(args, options)
                options['expected_flash_plan_hash'] = args[5]['plan_hash']
                self.rejected(args, options)
        for key in ('extra', 'cmake_cache'):
            args, options = sample()
            args[3]['diagnostic_modeproof']['cache_evidence'][key] = 'forbidden'
            recommit_schema_fixture(args, options)
            options['expected_flash_plan_hash'] = args[5]['plan_hash']
            self.rejected(args, options)
        args, options = sample()
        options['expected_cache_bytes'] += 1
        self.rejected(args, options)

    def test_hidden_cache_missing_proof_and_downgrade_fail(self):
        for damage in ('raw_cache', 'cache_missing', 'nested_proof', 'j1_downgrade', 'extra_proof_field'):
            args, options = sample()
            env = args[3]['diagnostic_modeproof']
            if damage == 'raw_cache':
                env['cmake_cache'] = 'private cache must stay private'
            elif damage == 'cache_missing':
                env.pop('cache_evidence')
            elif damage == 'nested_proof':
                env['proof_sha256'] = digest(args[2]['diagnostic'])
            elif damage == 'extra_proof_field':
                env['extra'] = True
            else:
                args[2] = args[2]['diagnostic']
            recommit_schema_fixture(args, options)
            options['expected_flash_plan_hash'] = args[5]['plan_hash']
            self.rejected(args, options)

    def test_commitmentless_dma_has_no_original_or_redacted_fallback(self):
        for detail in ('OFF', 'ON'):
            for dma in ('OFF', 'ON'):
                args, options = sample(detail, dma)
                args[4].pop('manifest_export_sha256')
                options.pop('manifest_export')
                options.pop('manifest_export_sha256')
                options.pop('expected_flash_plan_hash')
                self.rejected(args, options)

    def test_cache_and_export_hashes_survive_serialization(self):
        args, options = sample()
        response = dict(manifest=args[3], manifest_export=options['manifest_export'],
                        manifest_export_sha256=options['manifest_export_sha256'], expired=True, gap=True)
        loaded = json.loads(json.dumps(response))
        manifest, wrapper, commitment = read_export_response(loaded)
        self.assertEqual(export_digest(wrapper), commitment)
        self.assertEqual(manifest, args[3])
        loaded['manifest'] = dict(manifest, target='other')
        with self.assertRaises(ValueError):
            read_export_response(loaded)

    def test_duplicate_keys_nonfinite_json_and_canonical_unicode(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'export.json'
            path.write_text('{"manifest_export":null,"manifest_export":{}}')
            with self.assertRaises(ValueError):
                read_json(path)
        for value in (float('nan'), float('inf'), float('-inf')):
            with self.assertRaises(ValueError):
                export_digest(dict(value=value))
        self.assertEqual(export_digest(dict(z='\u6771\u4eac', a=[True, 1, None])),
                         hashlib.sha256(b'{"a":[true,1,null],"z":"\\u6771\\u4eac"}').hexdigest())

    def test_canonical_payload_equality_rejects_bool_integer_aliases(self):
        for rewrite_hash in (False, True):
            args, options = sample()
            env = args[3]['diagnostic_modeproof']
            env['proof'] = copy.deepcopy(args[2])
            env['proof']['schema_version'] = True
            if rewrite_hash:
                env['proof_sha256'] = digest(env['proof'])
            recommit_schema_fixture(args, options)
            options['expected_flash_plan_hash'] = args[5]['plan_hash']
            self.rejected(args, options)
        args, options = sample()
        options['manifest_export']['manifest'] = copy.deepcopy(args[3])
        options['manifest_export']['manifest']['schema_version'] = True
        recommit_schema_fixture(args, options)
        options['expected_flash_plan_hash'] = args[5]['plan_hash']
        self.rejected(args, options)
        args, options = sample()
        response = dict(manifest=copy.deepcopy(args[3]), manifest_export=options['manifest_export'],
                        manifest_export_sha256=options['manifest_export_sha256'])
        response['manifest']['schema_version'] = True
        with self.assertRaises(ValueError):
            read_export_response(response)

    def test_legacy_rejects_redaction_and_export_metadata_even_after_rehash(self):
        for key in ('cache_evidence', 'cmake_cache', 'requested_dma_mode', 'manifest_export', 'manifest_export_sha256'):
            args, options = sample(dma=None)
            args[4].pop('manifest_export_sha256')
            options.pop('manifest_export')
            options.pop('manifest_export_sha256')
            options.pop('expected_flash_plan_hash')
            if key in ('manifest_export', 'manifest_export_sha256'):
                args[3][key] = None
            else:
                args[3]['diagnostic_modeproof'][key] = None
            args[4]['manifest_sha256'] = digest(args[3])
            args[5]['plan_hash'] = digest(args[4])
            args[7]['collector_binding'].update(manifest_sha256=args[4]['manifest_sha256'],
                                              flash_plan_hash=args[5]['plan_hash'])
            args[8]['fixed_trial']['binding']['flash_receipt'] = copy.deepcopy(args[7]['collector_binding'])
            args[8]['plan_hash'] = digest(args[7])
            args[8]['fixed_trial']['binding']['plan_hash'] = args[8]['plan_hash']
            self.rejected(args, options)


if __name__ == '__main__':
    unittest.main()
