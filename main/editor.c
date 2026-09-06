#include "editor.h"
#include "board.h"
#include "jpfont.h"
#include "skk_session.h"
#include "sound.h"
#include "skk_core.h"
#include "esp_log.h"
#include "fonts.h"
#include <string.h>
#include <stdio.h>

#define BUF_MAX 256

// Readings the user types and what SKK should produce. Short enough that a
// 240 px line holds the target and the attempt side by side.
static const struct { const char *reading; const char *target; } DRILLS[] = {
    {"kanji",        "漢字"},
    {"nihongo",      "日本語"},
    {"AtarasiI",     "新しい"},
    {"TukauU",       "使う"},
    {"denshikousaku","電子工作"},
    {"KangaeRu",     "考える"},
};
#define DRILL_N (sizeof(DRILLS)/sizeof(DRILLS[0]))

static char     buf[BUF_MAX];
static size_t   buf_len;
static unsigned drill;
static bool     cleared;          // the current drill has been matched
static uint16_t *strip;
static int      strip_y, strip_h;
static bool     dirty;

bool editor_dirty(void) { return dirty; }

void editor_open(void) {
    buf_len=0; buf[0]=0; drill=0; cleared=false; dirty=true;
    if(skk_session_ready()) {
        ime_reset(skk_session());
        ime_set_on(skk_session(),true);
    }
    int64_t open_us=0, probe_us=0;
    skk_session_timing(&open_us,&probe_us);
    ESP_LOGI("editor","OPEN skk=%s font=%s open=%lldus probe=%lldus",
             skk_session_status(), jpfont_ready(JPFONT_TEXT)?"READY":"MISSING",open_us,probe_us);
}

static void append(const char *s, size_t len) {
    if(buf_len+len>=BUF_MAX) return;
    memcpy(buf+buf_len,s,len);
    buf_len+=len;
    buf[buf_len]=0;
}

// Delete one UTF-8 character, never a single byte of a multi-byte one.
static void backspace(void) {
    if(!buf_len) return;
    size_t i=buf_len-1;
    while(i>0 && ((unsigned char)buf[i]&0xc0)==0x80) i--;
    buf_len=i;
    buf[buf_len]=0;
}

bool editor_key(const keystroke_t *k) {
    // Any accepted key can move the preedit, the candidates or the buffer, so
    // one flag at the entry covers every path below.
    dirty=true;

    // Ctrl+J and opt+Space carry no text of their own; without this they fell
    // through the `!k->len` guard below and the toggle did nothing.
    if(k->toggle_ime) {
        if(skk_session_ready()) {
            ime_t *im=skk_session();
            ime_set_on(im,!ime_on(im));
            ESP_LOGI("editor","IME %s",ime_on(im)?"ON":"OFF");
        }
        return true;
    }
    if(!k->len) return true;

    // Everything goes to the IME first, so the editor cannot expand a token
    // before the engine has had its say.
    if(skk_session_ready()) {
        ime_t *im=skk_session();
        ime_disp_t d=ime_feed(im,k->text,k->len);
        if(d==IME_TEXT) {
            size_t len=0;
            const char *text=ime_text(im,&len);
            append(text,len);
            sound_play(1);
            ESP_LOGI("editor","COMMIT %.*s (%u bytes)",(int)len,text,(unsigned)len);
            if(!cleared && strcmp(buf,DRILLS[drill].target)==0) {
                cleared=true;
                ESP_LOGI("editor","DRILL %u CLEARED",drill);
            }
            return true;
        }
        if(d==IME_TAKEN) {
            size_t plen=0;
            const char *pre=ime_preedit(im,&plen);
            ESP_LOGI("editor","IME mode=%d cands=%d pre=%.*s",
                     ime_mode(im),ime_cand_count(im),(int)plen,pre);
            return true;
        }
        ESP_LOGI("editor","PASS %02x len=%u",(unsigned char)k->text[0],k->len);
    }

    // Not consumed by the IME: the editor's own keys.
    if(k->text[0]=='\0') {
        const char *name=k->text+1;
        size_t n=k->len-1;
        if(n==3 && !memcmp(name,"esc",3)) return false;
        if(n==3 && !memcmp(name,"del",3)) { backspace(); return true; }
        return true;
    }
    if(k->text[0]=='\b') { backspace(); return true; }
    if(k->text[0]=='\n') {
        // Enter with nothing composing moves on once the drill is matched.
        if(cleared) {
            drill=(drill+1)%DRILL_N;
            buf_len=0; buf[0]=0; cleared=false;
            sound_play(0);
        }
        return true;
    }
    if((unsigned char)k->text[0]>=0x20) append(k->text,k->len);
    return true;
}

