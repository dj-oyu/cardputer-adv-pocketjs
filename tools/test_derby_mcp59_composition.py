"""Compose public MCP59 consumer code with host r2, using synthetic records only.

PYTHONPATH must include the pinned consumer's src/ and tests/ directories.
No broker instance, job execution, serial port or production registry is changed.
"""
import copy
from pathlib import Path
import sys

import pytest
from test_collector_binding import chain as chain
from test_dma_collector_binding import make_dma_chain as make_dma_chain, rebind
from test_dma_modeproof import DMA_ENTRY, dma_image_fixture

from cardputer_mcp import diagnostic_registry as registry
from cardputer_mcp.collector_binding import (
    collector_live, prepare_collector, resolve_collector, validate_collector_schema,
)
from cardputer_mcp.diagnostic_dma_modeproof import collect_dma_modeproof

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/games'))
from test_derby_fixed_completion import receipts
from test_derby_recorded_trial_r2 import synthetic_raw
from derby_fixed_workload_chunks import MANIFEST_SHA256
from verify_derby_recorded_trial import digest, verify_recorded_trial
from derby_manifest_export_fixture import project


def complete_chain(chain):
    """Add public export fields absent from the consumer's minimal fixture."""
    manifest = chain.manifest
    manifest.update(schema_version=1, idf_version='6.0.1', target='esp32s3')
    manifest['source_provenance']['source_sha256'] = '2' * 64
    envelope = manifest['diagnostic_modeproof']
    proof = envelope['proof']
    diagnostic = proof.get('diagnostic', proof)
    diagnostic['fixed_workload']['manifest_sha256'] = MANIFEST_SHA256
    envelope['proof_sha256'] = digest(proof)
    entry = registry.diagnostic_entry(manifest['source_provenance'])
    for target in (chain.build, manifest):
        target['diagnostic_entry_sha256'] = entry.sha256
    chain.flash.update(backend='serial', broker_epoch='3' * 32, session_id='synthetic-session')
    chain.summary.update(boot_observed=True, target='esp32s3', observed_idf_version='6.0.1',
                         observed_elf_sha256_prefix=chain.flash['expected_elf_sha256'][:12])
    rebind(chain)
    return entry


def exported_args(chain, detail, dma):
    entry = complete_chain(chain)
    export_options = {}
    if dma is not None:
        # The pinned MCP59 has no v1 producer. This is explicitly a synthetic
        # contract projection; future producer-head composition is a separate gate.
        wrapper, commitment = project(chain.manifest)
        chain.flash['manifest_export_sha256'] = commitment
        rebind(chain)
        export_options = dict(manifest_export=wrapper, manifest_export_sha256=commitment,
                              expected_flash_plan_hash=chain.jobs['public-flash']['plan_hash'])
    # These are actual consumer outputs, never a hand-written binding substitute.
    details = prepare_collector(chain.resident, 'public-flash', 'owner')
    plan = dict(test='derby_fixed_workload', **details)
    validate_collector_schema(plan)
    assert collector_live(chain.resident, plan, 'owner')
    assert details['collector_binding'] == resolve_collector(chain.resident, 'public-flash', 'owner')
    args = receipts(synthetic_raw(detail), detail)
    args[2] = copy.deepcopy(chain.manifest['diagnostic_modeproof']['proof'])
    args[3] = copy.deepcopy(chain.manifest)
    if dma is not None:
        args[3] = copy.deepcopy(wrapper['manifest'])
    args[4] = copy.deepcopy(chain.flash)
    args[5] = dict(id='public-flash', **chain.jobs['public-flash'])
    args[6] = dict(requested_job_id='public-flash', flash_summary=copy.deepcopy(chain.summary))
    args[7].update(details)
    args[7].update(session_id=chain.flash['session_id'], broker_epoch=chain.flash['broker_epoch'])
    args[8]['plan_hash'] = digest(args[7])
    binding = args[8]['fixed_trial']['binding']
    binding.update(plan_hash=args[8]['plan_hash'], device_fingerprint=chain.flash['device_fingerprint'],
                   expected_firmware_sha256=chain.flash['expected_elf_sha256'], principal_id='owner',
                   flash_receipt=copy.deepcopy(details['collector_binding']))
    options = dict(expected_head=entry.commit, expected_detail=detail,
                   expected_entry_sha256=entry.sha256, expected_dma=dma, **export_options)
    return args, options


@pytest.mark.parametrize('detail', ['OFF', 'ON'])
@pytest.mark.parametrize('dma', ['OFF', 'ON'])
def test_actual_consumer_to_host_four_modes(make_dma_chain, detail, dma):
    args, options = exported_args(make_dma_chain(detail, dma), detail, dma)
    assert verify_recorded_trial(*args, **options)['recorded_chain_matches'] is True


