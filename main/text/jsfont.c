#include "jsfont.h"
#include "jpfont.h"
#include "utf8.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

// FONT ATLAS v3, the format pocketjs_ui_core_load_font_atlas parses. Header,
// then a codepoint-sorted cmap, then one fixed cell of alpha bytes per glyph.
#define ATLAS_MAGIC   0x41464344u   /* 'DCFA' */
#define ATLAS_VERSION 3
#define ATLAS_HEADER  16
#define ATLAS_CMAP    8

static pocketjs_ui_core_t *ui_core;
// Codepoints on show, ascending. gid 0 is the tofu box and is not in here.
static uint16_t seen[JSFONT_MAX];
static unsigned seen_n;

unsigned jsfont_count(void) { return seen_n; }

void jsfont_detach(void) { ui_core=NULL; }

// Rebuilds the whole slot. The core replaces its copy, so the old one is only
// freed after the new one is parsed; the transient buffer here is the third
// copy alive at that moment, which is why JSFONT_MAX stays small.
static bool reload(void) {
    if(!ui_core || !jpfont_ready(JPFONT_TEXT)) return false;
    unsigned cw=jpfont_cell_w(JPFONT_TEXT), ch=jpfont_cell_h(JPFONT_TEXT);
    unsigned cell=cw*ch;
    unsigned count=seen_n+1;                       // + the tofu at gid 0
    size_t size=ATLAS_HEADER+(size_t)count*ATLAS_CMAP+(size_t)count*cell;

    uint8_t *blob=heap_caps_malloc(size,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!blob) { ESP_LOGW("jsfont","no room for a %u byte atlas",(unsigned)size); return false; }
    memset(blob,0,size);

    blob[0]=(uint8_t)ATLAS_MAGIC; blob[1]=(uint8_t)(ATLAS_MAGIC>>8);
    blob[2]=(uint8_t)(ATLAS_MAGIC>>16); blob[3]=(uint8_t)(ATLAS_MAGIC>>24);
    blob[4]=ATLAS_VERSION; blob[5]=0;
    blob[6]=(uint8_t)count; blob[7]=(uint8_t)(count>>8);
    blob[8]=(uint8_t)cw; blob[9]=(uint8_t)ch;
    blob[10]=(uint8_t)jpfont_baseline(JPFONT_TEXT);
    blob[11]=(uint8_t)ch;                          // line advance
    blob[12]=JSFONT_SLOT; blob[13]=0; blob[14]=1; blob[15]=0;   // density 1

    uint8_t *cmap=blob+ATLAS_HEADER;
    uint8_t *cover=cmap+(size_t)count*ATLAS_CMAP;

    // gid 0: the tofu, mapped from U+0000 so the cmap stays one entry per
    // glyph and ascending. Nothing ever asks for U+0000.
    unsigned advance=jpfont_glyph(JPFONT_TEXT,0,cover);
    cmap[6]=(uint8_t)advance;
    for(unsigned i=0;i<seen_n;i++) {
        uint8_t *e=cmap+(size_t)(i+1)*ATLAS_CMAP;
        uint32_t cp=seen[i];
        e[0]=(uint8_t)cp; e[1]=(uint8_t)(cp>>8); e[2]=0; e[3]=0;
        e[4]=(uint8_t)(i+1); e[5]=(uint8_t)((i+1)>>8);
        e[6]=(uint8_t)jpfont_glyph(JPFONT_TEXT,cp,cover+(size_t)(i+1)*cell);
        e[7]=0;
    }

    esp_err_t err=pocketjs_ui_core_load_font_atlas(ui_core,blob,size);
    heap_caps_free(blob);
    if(err!=ESP_OK) { ESP_LOGW("jsfont","atlas rejected"); return false; }
    ESP_LOGI("jsfont","slot %d now holds %u glyphs (%u bytes)",
             JSFONT_SLOT,count,(unsigned)size);
    return true;
}

void jsfont_attach(pocketjs_ui_core_t *core) {
    ui_core=core;
    seen_n=0;
    // Start with the tofu alone, so a program that shows no text costs one cell.
    reload();
}

// Inserts in order. Returns true when the set changed.
static bool remember(uint32_t cp) {
    if(cp<0x20 || cp>0xffff) return false;
    unsigned lo=0, hi=seen_n;
    while(lo<hi) {
        unsigned mid=lo+(hi-lo)/2;
        if(seen[mid]<cp) lo=mid+1; else hi=mid;
    }
    if(lo<seen_n && seen[lo]==cp) return false;
    if(seen_n>=JSFONT_MAX) return false;    // full; the rest draw as tofu
    memmove(seen+lo+1,seen+lo,(seen_n-lo)*sizeof(seen[0]));
    seen[lo]=(uint16_t)cp;
    seen_n++;
    return true;
}

static JSValue host_glyphs(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)this_val;
    if(argc<1) return JS_UNDEFINED;
    size_t n=0;
    const char *s=JS_ToCStringLen(ctx,&n,argv[0]);
    if(!s) return JS_EXCEPTION;
    bool grew=false;
    for(size_t i=0;i<n;) {
        size_t adv;
        uint32_t cp=utf8_decode(s,n,i,&adv);
        i+=adv;
        if(remember(cp)) grew=true;
    }
    JS_FreeCString(ctx,s);
    // One reload per string, not per character: the core relays out and
    // repaints everything each time a slot changes.
    if(grew) reload();
    return JS_UNDEFINED;
}

esp_err_t jsfont_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    JSValue global=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,global,"__pjs_glyphs",
                      JS_NewCFunction(ctx,host_glyphs,"__pjs_glyphs",1));
    JS_FreeValue(ctx,global);
    return ESP_OK;
}

// Wrapping the binding rather than asking programs to call anything: the
// characters a program shows are exactly the ones it passes to setText.
const char JSFONT_WRAP[] =
    "(function(){var s=ui.setText;ui.setText=function(i,t){"
    "var v=String(t);__pjs_glyphs(v);return s.call(ui,i,v);};})()";
