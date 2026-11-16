"""Fault-test the real runtime's completion IO/shutdown/retry contract on the host.

This preserves the existing runtime assertions. A mock finish failure is not
proof that physical DMA has drained; alpha source loans need separate tracking.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "tools/kasane_contract/test_runtime.c").read_text()
include = '"../../main/ui/kasane/ksn_runtime.c"'
assert source.count(include) == 1
source = source.replace(include, '"' + str(root / 'main/ui/kasane/ksn_runtime.c') + '"')
assert source.count('int main(void){') == 1
source = source.replace('int main(void){', '''
static int fail_finish=1;
static unsigned finish_count;
static ksn_result finish(void *ctx){
    (void)ctx;finish_count++;
    if(ksn_runtime_shutdown()!=KSN_BUSY)abort();
    if(fail_finish){fail_finish=0;return KSN_IO;}
    return KSN_OK;
}
static const ksn_display_completion completion={NULL,finish};
int main(void){''')
needle = 'CHECK(ksn_runtime_present(&port,&stats)==KSN_OK&&pixels[0]==0x07e0);'
assert source.count(needle) == 1
source = source.replace(needle, '''
    ksn_tx retry_ticket=ksn_core_poll(&runtime->core).ticket;
    CHECK(ksn_runtime_present_complete(&port,NULL,false,&completion,&stats)==KSN_IO);
    CHECK(ksn_core_has_submission(&runtime->core));
    CHECK(ksn_core_poll(&runtime->core).ticket.value==retry_ticket.value);
    CHECK(ksn_runtime_shutdown()==KSN_BUSY);
    CHECK(ksn_runtime_present_complete(&port,NULL,false,&completion,&stats)==KSN_OK&&pixels[0]==0x07e0);
    CHECK(finish_count==2&&!ksn_core_has_submission(&runtime->core));''')
with tempfile.TemporaryDirectory(prefix='dma-failure-') as temp:
    temp = Path(temp)
    src = temp / 'runtime.c'
    src.write_text(source)
    components = ['ksn_core.c','ksn_render.c','ksn_blend_pie.c','ksn_cache.c','ksn_modal.c','ksn_view.c']
    for flags in [['-O2','-fstrict-aliasing'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover']]:
        exe = temp / 'runtime'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
            '-DKASANE_DMA_OVERLAP=1',*flags,'-I',str(root/'main/ui/kasane'),
            *[str(root/'main/ui/kasane'/s) for s in components],str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
print('PASS completion IO retains submission; runtime shutdown busy during finish and pending retry; success ACKs exact retry')
