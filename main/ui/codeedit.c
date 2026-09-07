#include "codeedit.h"
#include "vimcmd.h"
#include "utf8.h"
#include "board.h"
#include "paint.h"
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
#include <stdlib.h>

// A gap buffer would save the memmove on every insert, but the source is
// capped at 8 KB and one memmove of that is microseconds — far below the 27 ms
// the repaint costs. A flat buffer keeps the cursor arithmetic obvious.
//
// On the heap rather than in .bss, and not because 8 KB is much to reserve but
// because of *when* it is reserved: as a static it was resident from boot to
// power-off, including the whole of a run, which is the one moment the guest,
// the font atlas and the Wi-Fi driver are all competing for a contiguous
// block. It is taken when this screen opens, given back when it closes, and
// given back again for the length of every run — the source is in flash by
// then, so it comes back from there rather than from a copy.
static char   text_stub[1];
static char  *text = text_stub;   // never NULL: everything below walks it
static bool text_live(void) { return text!=text_stub; }

// Whether flash holds what the buffer holds. `doc.changed` cannot answer this
// on its own: a slot opened on its seed has never been written, is not
// modified, and would come back empty from a reload.
static bool stored;

// The buffer, the cursor and the modified flag live in the document the command
// engine works on, so there is one copy of each rather than two that drift.
static vim_doc_t   doc;
static vim_state_t vim;

// The kana state the person last asked for. Normal mode must not run the
// engine — the next key is a command, not a reading — so a toggle pressed
// there only records the wish, and each insert restores it.
static bool ime_wanted;

static size_t top_line;        // first visible line
static bool   dirty=true;
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
const char *code_source(size_t *out) { *out=doc.len; return text; }

// Takes the buffer if it is not held. A failure is not fatal: `cap` of zero
// makes the engine refuse every insert and the stub keeps every reader in
// bounds, so the screen still draws and still says why it is empty.
static void take_buffer(void) {
    if(!text_live()) {
        char *b=malloc(SRC_MAX+1);
        if(b) b[0]=0;
        else ESP_LOGE("code","NO MEMORY %u B; the slot is still in flash",
                      (unsigned)SRC_MAX+1);
        if(b) text=b;
    }
    doc.text=text; doc.cap=text_live()?SRC_MAX:0;
}
static void give_buffer(void) {
    if(text_live()) free(text);
    text=text_stub; text[0]=0;
    doc.text=text; doc.cap=0; doc.len=0;
}

// Which record this session reads and writes. Everything that saves goes
// through it, so a lesson can never reach the person's own program.
static unsigned slot=SRC_SLOT_USER;

// What the header calls this session: "JS" for the person's own program, the
// chapter number while a lesson is being worked on, so the two screens of the
// tutorial are never confused for each other.
static char label[4]="JS";

static void open_slot(unsigned which, const char *seed, size_t seed_len) {
    slot=which;
    take_buffer();
    doc.len=text_live()?srcstore_load(slot,text):0;
    stored=true;                        // what is in the buffer came from flash
    if(!doc.len && seed_len && text_live()) {
        if(seed_len>SRC_MAX) seed_len=SRC_MAX;
        memcpy(text,seed,seed_len);
        doc.len=seed_len;
        stored=false;                   // the seed has never been written
    }
    text[doc.len]=0;
    // Vim opens at the top of the file, and the first key is a command rather
    // than a character, so nothing is typed by accident on arrival.
    doc.cursor=0; doc.changed=false;
    vim_reset(&vim);
    vim_clamp(&vim,&doc);
    top_line=0; state=CODE_EDIT; dirty=true;
    // The footer spends its room teaching how to type, so the two keys that
    // make the Playground worth opening are taught on arrival instead. The
    // first command message replaces this, which is right: it is onboarding,
    // not chrome.
    snprintf(notice,sizeof(notice),"C-R RUN  C-S SAVE");
    if(!text_live()) snprintf(notice,sizeof(notice),"NO MEMORY FOR SOURCE");
    ime_wanted=false;
    if(skk_session_ready()) { ime_reset(skk_session()); ime_set_on(skk_session(),false); }
}

