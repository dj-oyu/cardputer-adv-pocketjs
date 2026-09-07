#include "keymap.h"
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

bool keymap_poll(keystroke_t *out) {
    board_keyevent_t e;
    if(!board_key_event(&e)) return false;
    int r=e.row, c=e.col;
    if(r<0||r>=4||c<0||c>=COLS) return false;

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
