"""Verify CI build artifacts with the pinned MCP59 consumer, offline only.

The process-local test entry describes an unadmitted candidate. It does not
modify a broker registry, register a source, or authorize a build or flash.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
from unittest.mock import patch

from cardputer_mcp import diagnostic_registry as registry
from cardputer_mcp.diagnostic_dma_modeproof import CONTRACT, collect_dma_modeproof


def verify(build, head, detail, dma):
    if re.fullmatch(r'[a-f0-9]{40}', head) is None:
        raise ValueError('full candidate head required')
    stamp = json.loads((build / 'dma-mode-proof.json').read_text())
    if (stamp['source_head'], stamp['detail_mode'], stamp['dma_mode']) != (head, detail, dma):
        raise ValueError('candidate source/axis stamp mismatch')
    source = dict(repository='dj-oyu/cardputer-adv-pocketjs', commit=head)
    try:
        registry.diagnostic_entry(source)
    except ValueError:
        pass
    else:
        raise ValueError('candidate unexpectedly admitted by production policy')
    artifacts = []
    for suffix in ('bin', 'elf'):
        name = 'cardputer_pocketjs.' + suffix
        raw = (build / name).read_bytes()
        receipt = dict(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())
        if stamp['images'][suffix] != receipt:
            raise ValueError('candidate image/stamp mismatch')
        artifacts.append(dict(name=name, **receipt))
    entry = registry.DiagnosticEntry(source['repository'], head, contract=CONTRACT, dma_modes=('ON', 'OFF'))
    # Only this offline verification process sees this synthetic policy entry.
    with patch.object(registry, '_ENTRIES', (*registry._ENTRIES, entry)):
        envelope = collect_dma_modeproof(build, detail, dma, source, artifacts)
    return dict(source_head=head, detail_mode=detail, dma_mode=dma,
                consumer_contract=CONTRACT, proof_sha256=envelope['proof_sha256'],
                artifacts=artifacts, production_admitted=False, hardware_tested=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--expect-head', required=True)
    parser.add_argument('--expect-detail', choices=('ON', 'OFF'), required=True)
    parser.add_argument('--expect-dma', choices=('ON', 'OFF'), required=True)
    args = parser.parse_args()
    print('DERBY_MCP59_CONSUMER_PROOF ' + json.dumps(
        verify(args.build, args.expect_head, args.expect_detail, args.expect_dma), sort_keys=True))


if __name__ == '__main__':
    main()