void code_open(void) {
    snprintf(label,sizeof(label),"JS");
    open_slot(SRC_SLOT_USER,TEMPLATE,sizeof(TEMPLATE)-1);
}

void code_open_lesson(unsigned lesson, const char *seed, size_t seed_len) {
    snprintf(label,sizeof(label),"T%u",(lesson+1)%100u);
    open_slot(SRC_SLOT_LESSON+lesson,seed,seed_len);
    // A chapter tells the learner to type the line and press Ctrl+R, and that
    // sentence has to stay true. Normal mode would read those letters as
    // commands, and the first thing a tutorial must not do is teach a second
    // language before the first one. The Playground still opens in normal mode;
    // this is the one screen where arriving means typing.
    vim_begin_insert(&vim);
}

// Leaving the screen. The buffer is 8 KB that nothing reads again until the
// next open, and the next open reads it out of flash.
void code_close(void) { give_buffer(); }

// The guest has finished parsing out of this buffer, so it goes back to the
// heap for the length of the run — which is exactly the stretch in which the
// radio needs about 48 KB free and finds 23. do_run() has written the source to
// flash by now, which is what makes this a release rather than a loss.
void code_run_release(void) { give_buffer(); }

// The run is over and the screen is about to be read again. The document comes
// back byte for byte, so the cursor and the undo ring's offsets still mean what
// they meant.
void code_run_restore(void) {
    take_buffer();
    doc.len=text_live()?srcstore_load(slot,text):0;
    text[doc.len]=0;
    if(doc.cursor>doc.len) doc.cursor=doc.len;
    vim_clamp(&vim,&doc);
    doc.changed=false; stored=true;
    dirty=true;
}

void code_returned(const char *error) {
    state=CODE_EDIT; dirty=true;
    snprintf(notice,sizeof(notice),"%s",error?error:"RETURNED");
}

// ---- text mechanics -------------------------------------------------------
//
// The engine has its own copies of these for the commands; these are the
// drawing side's, which only ever reads.

static size_t line_start(size_t i) {
    while(i && text[i-1]!='\n') i--;
    return i;
}
static size_t line_end(size_t i) {
    while(i<doc.len && text[i]!='\n') i++;
    return i;
}
static size_t cursor_line(void) {
    size_t line=0;
    for(size_t i=0;i<doc.cursor;i++) if(text[i]=='\n') line++;
    return line;
}

// Keep the cursor's line inside the window.
static void follow_cursor(void) {
    size_t line=cursor_line();
    if(line<top_line) top_line=line;
    else if(line>=top_line+VIEW_ROWS) top_line=line-VIEW_ROWS+1;
}

// ---- keys -----------------------------------------------------------------

static void do_save(void) {
    snprintf(notice,sizeof(notice),
             srcstore_save(slot,text,doc.len)?"SAVED %u B":"SAVE FAILED",
             (unsigned)doc.len);
    doc.changed=false; stored=true;
    sound_play(1);
}

static void do_run(void) {
    // Saving only what changed keeps a run from erasing three flash sectors,
    // and stalling the UI while it does. `stored` is the second half of that
    // test: the buffer is handed back to the heap for the length of the run and
    // reloaded from flash afterwards, so a seed that was never written would
    // come back as an empty document.
    if(doc.changed || !stored) {
        srcstore_save(slot,text,doc.len);
        doc.changed=false; stored=true;
    }
    state=CODE_RUNNING;
    snprintf(notice,sizeof(notice),"RUNNING");
}

static void do_new(void) {
    // The template is what a first-ever open starts from, not what "new" means
    // afterwards.
    text[0]=0; doc.len=0; doc.cursor=0; doc.changed=true;
    stored=false;
    top_line=0;
    vim_reset(&vim);
    // Emptying the document is followed by typing into it, so it starts typing.
    // A host piping a source in over USB depends on that too: it sends C-n and
    // then the bytes, and in normal mode those bytes would be commands.
    vim_begin_insert(&vim);
    if(skk_session_ready()) ime_set_on(skk_session(),ime_wanted);
    jsconsole_set_error(NULL);
    snprintf(notice,sizeof(notice),"NEW");
}

