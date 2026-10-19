"""Offline generated/ELF mode-proof tests; no firmware build or device operation."""
from pathlib import Path
import sys,tempfile,hashlib,struct
from unittest.mock import patch
R=Path(__file__).resolve().parents[1];sys.path.insert(0,str(R/'tools'))
from derby_fixed_workload_chunks import generate,MANIFEST_SHA256
from record_verification import verify_fixed_mode,verify_detail_mode,FIXED_SYMBOLS,DETAIL_REQUIRED,DETAIL_FIXED_SYMBOLS
from verify_diagnostic_build import prove
with tempfile.TemporaryDirectory() as tmp:
    build=Path(tmp);out=build/'generated/derby_fixed_workload';generate(R/'apps/derby',out,True)
    cache=build/'CMakeCache.txt';cache.write_text('POCKET_DERBY_FIXED_WORKLOAD:BOOL=ON\nPOCKET_DERBY_DETAIL_MEASURE:BOOL=ON\n')
    pan=(out/'derby_pan.js').read_bytes()
    if (R/'tools/games/derby_runner_batch.py').exists():
        from games.derby_runner_batch import instrument
        batch=build/'generated/derby_runner_batch';batch.mkdir();pan=instrument(pan.decode()).encode();(batch/'derby_pan.js').write_bytes(pan)
        cache.write_text(cache.read_text()+'POCKET_DERBY_RUNNER_BATCH:BOOL=ON\n')
    good=b'\0'.join((out/f'derby_{n}.js').read_bytes() for n in ('watch','view','play'))+b'\0'+pan+b'\0'+MANIFEST_SHA256.encode()+b'\0'
    elf=build/'cardputer_pocketjs.elf';elf.write_bytes(good)
    with patch('record_verification.elf_symbols',return_value=FIXED_SYMBOLS|DETAIL_REQUIRED|DETAIL_FIXED_SYMBOLS):
        assert verify_fixed_mode(build)['mode']=='ON'
        assert verify_detail_mode(build)['mode']=='ON'
        # Exercise the actual archive/broker proof with detailed generated view.
        cache.write_text(cache.read_text()+'POCKET_DERBY_COMPARE_FIXED_HW:BOOL=ON\n')
        image=bytearray(288);image[0]=0xE9;struct.pack_into('<I',image,32,0xABCD5432)
        image[144:150]=b'v6.0.1';image[176:208]=hashlib.sha256(good).digest()
        (build/'cardputer_pocketjs.bin').write_bytes(image)
        with patch('verify_diagnostic_build.verify_mode',return_value={'telemetry_mode':'ON'}), \
             patch('verify_diagnostic_build.verify_measure_mode',return_value={'measure_mode':'ON'}), \
             patch('verify_diagnostic_build.symbol_u32',return_value=0x2545F491):
            proof=prove(build)
            assert proof['detail_measurement']['mode']=='ON'
            assert proof['comparison_seed']['elf_embedded_view_verified']
        if (R/'tools/verify_derby_runner_batch.py').exists():
            from verify_derby_runner_batch import verify as verify_batch
            with patch('verify_derby_runner_batch.subprocess.check_output',return_value='0 t js_runners\n'):
                assert verify_batch(build,'ON')['detail_mode']=='ON'
        elf.write_bytes(good.replace(b'S.detail(0)',b'S.detail(1)'))
        try:verify_fixed_mode(build)
        except ValueError:pass
        else:raise AssertionError('mixed JS detail code accepted')
        elf.write_bytes(good)
        cache.write_text(cache.read_text().replace('DETAIL_MEASURE:BOOL=ON','DETAIL_MEASURE:BOOL=OFF'))
        try:verify_detail_mode(build)
        except ValueError:pass
        else:raise AssertionError('detail symbols in OFF accepted')
    cache.write_text(cache.read_text().replace('DETAIL_MEASURE:BOOL=OFF','DETAIL_MEASURE:BOOL=ON'))
    with patch('record_verification.elf_symbols',return_value=FIXED_SYMBOLS|DETAIL_REQUIRED-{'dd_suspend'}):
        try:verify_detail_mode(build)
        except ValueError:pass
        else:raise AssertionError('missing continuation pause accepted')
    # Match the real target regression: ON requires the newly linked owner
    # EXIT boundary, while the coarse/OFF contract must still reject it.
    for mode,symbols in [('ON',FIXED_SYMBOLS|DETAIL_REQUIRED),
                         ('ON',FIXED_SYMBOLS|DETAIL_REQUIRED|DETAIL_FIXED_SYMBOLS|{'df_unknown'}),
                         ('OFF',FIXED_SYMBOLS|DETAIL_FIXED_SYMBOLS)]:
        cache.write_text(cache.read_text().replace('DETAIL_MEASURE:BOOL=ON','DETAIL_MEASURE:BOOL='+mode).replace('DETAIL_MEASURE:BOOL=OFF','DETAIL_MEASURE:BOOL='+mode))
        with patch('record_verification.elf_symbols',return_value=symbols):
            try:verify_fixed_mode(build)
            except ValueError as error:assert str(error)=='fixed workload ELF boundary symbols differ'
            else:raise AssertionError('incorrect owner EXIT boundary accepted: '+mode)
print('PASS detailed generated watch/view/play/PAN ELF proof, mixed mode and missing pause rejection (synthetic ELF)')
