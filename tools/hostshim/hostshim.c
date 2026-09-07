// The board, the fonts, the flash and the IME, replaced by things a host can
// run. Everything here exists so that main/ui/codeedit.c -- the real file, not
// a copy of its logic -- can be compiled and its pixels compared.
//
// The one that matters is board_present(): on the device it is a one-way SPI
// transfer with MISO unwired, so nothing can read back what the panel holds.
// Here it writes into an array, which is what makes "the incremental repaint
// and the full one agree" a checkable statement rather than a hope.
#include "board.h"
#include "jpfont.h"
#include "srcstore.h"
#include "sound.h"
#include "jsconsole.h"
#include "skk_session.h"
#include "utf8.h"
#include <string.h>
#include <stdlib.h>

// ---- panel ----------------------------------------------------------------

uint16_t host_panel[LCD_W*LCD_H];
uint16_t *host_target = host_panel;
unsigned  host_strips_sent, host_rows_sent;

static uint16_t shared[LCD_W*STRIP_H];
uint16_t *board_strip(void) { return shared; }

esp_err_t board_present(int y, int rows, uint16_t *pixels) {
    if(y<0 || rows<1 || rows>STRIP_H || y+rows>LCD_H) return ESP_ERR_INVALID_ARG;
    for(int r=0;r<rows;r++)
        memcpy(host_target+(size_t)(y+r)*LCD_W, pixels+(size_t)r*LCD_W,
               LCD_W*sizeof(uint16_t));
    host_strips_sent++; host_rows_sent+=(unsigned)rows;
    return ESP_OK;
}

uint16_t board_rgb(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r&0xf8)<<8)|((g&0xfc)<<3)|(b>>3));
}
void board_capture(bool enabled) { (void)enabled; }

// ---- fonts ----------------------------------------------------------------
//
// Not misaki and not shinonome -- a made-up face with the one property that
// matters to the editor's arithmetic: latin is 6 px wide and everything above
// U+007F is 12, so a column is not a byte and not a character either. The
// glyphs are a deterministic pattern of the codepoint, which is enough to make
// "the same text drew differently" visible in a memcmp.
static bool ready[JPFONT_COUNT]={true,true};
void host_font_ready(jpfont_id_t id, bool on) { ready[id]=on; }

static unsigned cell_w(jpfont_id_t id, uint32_t cp) {
    return cp<0x80 ? (id==JPFONT_TEXT?6u:6u) : (id==JPFONT_TEXT?12u:8u);
}
static unsigned cell_h(jpfont_id_t id) { return id==JPFONT_TEXT?12u:8u; }
static bool ink(uint32_t cp, unsigned gx, unsigned gy) {
    return ((cp*31u + gx*7u + gy*13u) % 5u) == 0u;
}

bool jpfont_init(void) { return true; }
bool jpfont_ready(jpfont_id_t id) { return ready[id]; }
unsigned jpfont_cell_w(jpfont_id_t id) { return cell_w(id,0x3042); }
unsigned jpfont_cell_h(jpfont_id_t id) { return cell_h(id); }
unsigned jpfont_baseline(jpfont_id_t id) { return cell_h(id)-2; }
bool jpfont_has(jpfont_id_t id, uint32_t cp) { (void)id; (void)cp; return true; }

unsigned jpfont_glyph(jpfont_id_t id, uint32_t cp, uint8_t *out) {
    if(!ready[id]) return 0;
    unsigned w=cell_w(id,cp), h=cell_h(id);
    for(unsigned y=0;y<h;y++)
        for(unsigned x=0;x<w;x++) out[y*w+x]=ink(cp,x,y)?255:0;
    return w;
}

unsigned jpfont_advance(jpfont_id_t id, const char *s, size_t len, size_t i,
                        size_t *adv) {
    uint32_t cp=utf8_decode(s,len,i,adv);
    return ready[id]?cell_w(id,cp):0;
}

unsigned jpfont_width(jpfont_id_t id, const char *s, size_t len) {
    unsigned w=0;
    for(size_t i=0;i<len;) { size_t adv; w+=jpfont_advance(id,s,len,i,&adv); i+=adv; }
    return w;
}

int jpfont_draw_clip(jpfont_id_t id, uint16_t *pixels, int strip_y, int rows,
                     int x, int y, const char *s, size_t len, uint16_t colour,
                     int x0, int x1) {
    if(!ready[id]) return x;
    if(x0<0) x0=0;
    if(x1>LCD_W) x1=LCD_W;
    for(size_t i=0;i<len;) {
        size_t adv;
        uint32_t cp=utf8_decode(s,len,i,&adv);
        i+=adv;
        unsigned w=cell_w(id,cp), h=cell_h(id);
        if(x+(int)w<=x0) { x+=(int)w; continue; }
        for(unsigned gy=0;gy<h;gy++) {
            int py=y+(int)gy-strip_y;
            if(py<0||py>=rows) continue;
            for(unsigned gx=0;gx<w;gx++) {
                int px=x+(int)gx;
                if(px<x0||px>=x1) continue;
                if(ink(cp,gx,gy)) pixels[(size_t)py*LCD_W+px]=colour;
            }
        }
        x+=(int)w;
        if(x>=x1) break;
    }
    return x;
}