// One line per command, so a host script can assert on the cursor without a
// screenshot. Insert-mode keystrokes are deliberately silent: a host piping a
// 6 KB source in would otherwise get 6000 log lines through a 1 KB USB buffer,
// and the drawing task would stall behind them.
static void log_state(void) {
    static const char *const NAMES[]={"NORMAL","INSERT","CMD"};
    ESP_LOGI("code","VIM %s L%u C%u B%u",NAMES[vim_mode(&vim)],
             (unsigned)cursor_line()+1,
             (unsigned)(doc.cursor-line_start(doc.cursor)),
             (unsigned)doc.len);
}

bool code_key(const keystroke_t *k) {
    dirty=true;
    if(state==CODE_RUNNING) return true;

    if(k->toggle_ime) {
        ime_wanted=!ime_wanted;
        if(skk_session_ready() && vim_mode(&vim)==VIM_INSERT)
            ime_set_on(skk_session(),ime_wanted);
        snprintf(notice,sizeof(notice),"IME %s",ime_wanted?"ON":"OFF");
        return true;
    }
    if(!k->len) return true;

    // The editor's own three keys, in every mode, because the footer promises
    // them and tools/pocket_bridge.py drives the editor with them. They are
    // taken before the IME so a save is never swallowed by a conversion.
    if(k->text[0]) switch(k->text[0]) {
        case 0x13: do_save(); return true;   // C-s
        case 0x12: do_run();  return true;   // C-r
        case 0x0e: do_new();  return true;   // C-n
    }

    vim_key_t vk={VIM_KEY_TEXT,k->text,k->len};
    if(k->text[0]=='\0') {
        const char *name=k->text+1;
        size_t n=k->len-1;
        if(n==3&&!memcmp(name,"esc",3))        vk.kind=VIM_KEY_ESC;
        else if(n==3&&!memcmp(name,"del",3))   vk.kind=VIM_KEY_BACKSPACE;
        else if(n==4&&!memcmp(name,"left",4))  vk.kind=VIM_KEY_LEFT;
        else if(n==5&&!memcmp(name,"right",5)) vk.kind=VIM_KEY_RIGHT;
        else if(n==2&&!memcmp(name,"up",2))    vk.kind=VIM_KEY_UP;
        else if(n==4&&!memcmp(name,"down",4))  vk.kind=VIM_KEY_DOWN;
        else return true;
        vk.text=NULL; vk.len=0;
    } else if(k->text[0]==0x03) {
        // C-c. This keyboard has no Escape of its own — Fn+` produces the token
        // above and a host sends 0x1b — so leaving a mode has a second surface
        // that needs neither.
        vk.kind=VIM_KEY_ESC; vk.text=NULL; vk.len=0;
    }

    // Only insert mode may reach the IME. In normal mode a key is a command,
    // and handing `d` to a kana engine would make it one half of a reading.
    if(vim_mode(&vim)==VIM_INSERT && vk.kind==VIM_KEY_TEXT && skk_session_ready()) {
        ime_t *im=skk_session();
        ime_disp_t d=ime_feed(im,k->text,k->len);
        if(d==IME_TEXT) {
            size_t n=0;
            const char *committed=ime_text(im,&n);
            vim_key_t t={VIM_KEY_TEXT,committed,n};
            vim_feed(&vim,&doc,t);
            follow_cursor();
            return true;
        }
        if(d==IME_TAKEN) return true;
    }

    bool was_insert = vim_mode(&vim)==VIM_INSERT;
    // Leaving insert drops whatever was being composed rather than committing
    // it: a mode change must not put a half-converted reading in the source.
    if(vk.kind==VIM_KEY_ESC && was_insert && skk_session_ready()) {
        ime_reset(skk_session());
        ime_set_on(skk_session(),false);
    }

    vim_action_t act=vim_feed(&vim,&doc,vk);

    if(!was_insert && vim_mode(&vim)==VIM_INSERT && skk_session_ready())
        ime_set_on(skk_session(),ime_wanted);

    const char *m=vim_message(&vim);
    if(m[0]) snprintf(notice,sizeof(notice),"%s",m);
    follow_cursor();
    if(vim_mode(&vim)!=VIM_INSERT || !was_insert) log_state();

    switch(act) {
        case VIM_ACT_SAVE:       do_save(); break;
        case VIM_ACT_SAVE_QUIT:  do_save(); return false;
        case VIM_ACT_QUIT:
            // `:q` on unsaved work refuses and says which command means it.
            if(doc.changed) { snprintf(notice,sizeof(notice),"NO WRITE (:q! TO DROP)"); break; }
            return false;
        case VIM_ACT_QUIT_FORCE: return false;
        case VIM_ACT_NONE:       break;
    }
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


// The console strip and the error line, where 33 px has to hold several rows.
// Latin keeps the 5x7 face at 6 px — misaki's own is 3 px wide and unreadable
// at this size — and everything above U+007F comes from misaki's 8x8, so both
// sit inside the same 8 px row.
static void small_text(int x,int y,const char *s,size_t len,uint16_t colour) {
    for(size_t i=0;i<len && x<LCD_W;) {
        unsigned char c=(unsigned char)s[i];
        if(c<0x80) {
            char one[2]={(char)c,0};
            paint_ascii(x,y+1,one,colour);   // 7 rows inside the 8 px row
            x+=6; i++;
            continue;
        }
        size_t run=1;
        while(i+run<len && utf8_is_cont(s[i+run])) run++;
        x=jpfont_draw(JPFONT_SMALL,strip,strip_y,strip_h,x,y,s+i,run,colour);
        i+=run;
    }
}

// The bands of the screen. Each is drawn once per strip and clips itself, so
// none of them needs to know which strip is live — paint_begin holds that.
typedef struct {
    uint16_t ink, dim, accent, warn, caret, rule, ground;
    uint16_t span[JSLEX_KINDS];
} palette_t;
static palette_t colour;
static ime_t *ime;                // the session, or NULL when there is no dict
static size_t caret_line;

static void draw_header(void) {
    paint_ascii(4,3,label,colour.dim);
    paint_ascii(20,3,doc.changed?"*":" ",colour.warn);
    char pos[24];
    snprintf(pos,sizeof(pos),"L%u %uB",(unsigned)caret_line+1,(unsigned)doc.len);
    paint_ascii(30,3,pos,colour.dim);
    // The tutorial's verdicts come through here, so this line has to carry
    // Japanese; misaki's 8 px fits the 14 px header.
    if(notice[0]) small_text(104,2,notice,strlen(notice),colour.accent);
    paint_fill(0,13,LCD_W,1,colour.rule);
}

static int glyph_width(const char *s, size_t n) {
    return (int)(jpfont_ready(JPFONT_TEXT) ? jpfont_width(JPFONT_TEXT,s,n) : 6*n);
}

// The caret, and over it whatever the IME is composing. A reading being
// converted lives in the engine, not in the buffer, so it has to be drawn
// where it will land: without this the whole of "Kanji" stays invisible until
// it commits and typing looks like it stopped.
static void draw_caret(int x,int y,size_t line_at) {
    size_t plen=0;
    const char *pre=ime?ime_preedit(ime,&plen):NULL;
    int cx=x+glyph_width(text+line_at,doc.cursor-line_at);
    if(plen && jpfont_ready(JPFONT_TEXT)) {
        int pw=(int)jpfont_width(JPFONT_TEXT,pre,plen);
        paint_fill(cx,y,pw<LCD_W-cx?pw:LCD_W-cx,LINE_H,board_rgb(18,34,54));
        jpfont_draw(JPFONT_TEXT,strip,strip_y,strip_h,cx,y,pre,plen,colour.accent);
        cx+=pw;
    }
    if(vim_mode(&vim)!=VIM_NORMAL || plen) { paint_fill(cx,y,1,LINE_H,colour.caret); return; }
    // Normal mode sits *on* a character, so the caret covers one. The panel has
    // no inverse mode, so the glyph is painted again in the background colour
    // rather than left buried under the block.
    size_t n=0;
    if(doc.cursor<doc.len && text[doc.cursor]!='\n') {
        n=1;
        while(doc.cursor+n<doc.len && utf8_is_cont(text[doc.cursor+n])) n++;
    }
    int w=n?glyph_width(text+doc.cursor,n):5;
    paint_fill(cx,y,w<LCD_W-cx?w:LCD_W-cx,LINE_H,colour.caret);
    if(n && jpfont_ready(JPFONT_TEXT))
        jpfont_draw(JPFONT_TEXT,strip,strip_y,strip_h,cx,y,text+doc.cursor,n,colour.ground);
}

static void draw_lines(void) {
    size_t i=0, line=0;
    while(line<top_line && i<doc.len) { if(text[i]=='\n') line++; i++; }
    for(int row=0;row<VIEW_ROWS && i<=doc.len;row++,line++) {
        int y=VIEW_TOP+row*LINE_H;
        size_t end=line_end(i);
        char num[8];
        snprintf(num,sizeof(num),"%3u",(unsigned)line+1);
        paint_ascii(2,y+2,num,line==caret_line?colour.accent:colour.rule);

        int x=GUTTER;
        size_t at=span_n[row]?row_off[row]:i;
        for(unsigned sp=0;sp<span_n[row] && x<LCD_W;sp++) {
            size_t n=span_len[row][sp];
            uint16_t c=colour.span[span_kind[row][sp]];
            if(jpfont_ready(JPFONT_TEXT))
                x=jpfont_draw(JPFONT_TEXT,strip,strip_y,strip_h,x,y,text+at,n,c);
            else {
                char flat[32];
                size_t m=n<sizeof(flat)-1?n:sizeof(flat)-1;
                memcpy(flat,text+at,m); flat[m]=0;
                paint_ascii(x,y+2,flat,c); x+=6*(int)m;
            }
            at+=n;
        }
        if(doc.cursor>=i && doc.cursor<=end && line==caret_line) draw_caret(GUTTER,y,i);
        if(end>=doc.len) break;
        i=end+1;
    }
}

// Candidates while converting, otherwise what the last run said: its exception
// if it threw, else the lines it printed.
static void draw_console(void) {
    paint_fill(0,CONSOLE_TOP-2,LCD_W,1,colour.rule);
    int ncand=ime?ime_cand_count(ime):0, sel=ime?ime_sel(ime):-1;
    const char *failure=jsconsole_error();
    if(ncand>0 && sel>=0 && jpfont_ready(JPFONT_TEXT)) {
        char tag[16];
        snprintf(tag,sizeof(tag),"%u/%u",
                 (unsigned)(sel+1)%1000u,(unsigned)ncand%1000u);
        paint_ascii(4,CONSOLE_TOP+2,tag,colour.dim);
        int cx=44;
        for(int c=sel;c<ncand && cx<LCD_W-16;c++) {
            size_t clen=0;
            const char *cand=ime_cand(ime,c,&clen);
            if(!cand) break;
            cx=jpfont_draw(JPFONT_TEXT,strip,strip_y,strip_h,cx,CONSOLE_TOP,cand,clen,
                           c==sel?colour.ink:colour.dim);
            cx+=6;
        }
        return;
    }
    if(failure) { small_text(4,CONSOLE_TOP,failure,strlen(failure),colour.warn); return; }
    unsigned n=jsconsole_count(), rows=n<CONSOLE_ROWS?n:CONSOLE_ROWS;
    for(unsigned r=0;r<rows;r++) {
        const char *line=jsconsole_line(n-rows+r);
        small_text(4,CONSOLE_TOP+(int)r*8,line,strlen(line),colour.dim);
    }
}

// The last row. This is the only thing standing between a modal editor and
// someone writing their first JavaScript on this machine, so it does not
// decorate — it says which mode is on and which key gets out of it.
//
// The mode is a filled badge rather than text among text: the two modes differ
// by hue before they differ by word, so "why are my letters disappearing" is
// answerable at a glance, and the hint next to it names a key that exists on
// this keyboard. Escape is Fn+` here and nowhere in the legend, which is
// exactly the thing a person cannot guess.
#define BADGE_W  42
#define HINT_X   48
#define ECHO_X   160
#define KANA_X   210

static void draw_footer(void) {
    paint_fill(0,LCD_H-11,LCD_W,1,colour.rule);
    size_t clen=0;
    const char *cmd=vim_cmdline(&vim,&clen);
    if(cmd) {
        // The ':' or '/' is its own indicator, so the line takes the whole row.
        char line[VIM_CMD_MAX+2];
        line[0]=vim.cmd_kind;
        size_t n=clen<sizeof(line)-2?clen:sizeof(line)-2;
        memcpy(line+1,cmd,n); line[n+1]=0;
        paint_ascii(4,LCD_H-8,line,colour.ink);
        paint_fill(4+6*(int)(n+1),LCD_H-9,1,9,colour.caret);
        return;
    }

    bool insert=vim_mode(&vim)==VIM_INSERT;
    paint_fill(0,LCD_H-10,BADGE_W,10,insert?colour.warn:colour.accent);
    paint_ascii(3,LCD_H-8,insert?"INSERT":"NORMAL",colour.ground);
    paint_ascii(HINT_X,LCD_H-8,
                insert?"Fn+` = DONE TYPING":"PRESS i TO TYPE",
                insert?colour.ink:colour.dim);

    const char *echo=vim_pending(&vim);
    if(echo[0]) paint_ascii(ECHO_X,LCD_H-8,echo,colour.ink);

    // Four characters, because the badge and the hint now own the room the
    // long form used to have. KANA and KATA still read apart.
    const char *kana="EN";
    if(skk_session_ready() && ime_on(skk_session()))
        kana = ime_mode(skk_session())==SKK_MODE_KATA ? "KATA" : "KANA";
    paint_ascii(KANA_X,LCD_H-8,kana,colour.accent);
}

void code_draw(void) {
    dirty=false;
    strip=board_strip();
    ime = skk_session_ready() ? skk_session() : NULL;
    caret_line=cursor_line();
    colour=(palette_t){
        .ink=board_rgb(220,230,242), .dim=board_rgb(92,116,146),
        .accent=board_rgb(120,200,255), .warn=board_rgb(240,180,110),
        .caret=board_rgb(120,200,255), .rule=board_rgb(22,38,58),
        .ground=board_rgb(6,11,20),
        .span={
            [JSLEX_PLAIN]  =board_rgb(220,230,242),
            [JSLEX_KEYWORD]=board_rgb(130,190,255),
            [JSLEX_STRING] =board_rgb(150,220,160),
            [JSLEX_COMMENT]=board_rgb(96,116,140),
            [JSLEX_NUMBER] =board_rgb(240,190,130),
        },
    };

    // Colour the visible window once. Doing it inside the strip loop would
    // scan the source seventeen times for one repaint.
    span_reset();
    unsigned base=(unsigned)top_line;
    jslex_scan(text,doc.len,base,base+VIEW_ROWS-1,span_collect,&base);

    for(strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        strip_h=LCD_H-strip_y<STRIP_H?LCD_H-strip_y:STRIP_H;
        paint_begin(strip,strip_y,strip_h);
        for(int i=0;i<LCD_W*strip_h;i++) strip[i]=colour.ground;
        draw_header();
        draw_lines();
        draw_console();
        draw_footer();
        ESP_ERROR_CHECK(board_present(strip_y,strip_h,strip));
    }
}
