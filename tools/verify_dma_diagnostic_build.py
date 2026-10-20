"""Archive-safe DMA proof with two explicit, independent requested axes.

This contract does not extend a broker source allowlist or authorize flashing.
The enclosing source receipt must bind this proof to an approved full head.
"""
import argparse
import hashlib
import json
from pathlib import Path

from record_verification import command, elf_symbols
from verify_diagnostic_build import prove

CONTRACT = 'derby-dma-diagnostic-v1'
DMA_SYMBOLS = frozenset(('board_present_submit', 'board_present_finish'))
AXES = {'dma_overlap': 'KASANE_DMA_OVERLAP',
        'detail_measurement': 'POCKET_DERBY_DETAIL_MEASURE'}


def dma_function_symbols(build):
    """Require linked code symbols, not data bearing the same names."""
    text = command('xtensa-esp32s3-elf-nm', '--defined-only', str(build / 'cardputer_pocketjs.elf'))
    return {parts[-1] for line in text.splitlines()
            if len(parts := line.split()) >= 3 and parts[-2] in ('T', 't', 'W', 'w')}


def prove_dma(build, *, dma_overlap, detail_measurement):
    requested = dict(dma_overlap=dma_overlap, detail_measurement=detail_measurement)
    if any(type(value) is not str or value not in ('OFF', 'ON') for value in requested.values()):
        raise ValueError('explicit typed OFF/ON DMA and detail axes required')
    cache = (build / 'CMakeCache.txt').read_bytes()
    lines = cache.decode().splitlines()
    for axis, setting in AXES.items():
        values = [line for line in lines if line.startswith((setting + ':', setting + '='))]
        if values != [setting + ':BOOL=' + requested[axis]]:
            raise ValueError('requested/cache axis mismatch: ' + axis)
    # Existing proof checks generated fixture, fixed seed, owner completion,
    # linked detail value, SDK and the actual BIN-to-ELF hash pair.
    diagnostic = prove(build)
    if diagnostic['detail_measurement']['mode'] != detail_measurement:
        raise ValueError('verified detail axis mismatch')
    symbols = DMA_SYMBOLS & elf_symbols(build)
    if symbols != (DMA_SYMBOLS if dma_overlap == 'ON' else set()):
        raise ValueError('requested/cache/linked DMA axis mismatch')
    if dma_overlap == 'ON' and not DMA_SYMBOLS <= dma_function_symbols(build):
        raise ValueError('DMA symbols must be linked functions')
    return dict(schema_version=1, proof_contract=CONTRACT, requested_axes=requested,
                verified_axes=dict(requested),
                dma_overlap=dict(mode=dma_overlap, symbol_contract='board-present-submit-finish-v1',
                                 native_symbols=sorted(symbols),
                                 cmake_cache_sha256=hashlib.sha256(cache).hexdigest()),
                diagnostic=diagnostic, artifacts=diagnostic['artifacts'], hardware_tested=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--expect-dma', choices=('OFF', 'ON'), required=True)
    parser.add_argument('--expect-detail', choices=('OFF', 'ON'), required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    report = prove_dma(build, dma_overlap=args.expect_dma, detail_measurement=args.expect_detail)
    text = json.dumps(report, sort_keys=True)
    (build / 'dma-diagnostic-mode-proof.json').write_text(text + '\n')
    print('DERBY_DMA_DIAGNOSTIC_BUILD_PROOF ' + text, flush=True)


if __name__ == '__main__':
    main()
