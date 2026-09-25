// The tutorial's chapters and the Playground's template, run the way the
// firmware runs them: prelude and lesson as two evaluations in one realm, then
// frame() turns, against the real QuickJS and the real pocket.kasane surface.
//
// Written when both were moved off the legacy ui.* API (which the firmware no
// longer has): a lesson that throws, or draws nothing, fails a beginner on a
// board with no debugger, so it is checked here before anyone types it.
//
//   wsl -e bash -lc "cd <repo> && bash tools/build_lessons_test.sh && /tmp/test-lessons"
#include "pocket_kasane.h"
#include "pocket_av.h"
#include "system/sys_device.h"
#include "ui/kasane/ksn_runtime.h"
#include "text/ksn_font.h"
#include "lessons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void host_capabilities_clear(void);
int32_t pocket_av_ui_current_player(void){return 0;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out){
    (void)id;(void)out;return false;
}
bool sys_device_clock_read(sys_clock_state *out){(void)out;return false;}
void *__real_calloc(size_t count,size_t size);
void __real_free(void *ptr);
void __wrap_free(void *ptr) { __real_free(ptr); }
void *__wrap_calloc(size_t count,size_t size) { return __real_calloc(count,size); }

static JSRuntime *rt;
static JSContext *ctx;
static unsigned failures;
static uint16_t strip_pixels[240*8];
static uint16_t panel[240*135];
static char printed[512];

static void check(bool ok,const char *what,unsigned chapter) {
    printf("%s ch%u %s\n",ok?"ok  ":"FAIL",chapter,what);
    if(!ok)failures++;
}

static JSValue js_print(JSContext *c,JSValueConst self,int argc,JSValueConst *argv) {
    (void)self;
    for(int i=0;i<argc;i++) {
        const char *s=JS_ToCString(c,argv[i]);
        if(s) {
            size_t used=strlen(printed);
            snprintf(printed+used,sizeof printed-used,"%s\n",s);
            JS_FreeCString(c,s);
        }
    }
    return JS_UNDEFINED;
}

static bool eval(const char *source,const char *name) {
    if(!source) return true;
    JSValue v=JS_Eval(ctx,source,strlen(source),name,JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(v);
    if(!ok) {
        JSValue e=JS_GetException(ctx);
        const char *t=JS_ToCString(ctx,e);
        printf("    %s threw: %s\n",name,t?t:"?");
        if(t)JS_FreeCString(ctx,t);
        JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,v);
    pocket_kasane_end_turn();
    return ok;
}

static uint16_t *get_strip(void *o) { (void)o; return strip_pixels; }
static ksn_result send_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *pixels) {
    (void)o; memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels)); return KSN_OK;
}
static bool present(void) {
    if(!pocket_kasane_needs_present()) return true;
    ksn_display_port port={.strip=get_strip,.present=send_strip,
                           .width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats stats;
    return pocket_kasane_present(&port,&stats)==KSN_OK;
}

// frame(buttons) as app_tick() hands it: one turn, then the display.
static bool frame(unsigned buttons) {
    char call[64];
    snprintf(call,sizeof call,"typeof frame==='function'&&frame(%u)",buttons);
    return eval(call,"frame.js")&&present();
}

static unsigned distinct_colours(void) {
    uint16_t seen[8]; unsigned n=0;
    for(size_t i=0;i<240*135&&n<8;i++) {
        unsigned j=0; while(j<n&&seen[j]!=panel[i]) j++;
        if(j==n) seen[n++]=panel[i];
    }
    return n;
}

static void open_session(void) {
    rt=JS_NewRuntime(); ctx=JS_NewContext(rt); host_capabilities_clear();
    memset(panel,0,sizeof panel); printed[0]=0;
    pocket_kasane_install(ctx,NULL);   // the stub hangs it on globalThis.kasane
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"print",JS_NewCFunction(ctx,js_print,"print",1));
    JS_FreeValue(ctx,g);
    eval("globalThis.pocket={kasane:globalThis.kasane}","pocket.js");
}
static void close_session(void) {
    pocket_kasane_reset(); JS_FreeContext(ctx); JS_FreeRuntime(rt);
}

