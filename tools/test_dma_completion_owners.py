"""Reuse existing owner contracts through the new completion APIs.

The original assertions cover cache/modal/lease lifetime, allocation ceilings,
repair, native procedural publication, and teardown. Only the transport and
entry points are changed; no expected results are weakened.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="dma-owners-") as temp:
    temp = Path(temp)
    runtime = (root / "tools/kasane_contract/test_runtime.c").read_text()
    runtime = runtime.replace('"../../main/ui/kasane/ksn_runtime.c"',
                              '"' + str(root / "main/ui/kasane/ksn_runtime.c") + '"')
    runtime = runtime.replace("int main(void){", """
static unsigned finishes;
static ksn_result finish(void *ctx){
    (void)ctx;finishes++;
    if(runtime&&ksn_core_has_submission(&runtime->core)){
        if(ksn_core_poll(&runtime->core).status!=KSN_SUBMITTED)abort();
        if(ksn_runtime_shutdown()!=KSN_BUSY)abort();
    }
    return KSN_OK;
}
static const ksn_display_completion completion={NULL,finish};
int main(void){""")
    runtime = runtime.replace("ksn_runtime_present(&port,&stats)",
                              "ksn_runtime_present_complete(&port,NULL,false,&completion,&stats)")
    runtime = runtime.replace('    puts("native runtime:',
                              '    CHECK(finishes==3);\n    puts("DMA completion native runtime:')
    source = temp / "runtime.c"
    source.write_text(runtime)
    for flags in [["-O2", "-fstrict-aliasing"], ["-g", "-fsanitize=address,undefined"]]:
        exe = temp / "runtime"
        sources = ["ksn_core.c", "ksn_render.c", "ksn_blend_pie.c", "ksn_cache.c",
                   "ksn_modal.c", "ksn_view.c"]
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DKASANE_DMA_OVERLAP=1", *flags,
                        "-I", str(root / "main/ui/kasane"),
                        *[str(root / "main/ui/kasane" / s) for s in sources],
                        str(source), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

    kobako = (root / "tools/test_kobako_kasane.c").read_text()
    kobako = '#include <assert.h>\n#include "kobako_shiki_frame.h"\n' + kobako
    boundary = "static uint16_t *get_strip(void *opaque)"
    assert boundary in kobako
    kobako = kobako.replace(boundary, """
static uint16_t dma_tx[240*8];
static uint16_t dma_y,dma_rows;
static bool dma_pending,dma_candidate;
static void dma_flush(void){
    if(dma_pending)memcpy(panel_pixels+dma_y*240,dma_tx,dma_rows*240*2);
    dma_pending=false;
}
static ksn_result dma_finish(void *ctx){
    (void)ctx;
    if(dma_candidate)assert(kobako_proc_pending());
    dma_flush();return KSN_OK;
}
""" + boundary)
    original = "memcpy(panel_pixels+y*240,pixels,rows*240*sizeof(*pixels));"
    assert original in kobako
    kobako = kobako.replace(original, """dma_flush();
    memcpy(dma_tx,pixels,rows*240*sizeof(*pixels));
    dma_y=y;dma_rows=rows;dma_pending=true;""")
    original = "return kobako_kasane_present(&port,stats);"
    assert original in kobako
    kobako = kobako.replace(original, """
    dma_candidate=kobako_proc_pending();
    const ksn_display_completion completion={NULL,dma_finish};
    ksn_result result=kobako_kasane_present_complete(&port,&completion,stats);
    assert(!dma_pending);dma_candidate=false;return result;""")
    # Unused surface families are not exercised by this retained harness.
    # Keep their stubs explicit; the procedural implementation remains real.
    kobako += """
esp_err_t kobako_grid_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
esp_err_t kobako_video_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
esp_err_t kobako_pixel_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void kobako_grid_reset(void){}
void kobako_video_reset(void){}
void kobako_pixel_reset(void){}
bool kobako_grid_pending(void){return false;}
bool kobako_video_pending(void){return false;}
bool kobako_pixel_pending(void){return false;}
void kobako_grid_present_result(ksn_result r){(void)r;}
void kobako_video_present_result(ksn_result r){(void)r;}
void kobako_pixel_present_result(ksn_result r){(void)r;}
void kobako_grid_source_invalidated(uint32_t r){(void)r;}
"""
    kobako = kobako.replace("    repair_tests();", """
    repair_tests();
    kobako_kasane_reset();
    /* The frame functions live on kasane.shiki since Shiki v2.0; this harness
     * has no Shiki engine, so they go on a plain object and the one-pixel
     * line goes in through the native path kobako.shapes uses. */
    {
        JSValue g=JS_GetGlobalObject(ctx),dp=JS_NewObject(ctx);
        check(kobako_proc_frame_install(ctx,dp)==ESP_OK,"DMA frame functions install");
        JS_SetPropertyStr(ctx,g,"dp",dp);JS_FreeValue(ctx,g);
    }
    kobako_proc_mark dma_mark;
    bool dma_ok=run("dp.beginFrame(0);");
    dma_ok=dma_ok&&!JS_IsException(kobako_proc_native_begin(ctx,"dma",&dma_mark));
    dma_ok=dma_ok&&!JS_IsException(kobako_proc_native_line(ctx,"dma",10,10,10,10,65535));
    check(dma_ok&&run("dp.commit();"),"DMA native procedural candidate submits");
    check(kobako_proc_pending(),"DMA native candidate remains pending before transfer");
    check(present(&stats)==KSN_OK&&!kobako_proc_pending()&&panel_pixels[10*240+10]==65535,
          "DMA finish precedes native procedural promotion and panel equality");
""")
    source = temp / "kobako.c"
    source.write_text(kobako)
    env = dict(os.environ, TEST_SOURCE=str(source), OUT=str(temp / "kobako"),
               CACHE=str(temp / "qjs"),
               CFLAGS="-O1 -g -fsanitize=address,undefined -DKASANE_DMA_OVERLAP=1")
    subprocess.run(["bash", "tools/build_kasane_test.sh"], cwd=root, env=env, check=True)
    subprocess.run([str(temp / "kobako")], cwd=root, check=True)
