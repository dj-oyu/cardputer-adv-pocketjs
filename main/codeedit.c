#include "codeedit.h"
#include "board.h"
#include "jpfont.h"
#include "skk_session.h"
#include "srcstore.h"
#include "sound.h"
#include "skk_core.h"
#include "jsconsole.h"
#include "jslex.h"
#include "esp_log.h"
#include "fonts.h"
#include <string.h>
#include <stdio.h>

// A gap buffer would save the memmove on every insert, but the source is
// capped at 8 KB and one memmove of that is microseconds — far below the 27 ms
// the repaint costs. A flat buffer keeps the cursor arithmetic obvious.
static char   text[SRC_MAX+1];
static size_t len;
static size_t cursor;          // byte offset, always on a UTF-8 boundary
static size_t top_line;        // first visible line
static bool   dirty=true, unsaved;
static char   notice[48];
static code_state_t state;

#define LINE_H   12
#define GUTTER   22
#define VIEW_TOP 16
#define VIEW_ROWS 6
#define CONSOLE_TOP (VIEW_TOP+VIEW_ROWS*LINE_H+3)
// 8 px rows, so four of them fit the 33 px between the code and the footer.
#define CONSOLE_ROWS 4

// Draws something on the first frame, so a run that works looks like it did.
// The property numbers are the host's; see apps/hello/main.js for the full set.
static const char TEMPLATE[] =
    "const t = ui.createNode(1);\n"
    "ui.setProp(t, 24, 1);\n"
    "ui.setProp(t, 28, 16); ui.setProp(t, 25, 40);\n"
    "ui.setProp(t, 1, 208); ui.setProp(t, 2, 18);\n"
    "ui.setProp(t, 96, 0xf0f8ffff); ui.setProp(t, 97, 1);\n"
    "ui.setText(t, 'Hello!');\n"
    "ui.insertBefore(1, t, 0);\n"
    "let n = 0;\n"
    "globalThis.frame = (b) => {\n"
    "  if (b & 0x4000) ui.setText(t, 'ENTER ' + (++n));\n"
    "};\n";

code_state_t code_state(void) { return state; }
bool code_dirty(void) { return dirty; }
const char *code_source(size_t *out) { *out=len; return text; }

// Which record this session reads and writes. Everything that saves goes
// through it, so a lesson can never reach the person's own program.
static unsigned slot=SRC_SLOT_USER;

static void open_slot(unsigned which, const char *seed, size_t seed_len) {
    slot=which;
    len=srcstore_load(slot,text);
    if(!len && seed_len) {
        if(seed_len>SRC_MAX) seed_len=SRC_MAX;
        memcpy(text,seed,seed_len);
        len=seed_len;
    }
    text[len]=0;
    cursor=len; top_line=0; unsaved=false; state=CODE_EDIT; dirty=true;
    notice[0]=0;
    if(skk_session_ready()) { ime_reset(skk_session()); ime_set_on(skk_session(),false); }
}

void code_open(void) {
    open_slot(SRC_SLOT_USER,TEMPLATE,sizeof(TEMPLATE)-1);
}

void code_open_lesson(unsigned lesson, const char *seed, size_t seed_len) {
    open_slot(SRC_SLOT_LESSON+lesson,seed,seed_len);
}

void code_returned(const char *error) {
    state=CODE_EDIT; dirty=true;
    snprintf(notice,sizeof(notice),"%s",error?error:"RETURNED");
}

// ---- text mechanics -------------------------------------------------------

static size_t prev_boundary(size_t i) {
    if(!i) return 0;
    i--;
    while(i && ((unsigned char)text[i]&0xc0)==0x80) i--;
    return i;
}
static size_t next_boundary(size_t i) {
    if(i>=len) return len;
    i++;
    while(i<len && ((unsigned char)text[i]&0xc0)==0x80) i++;
    return i;
}
static size_t line_start(size_t i) {
    while(i && text[i-1]!='\n') i--;
    return i;
}
static size_t line_end(size_t i) {
    while(i<len && text[i]!='\n') i++;
    return i;
}
static size_t cursor_line(void) {
    size_t line=0;
    for(size_t i=0;i<cursor;i++) if(text[i]=='\n') line++;
    return line;
}

static void insert(const char *s, size_t n) {
    if(len+n>SRC_MAX) { snprintf(notice,sizeof(notice),"SOURCE FULL"); return; }
    memmove(text+cursor+n,text+cursor,len-cursor);
    memcpy(text+cursor,s,n);
    len+=n; cursor+=n; text[len]=0; unsaved=true;
}
static void erase_before(void) {
    if(!cursor) return;
    size_t start=prev_boundary(cursor);
    memmove(text+start,text+cursor,len-cursor);
    len-=cursor-start; cursor=start; text[len]=0; unsaved=true;
}

