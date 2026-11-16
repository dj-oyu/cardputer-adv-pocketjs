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
static ksn_app_lease loan_app;
static unsigned char *loan_tail;
static unsigned sends;
static const uint16_t *borrowed_strip;
static ksn_result loan_send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *p){
    sends++;borrowed_strip=p;
    return send(ctx,y,rows,p);
}
static ksn_result finish(void *ctx){
    (void)ctx;finish_count++;
    if(loan_app.value){
        if(ksn_runtime_app_detach(loan_app)!=KSN_BUSY)abort();
        if(!loan_tail||loan_tail[0]!=0xa5||!borrowed_strip)abort();
        volatile uint16_t observed=borrowed_strip[0];(void)observed;
    }
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
    CHECK(finish_count==3&&!ksn_core_has_submission(&runtime->core));''')
# Exercise real APP detach separately from global runtime shutdown. The display
# retains the strip until finish succeeds; the APP tail models owned source data.
needle = '    /* Exhaustion fails before allocation; it must not wrap to a stale lease. */'
assert source.count(needle) == 1
source=source.replace(needle,'''    {
        void *tail=NULL;
        CHECK(ksn_runtime_app_attach_tail(&loan_app,32,&tail)==KSN_OK);
        loan_tail=tail;loan_tail[0]=0xa5;
        ksn_view *owner=ksn_runtime_app_view(loan_app);
        CHECK(ksn_view_begin(owner,KSN_REPLACE,&tx)==KSN_OK);
        CHECK(ksn_view_background(owner,tx,0x00ff00ff)==KSN_OK);
        CHECK(ksn_view_submit(owner,tx)==KSN_OK);
        ksn_runtime_app_activate(loan_app);
        ksn_display_port loan_port={NULL,buffer,loan_send,240,135,8,NULL,NULL};
        fail_finish=1;
        CHECK(ksn_runtime_present_complete(&loan_port,NULL,false,&completion,&stats)==KSN_IO);
        unsigned retained_blocks=blocks,retained_sends=sends;
        CHECK(ksn_runtime_app_detach(loan_app)==KSN_BUSY&&blocks==retained_blocks);
        CHECK(ksn_runtime_app_view(loan_app)==owner&&loan_tail[0]==0xa5);
        CHECK(ksn_runtime_present(&loan_port,&stats)==KSN_BUSY);
        CHECK(ksn_runtime_present_backdrop(&loan_port,NULL,false,&stats)==KSN_BUSY);
        CHECK(ksn_runtime_present_complete(&loan_port,NULL,false,NULL,&stats)==KSN_BUSY);
        CHECK(sends==retained_sends);
        fail_finish=1;
        CHECK(ksn_runtime_present_complete(&loan_port,NULL,false,&completion,&stats)==KSN_IO);
        CHECK(sends==retained_sends&&blocks==retained_blocks);
        /* Fresh context object, no saved pointer to the old completion struct. */
        ksn_display_completion fresh={&loan_port,finish};
        CHECK(ksn_runtime_present_complete(&loan_port,NULL,false,&fresh,&stats)==KSN_OK);
        CHECK(sends>retained_sends);
        CHECK(ksn_runtime_app_detach(loan_app)==KSN_OK&&!blocks);
        loan_app=(ksn_app_lease){0};loan_tail=NULL;borrowed_strip=NULL;
    }
''' + needle)
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
print('PASS APP detach retains tail until successful drain; blocked retries do not send; completion IO retains submission; runtime shutdown busy during finish and pending retry; success ACKs exact retry')
