// What `js=` actually responds to, on a host, with the real quickjs-ng.
//
//   wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs &&
//     bash tools/build_js_ledger.sh && /tmp/test-js-ledger apps/hello/main.js ..."
//
// WHY THIS EXISTS. `js=` is pocketjs_guest_stats()'s heap_used, which is
// JS_ComputeMemoryUsage()'s malloc_size (.cache/pocketjs/.../guest.c:435), and
// pocket.device.metrics() reports the same field as jsHeapBytes. Every memory
// argument on this device reaches for it. On 2026-09-09 two builds whose app
// sources differed by 1,418 bytes -- one of them with every ui.* call removed --
// reported a byte-identical 107,587, which means the field did not respond to
// the thing two people had assumed dominated it.
//
// This measures the ledger at each stage a guest goes through, so the fixed part
// and the per-source part can be told apart without a board. It cannot see the
// native side (the pocket.* namespaces are C objects built by the firmware), so
// a source that reads pocket.* costs MORE on the device than it does here. That
// gap is the point: what this prints is the JS-only floor and the JS-only slope.
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t ledger(JSRuntime *rt) {
    JSMemoryUsage u;
    memset(&u,0,sizeof u);
    JS_ComputeMemoryUsage(rt,&u);
    return (int64_t)u.malloc_size;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f=fopen(path,"rb");
    if(!f) { perror(path); exit(1); }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char *b=malloc((size_t)n+1);
    if(fread(b,1,(size_t)n,f)!=(size_t)n) { perror(path); exit(1); }
    b[n]='\0'; *len=(size_t)n; fclose(f);
    return b;
}

int main(int argc, char **argv) {
    JSRuntime *rt=JS_NewRuntime();
    int64_t after_rt=ledger(rt);
    JSContext *ctx=JS_NewContext(rt);
    int64_t after_ctx=ledger(rt);
    printf("runtime only          %8lld\n",(long long)after_rt);
    printf("+ context/intrinsics  %8lld   (+%lld)\n",
           (long long)after_ctx,(long long)(after_ctx-after_rt));
    printf("\n%-28s %8s %9s %9s\n","source","bytes","compiled","+ledger");
    for(int i=1;i<argc;i++) {
        size_t n=0;
        char *src=slurp(argv[i],&n);
        int64_t before=ledger(rt);
        // Compile only: this is the bytecode, the atoms and the constant pool,
        // with none of what running it would build. If the ledger does not move
        // here, source size is not what it is made of.
        JSValue fn=JS_Eval(ctx,src,n,argv[i],JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);
        int64_t after=ledger(rt);
        printf("%-28s %8zu %9lld %9lld%s\n",
               strrchr(argv[i],'/')?strrchr(argv[i],'/')+1:argv[i],
               n,(long long)after,(long long)(after-before),
               JS_IsException(fn)?"   COMPILE FAILED":"");
        JS_FreeValue(ctx,fn);
        free(src);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