def test_modern_entry_bound_j1_without_dma_axis(chain):
    args, options = exported_args(chain, 'OFF', None)
    assert 'derby_dma_mode' not in args[7]['collector_binding']
    assert verify_recorded_trial(*args, **options)['recorded_chain_matches'] is True
    args[7]['collector_binding']['derby_dma_mode'] = None
    args[8]['plan_hash'] = digest(args[7])
    args[8]['fixed_trial']['binding']['plan_hash'] = args[8]['plan_hash']
    with pytest.raises(ValueError):
        verify_recorded_trial(*args, **options)


@pytest.mark.parametrize('field', ['derby_detail_mode', 'derby_dma_mode', 'diagnostic_entry_sha256'])
@pytest.mark.parametrize('record', ['build', 'manifest'])
@pytest.mark.parametrize('damage', ['missing', 'null', 'different'])
def test_actual_consumer_axes_and_entry_after_rehash(make_dma_chain, field, record, damage):
    chain = make_dma_chain('OFF', 'ON')
    complete_chain(chain)
    target = getattr(chain, record)
    if damage == 'missing':
        target.pop(field)
    else:
        target[field] = None if damage == 'null' else ('e' * 64 if field.endswith('sha256') else
                                                       ('ON' if target[field] == 'OFF' else 'OFF'))
    rebind(chain)
    with pytest.raises(ValueError):
        resolve_collector(chain.resident, 'public-flash', 'owner')


@pytest.mark.parametrize('field', ['derby_dma_mode', 'diagnostic_entry_sha256', 'modeproof_sha256',
                                 'source_commit', 'source_receipt', 'principal_id', 'device_id',
                                 'firmware_receipt'])
@pytest.mark.parametrize('record', ['plan', 'captured'])
@pytest.mark.parametrize('damage', ['missing', 'null', 'different'])
def test_host_rejects_changed_actual_binding(make_dma_chain, field, record, damage):
    args, options = exported_args(make_dma_chain('OFF', 'ON'), 'OFF', 'ON')
    target = args[7]['collector_binding'] if record == 'plan' else args[8]['fixed_trial']['binding']['flash_receipt']
    if damage == 'missing':
        target.pop(field)
    else:
        target[field] = None if damage == 'null' else 'e' * 64
    args[8]['plan_hash'] = digest(args[7])
    args[8]['fixed_trial']['binding']['plan_hash'] = args[8]['plan_hash']
    with pytest.raises((ValueError, KeyError)):
        verify_recorded_trial(*args, **options)


@pytest.mark.parametrize('damage', ['nested_hash', 'downgrade', 'public_redaction', 'missing_expectation'])
def test_host_outer_proof_and_committed_export_required(make_dma_chain, damage):
    args, options = exported_args(make_dma_chain('OFF', 'ON'), 'OFF', 'ON')
    if damage == 'nested_hash':
        for binding in (args[7]['collector_binding'], args[8]['fixed_trial']['binding']['flash_receipt']):
            binding['modeproof_sha256'] = digest(args[2]['diagnostic'])
    elif damage == 'downgrade':
        args[2] = args[2]['diagnostic']
    elif damage == 'missing_expectation':
        options['expected_entry_sha256'] = None
    else:
        # Redacted DMA exports have no legacy fallback without their commitment.
        options.pop('manifest_export')
        options.pop('manifest_export_sha256')
        args[4].pop('manifest_export_sha256')
    args[8]['plan_hash'] = digest(args[7])
    args[8]['fixed_trial']['binding']['plan_hash'] = args[8]['plan_hash']
    with pytest.raises((ValueError, KeyError)):
        verify_recorded_trial(*args, **options)


@pytest.mark.parametrize('damage', ['cache_string', 'cache_untyped', 'bin', 'elf', 'current', 'source', 'owner', 'device'])
def test_actual_consumer_retained_chain_tamper(make_dma_chain, damage):
    chain = make_dma_chain('OFF', 'ON')
    complete_chain(chain)
    if damage.startswith('cache_'):
        suffix = ':STRING=ON' if damage == 'cache_string' else '=ON'
        chain.manifest['diagnostic_modeproof']['cmake_cache'] += 'KASANE_DMA_OVERLAP' + suffix + '\n'
    elif damage in ('bin', 'elf'):
        chain.files['cardputer_pocketjs.' + damage] += b'changed'
    elif damage == 'current':
        chain.current['receipt'] = 'e' * 64
    elif damage == 'source':
        chain.build['source_provenance']['commit'] = 'f' * 40
    elif damage == 'owner':
        chain.flash['flash_principal'] = 'other-owner'
    else:
        chain.flash['device_id'] = 'other-device'
    rebind(chain)
    with pytest.raises(ValueError):
        resolve_collector(chain.resident, 'public-flash', 'owner')


def test_production_registry_rejects_unadmitted_dma_source(tmp_path):
    source, artifacts, _ = dma_image_fixture(tmp_path, 'OFF', 'ON')
    assert all(entry.commit != DMA_ENTRY.commit for entry in registry._ENTRIES)
    with pytest.raises(ValueError):
        collect_dma_modeproof(tmp_path, 'OFF', 'ON', source, artifacts)
