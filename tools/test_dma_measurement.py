"""Exact app finish + renderer + J1 scope implementations; synthetic time only."""
from pathlib import Path
import subprocess
import tempfile
R=Path(__file__).resolve().parents[1]
s=(R/'main/app_session.c').read_text()
finish=''
for signature in ['static esp_err_t kasane_drain(', 'static esp_err_t kasane_dma_submit(',
                  'static ksn_result kasane_finish(']:
    a=s.index(signature)
    finish+=s[a:s.index('\n}',a)+2]+'\n'
source=(R/'tools/kasane_contract/test_dma_measurement.c').read_text().replace(
    'DMA_FINISH_BOUNDARY','#ifdef KASANE_DMA_OVERLAP\n'+finish+'\n#endif')
with tempfile.TemporaryDirectory(prefix='j1-dma-measure-') as temp:
    p=Path(temp);src=p/'test.c';src.write_text(source)
    for on in (False,True):
        for flags in (['-O2'],['-g','-fsanitize=address,undefined']):
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',*flags,
                '-DPOCKET_DERBY_MEASURE','-DPOCKET_DERBY_DETAIL_MEASURE',
                *(['-DKASANE_DMA_OVERLAP'] if on else []),
                '-I',str(R/'main/ui/kasane'),'-I',str(R/'main/pocket'),
                '-I',str(R/'tools/kasane_contract'),str(src),
                *[str(R/'main'/f) for f in ['ui/kasane/ksn_core.c','ui/kasane/ksn_render.c',
                    'ui/kasane/ksn_blend_pie.c','pocket/derby_measure.c']],
                '-lm','-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