// Keep the cursor's line inside the window.
static void follow_cursor(void) {
    size_t line=cursor_line();
    if(line<top_line) top_line=line;
    else if(line>=top_line+VIEW_ROWS) top_line=line-VIEW_ROWS+1;
}

static void move_vertical(int delta) {
    size_t start=line_start(cursor), column=cursor-start;
    if(delta<0) {
        if(!start) return;
        size_t up=line_start(start-1);
        cursor=up+column;
        if(cursor>start-1) cursor=start-1;
    } else {
        size_t end=line_end(cursor);
        if(end>=len) return;
        size_t down=end+1, dend=line_end(down);
        cursor=down+column;
        if(cursor>dend) cursor=dend;
    }
    // The column arithmetic can land inside a multi-byte character.
    while(cursor>0 && cursor<len && ((unsigned char)text[cursor]&0xc0)==0x80) cursor--;
}

// ---- keys -----------------------------------------------------------------

bool code_key(const keystroke_t *k) {
    dirty=true;
    if(state==CODE_RUNNING) return true;

    if(k->toggle_ime) {
        if(skk_session_ready()) {
            ime_t *im=skk_session();
            ime_set_on(im,!ime_on(im));
            snprintf(notice,sizeof(notice),"IME %s",ime_on(im)?"ON":"OFF");
        }
        return true;
    }
    if(!k->len) return true;

    // The IME sees every key first, so a token can never be expanded before the
    // engine has had its say. It passes everything back when kana input is off.
    if(skk_session_ready()) {
        ime_t *im=skk_session();
        ime_disp_t d=ime_feed(im,k->text,k->len);
        if(d==IME_TEXT) {
            size_t n=0;
            const char *committed=ime_text(im,&n);
            insert(committed,n); follow_cursor();
            return true;
        }
        if(d==IME_TAKEN) return true;
    }

    if(k->text[0]=='\0') {
        const char *name=k->text+1;
        size_t n=k->len-1;
        if(n==3&&!memcmp(name,"esc",3))   return false;
        if(n==3&&!memcmp(name,"del",3))   { erase_before(); follow_cursor(); return true; }
        if(n==4&&!memcmp(name,"left",4))  { cursor=prev_boundary(cursor); follow_cursor(); return true; }
        if(n==5&&!memcmp(name,"right",5)) { cursor=next_boundary(cursor); follow_cursor(); return true; }
        if(n==2&&!memcmp(name,"up",2))    { move_vertical(-1); follow_cursor(); return true; }
        if(n==4&&!memcmp(name,"down",4))  { move_vertical(1);  follow_cursor(); return true; }
        return true;
    }

    switch(k->text[0]) {
        case '\b': erase_before(); follow_cursor(); return true;
        case '\n': insert("\n",1); follow_cursor(); return true;
        case '\t': insert("  ",2); follow_cursor(); return true;
        case 0x13: // C-s
            snprintf(notice,sizeof(notice),
                     srcstore_save(slot,text,len)?"SAVED %u B":"SAVE FAILED",(unsigned)len);
            unsaved=false; sound_play(1);
            return true;
        case 0x0e: // C-n: empty document. The template is what a first-ever
                   // open starts from, not what "new" means afterwards.
            text[0]=0; len=0; cursor=0; top_line=0; unsaved=true;
            jsconsole_set_error(NULL);
            snprintf(notice,sizeof(notice),"NEW");
            return true;
        case 0x12: // C-r. Saving only what changed keeps a run from erasing
                   // three flash sectors, and stalling the UI while it does.
            if(unsaved) { srcstore_save(slot,text,len); unsaved=false; }
            state=CODE_RUNNING;
            snprintf(notice,sizeof(notice),"RUNNING");
            return true;
    }
    if((unsigned char)k->text[0]>=0x20) { insert(k->text,k->len); follow_cursor(); }
    return true;
}

// ---- drawing --------------------------------------------------------------

static uint16_t *strip;
static int strip_y, strip_h;

// Colouring for the visible window only. Runs are contiguous within a line, so
// storing a length and a kind is enough — the offset of each run is the sum of
// the ones before it, and nothing has to hold a byte-per-character map.
// 6 rows x 24 runs x 2 bytes plus bookkeeping: under 400 bytes of BSS.
#define SPANS_PER_ROW 24
#define ROW_BYTES_MAX 128       // far past the 218 px the text column can show
static uint8_t span_len[VIEW_ROWS][SPANS_PER_ROW];
static uint8_t span_kind[VIEW_ROWS][SPANS_PER_ROW];
static uint8_t span_n[VIEW_ROWS];
static uint16_t row_off[VIEW_ROWS];   // SRC_MAX is 8192, so 16 bits reach it

static void span_reset(void) { memset(span_n,0,sizeof(span_n)); }

