#include "jsconsole.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

// A fixed ring of short lines. Only the app task writes here, and it does so
// between JS calls, so nothing locks.
static char     lines[JSC_LINES][JSC_COLS+1];
static unsigned count, next;
static char     pending[JSC_COLS+1];
static size_t   pending_len;
static char     error[128];

void jsconsole_clear(void) {
    count=0; next=0; pending_len=0; pending[0]=0; error[0]=0;
}
unsigned jsconsole_count(void) { return count; }
const char *jsconsole_line(unsigned i) {
    if(i>=count) return "";
    unsigned first = count<JSC_LINES ? 0 : next;
    return lines[(first+i)%JSC_LINES];
}
const char *jsconsole_error(void) { return error[0]?error:NULL; }
void jsconsole_set_error(const char *text) {
    if(!text) { error[0]=0; return; }
    snprintf(error,sizeof(error),"%s",text);
    // Newlines would run off the single row the editor gives this.
    for(char *p=error;*p;p++) if(*p=='\n'||*p=='\r') *p=' ';
}

static void flush_line(void) {
    pending[pending_len]=0;
    snprintf(lines[next],sizeof(lines[0]),"%s",pending);
    next=(next+1)%JSC_LINES;
    if(count<JSC_LINES) count++;
    pending_len=0; pending[0]=0;
}

static void append(const char *s, size_t n) {
    for(size_t i=0;i<n;i++) {
        char c=s[i];
        if(c=='\n') { flush_line(); continue; }
        if(c=='\r' || c=='\t') c=' ';
        if((unsigned char)c<0x20) continue;
        // Wrapping mid-character would hand the renderer half a UTF-8
        // sequence, which draws as tofu. A continuation byte therefore never
        // starts a line: the whole character moves down together.
        if(pending_len>=JSC_COLS) {
            if(((unsigned char)c&0xc0)==0x80) {
                while(pending_len && ((unsigned char)pending[pending_len-1]&0xc0)==0x80)
                    pending_len--;
                if(pending_len) pending_len--;   // the lead byte too
            }
            flush_line();
        }
        pending[pending_len++]=c;
    }
}

// print(...) / console.log(...). Values are stringified with JS_ToCStringLen,
// which can run a toString and therefore re-enter JS; nothing here holds state
// across that beyond the ring, which is fine to append into re-entrantly.
static JSValue host_print(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    (void)this_val;
    for(int i=0;i<argc;i++) {
        size_t n=0;
        const char *s=JS_ToCStringLen(ctx,&n,argv[i]);
        if(!s) return JS_EXCEPTION;
        append(s,n);
        if(i+1<argc) append(" ",1);
        // A script can print kilobytes; one log line that long blocks the USB
        // writer for tens of milliseconds.
        ESP_LOGI("js","%.*s",(int)(n>200?200:n),s);
        JS_FreeCString(ctx,s);
    }
    append("\n",1);
    return JS_UNDEFINED;
}

// __pjs_error(message, stack) — the frame wrapper's way of handing an exception
// to the host before rethrowing it.
static JSValue host_error(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    (void)this_val;
    char buffer[sizeof(error)];
    buffer[0]=0;
    if(argc>0) {
        const char *m=JS_ToCString(ctx,argv[0]);
        if(m) { snprintf(buffer,sizeof(buffer),"%s",m); JS_FreeCString(ctx,m); }
    }
    if(argc>1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        const char *s=JS_ToCString(ctx,argv[1]);
        if(s) {
            // The first stack line carries the file and line number.
            const char *nl=strchr(s,'\n');
            int n=nl?(int)(nl-s):(int)strlen(s);
            size_t used=strlen(buffer);
            snprintf(buffer+used,sizeof(buffer)-used," %.*s",n,s);
            JS_FreeCString(ctx,s);
        }
    }
    jsconsole_set_error(buffer);
    ESP_LOGW("js","%s",buffer);
    return JS_UNDEFINED;
}

esp_err_t jsconsole_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue print=JS_NewCFunction(ctx,host_print,"print",1);
    JSValue console=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,console,"log",JS_DupValue(ctx,print));
    JS_SetPropertyStr(ctx,console,"warn",JS_DupValue(ctx,print));
    JS_SetPropertyStr(ctx,console,"error",JS_DupValue(ctx,print));
    JS_SetPropertyStr(ctx,global,"console",console);
    JS_SetPropertyStr(ctx,global,"print",print);
    JS_SetPropertyStr(ctx,global,"__pjs_error",
                      JS_NewCFunction(ctx,host_error,"__pjs_error",2));
    JS_FreeValue(ctx,global);
    return ESP_OK;
}
