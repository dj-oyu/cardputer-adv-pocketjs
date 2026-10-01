#include "keymap.h"
#include "keystate.h"
#include "esp_log.h"
#include <string.h>

// The Cardputer ADV matrix, as the official demo remaps it: 4 rows x 14
// columns. Row 2 col 0/1 and row 3 col 0/1/2 are the modifiers; everything
// else produces a character. '\0' means the cell is a modifier or unused.
#define COLS 14
static const char PLAIN[4][COLS] = {
    {'`','1','2','3','4','5','6','7','8','9','0','-','=',  8},
    {  9,'q','w','e','r','t','y','u','i','o','p','[',']','\\'},
    {  0,  0,'a','s','d','f','g','h','j','k','l',';','\'', 10},
    {  0,  0,  0,'z','x','c','v','b','n','m',',','.','/',' '},
};
static const char SHIFTED[4][COLS] = {
    {'~','!','@','#','$','%','^','&','*','(',')','_','+',  8},
    {  9,'Q','W','E','R','T','Y','U','I','O','P','{','}','|'},
    {  0,  0,'A','S','D','F','G','H','J','K','L',':','"',  10},
    {  0,  0,  0,'Z','X','C','V','B','N','M','<','>','?',' '},
};

// Modifier cells.
#define IS_FN(r,c)    ((r)==2 && (c)==0)
#define IS_SHIFT(r,c) ((r)==2 && (c)==1)
#define IS_CTRL(r,c)  ((r)==3 && (c)==0)
#define IS_OPT(r,c)   ((r)==3 && (c)==1)
#define IS_ALT(r,c)   ((r)==3 && (c)==2)

static bool held_fn, held_shift, held_ctrl, held_opt, held_alt;
bool keymap_shift(void) { return held_shift; }
bool keymap_fn(void) { return held_fn; }

static void token(keystroke_t *k, const char *name) {
    k->text[0]='\0';
    size_t n=strlen(name);
    memcpy(k->text+1,name,n);
    k->len=(uint8_t)(n+1);
}
static void byte(keystroke_t *k, char c) { k->text[0]=c; k->len=1; }

#ifdef POCKET_KEYTEST
static bool inject_take(board_keyevent_t *e);
#endif

bool keymap_poll(keystroke_t *out) {
    board_keyevent_t e;
    if(!board_key_event(&e)
#ifdef POCKET_KEYTEST
       && !inject_take(&e)
#endif
      ) return false;
    int r=e.row, c=e.col;
    if(r<0||r>=4||c<0||c>=COLS) return false;

    // Every edge goes to keystate before anything below can return: releases
    // and modifier presses are exactly what the translation drops, and they are
    // what a game reads (keystate.h). This is the one place the FIFO is read,
    // so it is the one place that can see all of them. The overflow first: the
    // events lost were NEWER than this one, and releasing everything before
    // applying it leaves this and the nine still queued behind it in force.
    if(e.overflow) {
        keystate_overflow();
        ESP_LOGW("keymap","KEY_OVERFLOW keypad FIFO dropped events; held keys released");
    }
    keystate_apply(r,c,e.pressed);

    if(IS_FN(r,c))    { held_fn=e.pressed;    return false; }
    if(IS_SHIFT(r,c)) { held_shift=e.pressed; return false; }
    if(IS_CTRL(r,c))  { held_ctrl=e.pressed;  return false; }
    if(IS_OPT(r,c))   { held_opt=e.pressed;   return false; }
    if(IS_ALT(r,c))   { held_alt=e.pressed;   return false; }
    if(!e.pressed) return false;

    memset(out,0,sizeof(*out));

    // Ctrl+Alt+Del: taken before anything else so it works while the IME or a
    // JS app owns the keyboard. Del is row 0, col 13.
    if(held_ctrl&&held_alt&&r==0&&c==13) { out->force_stop=true; return true; }

    // The IME toggle. Ctrl+J is SKK's own binding and stays distinguishable
    // from Enter here because the folding to 0x0A happens below, not in the
    // driver. opt+Space is the second surface for it.
    if((held_ctrl&&r==2&&c==8) || (held_opt&&r==3&&c==13)) {
        out->toggle_ime=true; return true;
    }

    if(held_fn) {
        if(r==0&&c==0)  { token(out,"esc");  out->nav=KEY_BACK;  return true; }
        if(r==0&&c==13) { token(out,"del");                      return true; }
        if(r==2&&c==11) { token(out,"up");    out->nav=KEY_UP;    return true; }
        if(r==3&&c==10) { token(out,"left");  out->nav=KEY_LEFT;  return true; }
        if(r==3&&c==11) { token(out,"down");  out->nav=KEY_DOWN;  return true; }
        if(r==3&&c==12) { token(out,"right"); out->nav=KEY_RIGHT; return true; }
        return false;
    }

    char ch=(held_shift?SHIFTED:PLAIN)[r][c];
    if(!ch) return false;

    // Ctrl folds a letter to its control byte. C-g (0x07) cancels a
    // conversion; skk_core reads it straight from the key path.
    if(held_ctrl && ch>='a' && ch<='z') { byte(out,(char)(ch-'a'+1)); return true; }
    if(held_ctrl && ch>='A' && ch<='Z') { byte(out,(char)(ch-'A'+1)); return true; }

    if(ch==10) { byte(out,'\n'); out->nav=KEY_ENTER; return true; }
    if(ch==9)  { byte(out,'\t'); return true; }
    if(ch==8)  { byte(out,'\b'); return true; }
    byte(out,ch);
    // The home screen predates text input and steers with the bare keys that
    // carry the arrows in their Fn legend, so `nav` keeps naming them. A text
    // consumer reads `text` and ignores this.
    if(!held_shift) {
        if(r==0&&c==0)  out->nav=KEY_BACK;
        if(r==2&&c==11) out->nav=KEY_UP;
        if(r==3&&c==10) out->nav=KEY_LEFT;
        if(r==3&&c==11) out->nav=KEY_DOWN;
        if(r==3&&c==12) out->nav=KEY_RIGHT;
    }
    return true;
}