// jslex hands back one run at a time; long runs arrive split at 255 bytes so a
// uint8 length is always enough.
static void span_collect(void *user_data, unsigned line,
                         size_t off, size_t length, uint8_t kind) {
    unsigned base=*(unsigned*)user_data;
    if(line<base) return;
    unsigned row=line-base;
    if(row>=VIEW_ROWS || off>0xffff) return;
    if(!span_n[row]) row_off[row]=off;
    size_t seen=0;
    for(unsigned i=0;i<span_n[row];i++) seen+=span_len[row][i];
    while(length && seen<ROW_BYTES_MAX) {
        size_t take=length>255?255:length;
        if(seen+take>ROW_BYTES_MAX) take=ROW_BYTES_MAX-seen;
        if(span_n[row]<SPANS_PER_ROW) {
            span_len[row][span_n[row]]=(uint8_t)take;
            span_kind[row][span_n[row]]=kind;
            span_n[row]++;
        } else {
            // Out of run slots. Dropping the rest would leave the tail of a
            // busy line — `x=1;y=2;z=3;...` is four runs per four characters —
            // simply unpainted, so it joins the previous run and keeps its
            // colour instead of vanishing.
            unsigned last=SPANS_PER_ROW-1;
            size_t room=255-span_len[row][last];
            if(!room) break;
            if(take>room) take=room;
            span_len[row][last]=(uint8_t)(span_len[row][last]+take);
        }
        seen+=take; length-=take;
    }
}

static void ascii(int x,int y,const char *s,uint16_t colour) {
    if(y>=strip_y+strip_h || y+7<=strip_y) return;
    for(;*s;s++,x+=6) {
        unsigned c=(unsigned char)*s;
        if(c<32||c>126) c='?';
        for(int yy=0;yy<7;yy++) {
            int py=y+yy-strip_y;
            if(py<0||py>=strip_h) continue;
            for(int xx=0;xx<5;xx++)
                if(font_rows[(c-32)*7+yy]&(1<<(4-xx))) {
                    int px=x+xx;
                    if(px>=0&&px<LCD_W) strip[py*LCD_W+px]=colour;
                }
        }
    }
}

// The console strip and the error line, where 33 px has to hold several rows.
// Latin keeps the 5x7 face at 6 px — misaki's own is 3 px wide and unreadable
// at this size — and everything above U+007F comes from misaki's 8x8, so both
// sit inside the same 8 px row.
static void small_text(int x,int y,const char *s,size_t len,uint16_t colour) {
    for(size_t i=0;i<len && x<LCD_W;) {
        unsigned char c=(unsigned char)s[i];
        if(c<0x80) {
            char one[2]={(char)c,0};
            ascii(x,y+1,one,colour);   // 7 rows inside the 8 px row
            x+=6; i++;
            continue;
        }
        size_t run=1;
        while(i+run<len && ((unsigned char)s[i+run]&0xc0)==0x80) run++;
        x=jpfont_draw(JPFONT_SMALL,strip,strip_y,strip_h,x,y,s+i,run,colour);
        i+=run;
    }
}

static void fill(int x,int y,int w,int h,uint16_t colour) {
    for(int r=0;r<h;r++) {
        int py=y+r-strip_y;
        if(py<0||py>=strip_h) continue;
        for(int c=0;c<w;c++) {
            int px=x+c;
            if(px>=0&&px<LCD_W) strip[py*LCD_W+px]=colour;
        }
    }
}