int jpfont_draw(jpfont_id_t id, uint16_t *pixels, int strip_y, int rows,
                int x, int y, const char *s, size_t len, uint16_t colour) {
    return jpfont_draw_clip(id,pixels,strip_y,rows,x,y,s,len,colour,0,LCD_W);
}

// ---- flash ----------------------------------------------------------------

static char   slotbuf[SRC_SLOT_COUNT][SRC_MAX+1];
static size_t slotlen[SRC_SLOT_COUNT];
static bool   slotset[SRC_SLOT_COUNT];

size_t srcstore_load(unsigned slot, char *out) {
    if(slot>=SRC_SLOT_COUNT || !slotset[slot]) { out[0]=0; return 0; }
    memcpy(out,slotbuf[slot],slotlen[slot]);
    out[slotlen[slot]]=0;
    return slotlen[slot];
}
size_t srcstore_load_checked(unsigned slot, char *out, bool *verified) {
    if(verified) *verified = slot<SRC_SLOT_COUNT && slotset[slot];
    return srcstore_load(slot,out);
}
bool srcstore_save(unsigned slot, const char *text, size_t len) {
    if(slot>=SRC_SLOT_COUNT || len>SRC_MAX) return false;
    memcpy(slotbuf[slot],text,len); slotlen[slot]=len; slotset[slot]=true;
    return true;
}
bool srcstore_clear(unsigned slot) {
    if(slot>=SRC_SLOT_COUNT) return false;
    slotset[slot]=false; slotlen[slot]=0; return true;
}
uint32_t srcstore_revision(unsigned slot) { return slotset[slot]?1u:0u; }

// ---- sound ----------------------------------------------------------------

bool sound_play(int kind) { (void)kind; return true; }

// ---- console --------------------------------------------------------------

static char cons[JSC_LINES][JSC_COLS+1];
static unsigned cons_n;
static char cons_err[96];
static bool cons_has_err;

void jsconsole_clear(void) { cons_n=0; }
unsigned jsconsole_count(void) { return cons_n; }
const char *jsconsole_line(unsigned i) { return cons[i%JSC_LINES]; }
const char *jsconsole_error(void) { return cons_has_err?cons_err:NULL; }
void jsconsole_set_error(const char *t) {
    cons_has_err = t!=NULL;
    if(t) { strncpy(cons_err,t,sizeof cons_err-1); cons_err[sizeof cons_err-1]=0; }
}
void host_console_push(const char *line) {
    unsigned at = cons_n<JSC_LINES ? cons_n++ : (JSC_LINES-1);
    if(at==JSC_LINES-1 && cons_n==JSC_LINES)
        for(unsigned i=0;i+1<JSC_LINES;i++) memcpy(cons[i],cons[i+1],JSC_COLS+1);
    strncpy(cons[at],line,JSC_COLS); cons[at][JSC_COLS]=0;
}

// ---- IME ------------------------------------------------------------------

static ime_t session;
static bool  session_present;

bool skk_session_ready(void) { return session_present; }
ime_t *skk_session(void) { return &session; }

bool ime_on(const ime_t *im) { return im->on; }
int  ime_mode(const ime_t *im) { return im->mode; }
void ime_set_on(ime_t *im, bool on) { im->on=on; }
void ime_reset(ime_t *im) { im->preedit_len=0; im->ncand=0; im->sel=-1; }
ime_disp_t ime_feed(ime_t *im, const char *key, size_t len) {
    (void)key; (void)len;
    return im->on ? im->next : IME_NONE;
}
const char *ime_text(const ime_t *im, size_t *len) { *len=im->commit_len; return im->commit; }
const char *ime_preedit(const ime_t *im, size_t *len) { *len=im->preedit_len; return im->preedit; }
int  ime_cand_count(const ime_t *im) { return im->ncand; }
int  ime_sel(const ime_t *im) { return im->sel; }
const char *ime_cand(const ime_t *im, int i, size_t *len) {
    if(i<0||i>=im->ncand) return NULL;
    *len=im->cand_len[i]; return im->cand[i];
}

void host_ime_present(bool present) {
    session_present=present;
    if(present) { session.sel=-1; session.mode=SKK_MODE_HIRA; session.next=IME_NONE; }
}
void host_ime_preedit(const char *utf8) {
    session.preedit_len = utf8?strlen(utf8):0;
    if(utf8) memcpy(session.preedit,utf8,session.preedit_len);
}
void host_ime_candidates(const char *const *list, int n, int sel) {
    if(n>8) n=8;
    for(int i=0;i<n;i++) { session.cand[i]=list[i]; session.cand_len[i]=strlen(list[i]); }
    session.ncand=n; session.sel=sel;
}