// ------------------------------------------------------------- key names
//
// pocket.input.keys names a key by what its cap says, derived from PLAIN so
// the matrix is described once: a printing character is its own name, the
// four control bytes and the modifier cells get a word. Letters are the lower
// case; Shift is a key of its own and does not rename the others.

static const struct { const char *name; uint8_t row, col; } WORDS[] = {
    {"del",   0,13}, {"tab",  1,0},  {"enter", 2,13}, {"space", 3,13},
    {"fn",    2,0},  {"shift",2,1},  {"ctrl",  3,0},  {"opt",   3,1},
    {"alt",   3,2},
    // Aliases, never produced by keymap_key_name(). The arrows are the cells
    // keymap_poll() names up/left/down/right under Fn (and steers the menu
    // with bare); esc and back are the Fn and bare readings of '`'.
    {"up",    2,11}, {"left", 3,10}, {"down",  3,11}, {"right", 3,12},
    {"esc",   0,0},  {"back", 0,0},
};
#define WORD_NAMES 9      // the first nine are canonical

bool keymap_key_name(int index, char buf[8]) {
    if(index<0 || index>=4*COLS) return false;
    int r=index/COLS, c=index%COLS;
    char ch=PLAIN[r][c];
    if(ch>' ' && ch<0x7f) { buf[0]=ch; buf[1]=0; return true; }
    for(int i=0;i<WORD_NAMES;i++)
        if(WORDS[i].row==r && WORDS[i].col==c) {
            strcpy(buf,WORDS[i].name);
            return true;
        }
    return false;
}

int keymap_key_index(const char *name) {
    char low[8];
    size_t n=0;
    for(;name[n];n++) {
        if(n==sizeof(low)-1) return -1;          // longer than any name
        char ch=name[n];
        low[n]=(ch>='A' && ch<='Z')?(char)(ch-'A'+'a'):ch;
    }
    low[n]=0;
    if(n==1) {
        for(int r=0;r<4;r++) for(int c=0;c<COLS;c++)
            if(PLAIN[r][c]==low[0] && low[0]>' ') return r*COLS+c;
        return -1;
    }
    for(size_t i=0;i<sizeof(WORDS)/sizeof(WORDS[0]);i++)
        if(!strcmp(WORDS[i].name,low)) return WORDS[i].row*COLS+WORDS[i].col;
    return -1;
}

#ifdef POCKET_KEYTEST
// ------------------------------------------------- USB key injection (diag)
//
// A host script presses and releases keys as the keyboard would: the event is
// queued here and keymap_poll() takes it when the FIFO is empty, so it reaches
// keystate AND the translation above -- an injected Ctrl+Alt+Del stops the
// app. Diagnostic builds only (POCKET_KEYTEST in main/CMakeLists.txt): the
// shipping USB byte table stays what tools/*.py already rely on.
//
// Frame: US(0x1f) 'K' op hex hex LF, op '+' press or '-' release, hex the key
// index row*14+col. The input task both parses and drains, so the queue needs
// no lock. A bad frame is swallowed, never typed.
static board_keyevent_t injected[16];
static unsigned inject_head, inject_count;

static bool inject_take(board_keyevent_t *e) {
    if(!inject_count) return false;
    *e=injected[inject_head];
    inject_head=(inject_head+1)%16u;
    inject_count--;
    return true;
}

bool keymap_inject_usb(uint8_t c) {
    static unsigned state;              // 0 idle, 1 'K', 2 op, 3/4 hex, 5 LF, 9 junk
    static bool press;
    static unsigned index;
    if(c==0x1f) { state=1; return true; }
    if(!state) return false;
    int v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
    switch(state) {
    case 1: state=c=='K'?2:9; return true;
    case 2: press=c=='+'; state=(c=='+'||c=='-')?3:9; return true;
    case 3: if(v<0){state=9;return true;} index=(unsigned)v<<4; state=4; return true;
    case 4: if(v<0){state=9;return true;} index|=(unsigned)v; state=5; return true;
    case 5:
        if(c=='\n' && index<4*COLS && inject_count<16) {
            injected[(inject_head+inject_count)%16u]=(board_keyevent_t){
                .row=(uint8_t)(index/COLS),.col=(uint8_t)(index%COLS),.pressed=press};
            inject_count++;
        }
        state=c=='\n'?0:9; return true;
    default: if(c=='\n') state=0; return true;
    }
}
#endif