// The Playground's template is a static in codeedit.c; read it out of the
// source rather than keep a copy that could drift.
static char *template_source(void) {
    FILE *f=fopen("main/ui/codeedit.c","rb");
    if(!f) return NULL;
    static char file[1<<17]; size_t n=fread(file,1,sizeof file-1,f); fclose(f); file[n]=0;
    const char *p=strstr(file,"static const char TEMPLATE[] =");
    if(!p) return NULL;
    // The literal's own lines hold semicolons; the declaration ends at \n";.
    const char *end=strstr(p,"\\n\";");
    if(!end) return NULL;
    end+=3;
    char *out=calloc(1,4096); size_t o=0;
    for(const char *q=p;q<end;q++) {
        if(*q!='"') continue;
        for(q++;*q!='"';q++) {
            if(*q=='\\') { q++; out[o++]=*q=='n'?'\n':*q; }
            else out[o++]=*q;
        }
    }
    return out;
}

int main(void) {
    for(unsigned i=0;i<lesson_count();i++) {
        const lesson_t *l=lesson_at(i);
        unsigned ch=i+1;
        check(!strstr(l->code,"ui.")&&(!l->prelude||!strstr(l->prelude,"ui.")),
              "no legacy ui.* call",ch);
        open_session();
        bool ok=eval(l->prelude,"prelude.js")&&eval(l->code,"user.js");
        switch(l->check) {
        case CHECK_ERROR_THEN_PRINTS:
            check(!ok,"the seeded mistake throws",ch);
            break;
        case CHECK_PRINTS: case CHECK_PRINTS_BOTH:
            check(ok&&strstr(printed,l->want)!=NULL,"runs and prints what the chapter checks",ch);
            break;
        case CHECK_CHANGED: case CHECK_JAPANESE: {
            check(ok&&present()&&frame(0),"runs, presents and survives a frame",ch);
            const char *at=strstr(l->code,l->want);
            check(at!=NULL,"the seeded code holds what the check looks for",ch);
            if(!at) break;
            // Then the edit the chapter asks for, and the picture has to move.
            static uint16_t before[240*135]; memcpy(before,panel,sizeof before);
            close_session(); open_session();
            char edited[1024];
            const char *to=l->check==CHECK_JAPANESE?"'display'":
                           l->want[0]=='\''?"'HI'":"0xff4040ff";
            snprintf(edited,sizeof edited,"%.*s%s%s",(int)(at-l->code),l->code,to,at+strlen(l->want));
            check(eval(l->prelude,"prelude.js")&&eval(edited,"user.js")&&present()&&frame(0),
                  "the chapter's edit runs",ch);
            check(memcmp(before,panel,sizeof before)!=0,"the chapter's edit changes the picture",ch);
            break;
        }
        case CHECK_PRINTS_PREFIX: {
            check(ok&&present()&&frame(0),"runs and presents the prelude's scene",ch);
            check(distinct_colours()>=2,"the prelude draws its text",ch);
            uint16_t before[240*135]; memcpy(before,panel,sizeof before);
            check(frame(0x4000)&&frame(0)&&frame(0x4000)&&frame(0),"two Enter presses",ch);
            check(strstr(printed,"ENTER 1\nENTER 2\n")!=NULL,"prints ENTER per press",ch);
            check(memcmp(before,panel,sizeof before)!=0,"the count on screen changes",ch);
            break;
        }
        }
        close_session();
    }
    // The Playground's first run.
    char *tpl=template_source();
    check(tpl&&!strstr(tpl,"ui."),"template is found and has no legacy ui.* call",0);
    if(tpl) {
        open_session();
        check(eval(tpl,"user.js")&&present()&&frame(0),"template runs and presents",0);
        check(distinct_colours()>=2,"template draws its text",0);
        uint16_t before[240*135]; memcpy(before,panel,sizeof before);
        check(frame(0x4000)&&frame(0)&&memcmp(before,panel,sizeof before)!=0,
              "Enter changes the template's text",0);
        close_session(); free(tpl);
    }
    printf("%s: %u failure(s)\n",failures?"LESSONS FAIL":"LESSONS PASS",failures);
    return failures?1:0;
}