// The 5x7 ASCII face the shell uses, for labels the Japanese font need not
// carry and for the fallback when jp_font is missing.
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

static void jp(int x,int y,const char *s,size_t len,uint16_t colour) {
    jpfont_draw(JPFONT_TEXT,strip,strip_y,strip_h,x,y,s,len,colour);
}

// SKK's own indicators rather than latin abbreviations, now that the font can
// draw them: あ / ア / A while typing, ▽ / ▼ while converting.
static const char *mode_mark(void) {
    if(!skk_session_ready()) return "--";
    if(!ime_on(skk_session())) return "A";
    switch(ime_mode(skk_session())) {
        case SKK_MODE_ASCII:   return "A";
        case SKK_MODE_KATA:    return "ア";
        case SKK_MODE_MIDASHI: return "▽";
        case SKK_MODE_OKURI:   return "▽*";
        case SKK_MODE_SELECT:  return "▼";
        default:               return "あ";
    }
}

void editor_draw(void) {
    dirty=false;
    strip=board_strip();
    ime_t *im = skk_session_ready() ? skk_session() : NULL;
    const uint16_t ink   =board_rgb(226,235,245);
    const uint16_t dim   =board_rgb(96,120,150);
    const uint16_t accent=board_rgb(120,200,255);
    const uint16_t good  =board_rgb(130,230,150);
    const uint16_t rule  =board_rgb(24,40,62);

    for(strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        strip_h=LCD_H-strip_y<STRIP_H ? LCD_H-strip_y : STRIP_H;
        for(int i=0;i<LCD_W*strip_h;i++) strip[i]=board_rgb(6,12,22);

        // Header: the drill's romaji and the IME mode.
        ascii(4,3,"SKK",dim);
        ascii(28,3,DRILLS[drill].reading,accent);
        const char *mark=mode_mark();
        if(jpfont_ready(JPFONT_TEXT)) {
            int w=(int)jpfont_width(JPFONT_TEXT,mark,strlen(mark));
            jp(LCD_W-4-w,1,mark,strlen(mark),accent);
        } else ascii(LCD_W-16,3,mark,dim);
        for(int x=0;x<LCD_W;x++) {
            int py=13-strip_y;
            if(py>=0&&py<strip_h) strip[py*LCD_W+x]=rule;
        }

        if(!jpfont_ready(JPFONT_TEXT)) {
            ascii(4,40,"NO JAPANESE FONT",ink);
            ascii(4,52,"FLASH jp_font PARTITION",dim);
        } else {
            // Target, then what the user has actually produced.
            ascii(4,20,"TARGET",dim);
            jp(54,17,DRILLS[drill].target,strlen(DRILLS[drill].target),dim);

            ascii(4,40,"YOURS",dim);
            jp(54,37,buf,buf_len,cleared?good:ink);

            if(cleared) ascii(4,57,"OK - ENTER FOR NEXT",good);

            if(im) {
                size_t plen=0;
                const char *pre=ime_preedit(im,&plen);
                if(plen) jp(4,74,pre,plen,accent);

                int n=ime_cand_count(im), sel=ime_sel(im);
                if(n>0 && sel>=0) {
                    // SKK_CAND_MAX is 64, so two digits always suffice; the
                    // clamp is what lets the compiler see that.
                    char tag[16];
                    snprintf(tag,sizeof(tag),"%u/%u",
                             (unsigned)(sel+1)%1000u,(unsigned)n%1000u);
                    ascii(4,95,tag,dim);
                    int x=44;
                    for(int i=sel;i<n && x<LCD_W-16;i++) {
                        size_t clen=0;
                        const char *c=ime_cand(im,i,&clen);
                        if(!c) break;
                        x=jpfont_draw(JPFONT_TEXT,strip,strip_y,strip_h,x,92,c,clen,
                                      i==sel?ink:dim);
                        x+=6;
                    }
                }
            } else {
                ascii(4,74,skk_session_status(),dim);
            }
        }

        ascii(4,LCD_H-9,"C-J KANA  ESC BACK",dim);
        ESP_ERROR_CHECK(board_present(strip_y,strip_h,strip));
    }
}