void code_draw(void) {
    dirty=false;
    strip=board_strip();
    ime_t *im = skk_session_ready() ? skk_session() : NULL;
    const uint16_t ink   =board_rgb(220,230,242);
    const uint16_t dim   =board_rgb(92,116,146);
    const uint16_t accent=board_rgb(120,200,255);
    const uint16_t warn  =board_rgb(240,180,110);
    const uint16_t caret =board_rgb(120,200,255);
    const uint16_t rule  =board_rgb(22,38,58);

    // Colour the visible window once. Doing it inside the strip loop would
    // scan the source seventeen times for one repaint.
    size_t here=cursor_line();
    span_reset();
    unsigned base=(unsigned)top_line;
    jslex_scan(text,len,base,base+VIEW_ROWS-1,span_collect,&base);

    const uint16_t palette[JSLEX_KINDS]={
        [JSLEX_PLAIN]  =board_rgb(220,230,242),
        [JSLEX_KEYWORD]=board_rgb(130,190,255),
        [JSLEX_STRING] =board_rgb(150,220,160),
        [JSLEX_COMMENT]=board_rgb(96,116,140),
        [JSLEX_NUMBER] =board_rgb(240,190,130),
    };

    for(strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        strip_h=LCD_H-strip_y<STRIP_H?LCD_H-strip_y:STRIP_H;
        for(int i=0;i<LCD_W*strip_h;i++) strip[i]=board_rgb(6,11,20);

        ascii(4,3,"JS",dim);
        ascii(20,3,unsaved?"*":" ",warn);
        char pos[24];
        snprintf(pos,sizeof(pos),"L%u %uB",(unsigned)here+1,(unsigned)len);
        ascii(30,3,pos,dim);
        if(notice[0]) ascii(120,3,notice,accent);
        fill(0,13,LCD_W,1,rule);

        // Lines, from top_line down. Only the visible window is walked.
        size_t i=0, line=0;
        while(line<top_line && i<len) { if(text[i]=='\n') line++; i++; }
        for(int row=0;row<VIEW_ROWS && i<=len;row++,line++) {
            int y=VIEW_TOP+row*LINE_H;
            size_t end=line_end(i);
            char num[8];
            snprintf(num,sizeof(num),"%3u",(unsigned)line+1);
            ascii(2,y+2,num,line==here?accent:rule);

            int x=GUTTER;
            size_t at=span_n[row]?row_off[row]:i;
            for(unsigned sp=0;sp<span_n[row] && x<LCD_W;sp++) {
                size_t n=span_len[row][sp];
                uint16_t colour=palette[span_kind[row][sp]];
                if(jpfont_ready(JPFONT_TEXT))
                    x=jpfont_draw(JPFONT_TEXT,strip,strip_y,strip_h,x,y,text+at,n,colour);
                else {
                    char flat[32];
                    size_t m=n<sizeof(flat)-1?n:sizeof(flat)-1;
                    memcpy(flat,text+at,m); flat[m]=0;
                    ascii(x,y+2,flat,colour); x+=6*(int)m;
                }
                at+=n;
            }

            if(cursor>=i && cursor<=end && line==here) {
                int cx=GUTTER+(int)(jpfont_ready(JPFONT_TEXT)?jpfont_width(JPFONT_TEXT,text+i,cursor-i)
                                                  :6*(cursor-i));
                // A reading being converted lives in the IME, not in the
                // buffer, so it has to be drawn where it will land. Without
                // this the whole of "Kanji" is invisible until it commits and
                // typing looks like it stopped.
                size_t plen=0;
                const char *pre=im?ime_preedit(im,&plen):NULL;
                if(plen && jpfont_ready(JPFONT_TEXT)) {
                    int pw=(int)jpfont_width(JPFONT_TEXT,pre,plen);
                    fill(cx,y,pw<LCD_W-cx?pw:LCD_W-cx,LINE_H,board_rgb(18,34,54));
                    jpfont_draw(JPFONT_TEXT,strip,strip_y,strip_h,cx,y,pre,plen,accent);
                    fill(cx+pw,y,1,LINE_H,caret);
                } else {
                    fill(cx,y,1,LINE_H,caret);
                }
            }
            if(end>=len) { i=len+1; break; }
            i=end+1;
        }

        // Candidates while converting, otherwise what the last run said: its
        // exception if it threw, else the lines it printed.
        fill(0,CONSOLE_TOP-2,LCD_W,1,rule);
        int ncand=im?ime_cand_count(im):0, sel=im?ime_sel(im):-1;
        const char *failure=jsconsole_error();
        if(ncand>0 && sel>=0 && jpfont_ready(JPFONT_TEXT)) {
            char tag[16];
            snprintf(tag,sizeof(tag),"%u/%u",
                     (unsigned)(sel+1)%1000u,(unsigned)ncand%1000u);
            ascii(4,CONSOLE_TOP+2,tag,dim);
            int cx=44;
            for(int c=sel;c<ncand && cx<LCD_W-16;c++) {
                size_t clen=0;
                const char *cand=ime_cand(im,c,&clen);
                if(!cand) break;
                cx=jpfont_draw(JPFONT_TEXT,strip,strip_y,strip_h,cx,CONSOLE_TOP,cand,clen,
                               c==sel?ink:dim);
                cx+=6;
            }
        }
        else if(failure) small_text(4,CONSOLE_TOP,failure,strlen(failure),warn);
        else {
            unsigned n=jsconsole_count(), rows=n<CONSOLE_ROWS?n:CONSOLE_ROWS;
            for(unsigned r=0;r<rows;r++) {
                const char *line=jsconsole_line(n-rows+r);
                small_text(4,CONSOLE_TOP+(int)r*8,line,strlen(line),dim);
            }
        }

        fill(0,LCD_H-11,LCD_W,1,rule);
        const char *mode="EN";
        if(skk_session_ready() && ime_on(skk_session()))
            mode = ime_mode(skk_session())==SKK_MODE_KATA ? "KANA/KATA" : "KANA";
        ascii(4,LCD_H-8,"C-R RUN C-S SAVE C-N NEW",dim);
        ascii(180,LCD_H-8,mode,accent);
        ESP_ERROR_CHECK(board_present(strip_y,strip_h,strip));
    }
}
