#include "tutorial.h"
#include "lessons.h"
#include "codeedit.h"
#include "srcstore.h"
#include "jsconsole.h"
#include "jpfont.h"
#include "board.h"
#include "sound.h"
#include "fonts.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define LINE_H 12
#define BODY_TOP 16

static unsigned chapter;
static bool     reference;      // Tab opened the number list
static bool     cleared;        // the current chapter has been reached
static bool     saw_error;      // chapter 2: an exception has been seen
static tutorial_state_t state;
static bool     dirty=true;
static char     verdict[40];

// The prelude and the learner's lines, joined for the run. One buffer rather
// than making codeedit hold a longer source: the join is only ever read.
static char joined[SRC_MAX+512];
static size_t joined_len;

static uint16_t *strip;
static int strip_y, strip_h;

tutorial_state_t tutorial_state(void) { return state; }

// While the Playground is up it owns the display, so its repaints have to be
// reported as this screen's. Without the second half a keystroke reached the
// buffer and the screen never redrew, which looks exactly like a keyboard
// that has stopped working.
bool tutorial_dirty(void) {
    return dirty || (state==TUTORIAL_WRITING && code_dirty());
}

// Where to resume: the chapter after the last one reached, so a power cycle
// does not send anyone back to the beginning. The last chapter is one people
// come back to and play with, so progress stops there rather than wrapping.
static unsigned recall_progress(void) {
    nvs_handle_t h;
    uint8_t v=0;
    if(nvs_open("tutorial",NVS_READONLY,&h)==ESP_OK) {
        nvs_get_u8(h,"next",&v);
        nvs_close(h);
    }
    return v<lesson_count()?v:0;
}
static void remember_progress(void) {
    unsigned next=chapter+1;
    if(next>=lesson_count()) next=lesson_count()-1;
    if(next<=recall_progress()) return;
    nvs_handle_t h;
    if(nvs_open("tutorial",NVS_READWRITE,&h)!=ESP_OK) return;
    nvs_set_u8(h,"next",(uint8_t)next);
    nvs_commit(h);
    nvs_close(h);
}

static void enter_chapter(unsigned index) {
    chapter=index;
    cleared=false; saw_error=false; reference=false;
    verdict[0]=0;
    state=TUTORIAL_READING;
    dirty=true;
}

void tutorial_open(void) {
    enter_chapter(recall_progress());
    ESP_LOGI("tutorial","OPEN chapter=%u of %u",chapter+1,lesson_count());
}

const char *tutorial_source(size_t *len) {
    const lesson_t *l=lesson_at(chapter);
    size_t n=0;
    const char *written=code_source(&n);
    joined_len=0;
    if(l->prelude) {
        size_t p=strlen(l->prelude);
        if(p<sizeof(joined)) { memcpy(joined,l->prelude,p); joined_len=p; }
    }
    if(joined_len+n<sizeof(joined)) {
        memcpy(joined+joined_len,written,n);
        joined_len+=n;
    }
    joined[joined_len]=0;
    *len=joined_len;
    return joined;
}

// ---- did they get there? --------------------------------------------------

static bool console_has(const char *want, bool prefix) {
    unsigned n=jsconsole_count();
    for(unsigned i=0;i<n;i++) {
        const char *line=jsconsole_line(i);
        if(prefix ? !strncmp(line,want,strlen(want)) : !strcmp(line,want))
            return true;
    }
    return false;
}
// codeedit keeps its buffer NUL terminated, so strstr is safe on it.
static bool source_has(const char *want) {
    size_t n=0;
    const char *src=code_source(&n);
    return want && strstr(src,want)!=NULL;
}
static bool source_has_japanese(void) {
    size_t n=0;
    const char *src=code_source(&n);
    for(size_t i=0;i<n;i++) if((unsigned char)src[i]>=0x80) return true;
    return false;
}

void tutorial_ran(esp_err_t started, const char *error) {
    const lesson_t *l=lesson_at(chapter);
    bool threw = error && error[0];
    bool clean = !threw;
    bool ok=false;

    switch(l->check) {
        case CHECK_PRINTS:
            ok = clean && started==ESP_ERR_NOT_FOUND && console_has(l->want,false);
            break;
        case CHECK_ERROR_THEN_PRINTS:
            // The chapter is about reading a mistake, so making one is part of
            // it: a run that was right the first time has not been through it.
            if(threw && strstr(error,l->want)) { saw_error=true; break; }
            ok = saw_error && clean && console_has(l->want2,false);
            break;
        case CHECK_PRINTS_BOTH:
            ok = clean && console_has(l->want,false) && console_has(l->want2,false);
            break;
        case CHECK_CHANGED:
            ok = clean && started==ESP_OK && !source_has(l->want);
            break;
        case CHECK_PRINTS_PREFIX:
            ok = clean && started==ESP_OK && console_has(l->want,true);
            break;
        case CHECK_JAPANESE:
            ok = clean && started==ESP_OK && source_has(l->want) && source_has_japanese();
            break;
    }

    if(ok && !cleared) {
        cleared=true;
        sound_play(1);
        remember_progress();
        ESP_LOGI("tutorial","CHAPTER %u CLEARED",chapter+1);
    }
    bool last = chapter+1>=lesson_count();
    snprintf(verdict,sizeof(verdict),"%s",
             cleared ? (last?"全部できた!":"できた! Enter で次へ") :
             threw   ? "赤い字を読んで直そう" :
                       "まだ。もう一度 Ctrl+R");

    // An attempt that missed stays in the editor. The message it has to act on
    // — the exception, or what print produced — is drawn there and nowhere
    // else, so bouncing back to the chapter would hide the very thing chapter
    // two is about. Only success is worth changing screens for.
    code_returned(cleared?NULL:verdict);
    state = cleared ? TUTORIAL_READING : TUTORIAL_WRITING;
    dirty=true;
}

// ---- keys -----------------------------------------------------------------

static void open_editor(void) {
    const lesson_t *l=lesson_at(chapter);
    code_open_lesson(chapter, l->preload?l->code:"", l->preload?strlen(l->code):0);
    state=TUTORIAL_WRITING;
    dirty=true;
}

bool tutorial_key(const keystroke_t *k) {
    if(state==TUTORIAL_WRITING) {
        if(code_key(k)) return true;
        state=TUTORIAL_READING;      // Esc in the editor comes back here
        dirty=true;
        return true;
    }

    dirty=true;
    if(!k->len) return true;

    // Nothing is typed on this screen, so it steers by the navigation key the
    // way the home screen does: the bare , and . work, not only their Fn
    // arrows. keymap fills `nav` for both.
    switch(k->nav) {
        case KEY_BACK:  return false;
        case KEY_LEFT:  if(chapter) enter_chapter(chapter-1); return true;
        case KEY_RIGHT: if(chapter+1<lesson_count()) enter_chapter(chapter+1); return true;
        case KEY_ENTER:
            if(cleared && chapter+1<lesson_count()) enter_chapter(chapter+1);
            else open_editor();
            return true;
        default: break;
    }
    if(k->text[0]=='\t') { reference=!reference; return true; }
    // Del throws away this chapter's working copy. A chapter only seeds its
    // example into an empty slot, so without this a learner who edited one
    // into a mess had no way back to it.
    if(k->text[0]=='\0' && k->len==4 && !memcmp(k->text+1,"del",3)) {
        srcstore_clear(SRC_SLOT_LESSON+chapter);
        cleared=false; saw_error=false;
        snprintf(verdict,sizeof(verdict),"%s","この章を最初から");
        ESP_LOGI("tutorial","CHAPTER %u RESET",chapter+1);
        return true;
    }
    return true;
}

// ---- drawing --------------------------------------------------------------

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
static void jp(int x,int y,const char *s,uint16_t colour) {
    if(!s||!*s) return;
    jpfont_draw(JPFONT_TEXT,strip,strip_y,strip_h,x,y,s,strlen(s),colour);
}
static void fill(int x,int y,int w,int h,uint16_t colour) {
    for(int r=0;r<h;r++) {
        int py=y+r-strip_y;
        if(py<0||py>=strip_h) continue;
        for(int c=0;c<w;c++) if(x+c>=0&&x+c<LCD_W) strip[py*LCD_W+x+c]=colour;
    }
}

void tutorial_draw(void) {
    if(state==TUTORIAL_WRITING) { code_draw(); dirty=false; return; }
    dirty=false;
    strip=board_strip();
    const lesson_t *l=lesson_at(chapter);
    const uint16_t ink   =board_rgb(222,232,244);
    const uint16_t dim   =board_rgb(96,118,146);
    const uint16_t accent=board_rgb(120,200,255);
    const uint16_t good  =board_rgb(130,230,150);
    const uint16_t rule  =board_rgb(24,40,62);

    char progress[12];
    snprintf(progress,sizeof(progress),"%u/%u",chapter+1,lesson_count());

    for(strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        strip_h=LCD_H-strip_y<STRIP_H?LCD_H-strip_y:STRIP_H;
        for(int i=0;i<LCD_W*strip_h;i++) strip[i]=board_rgb(6,11,20);

        jp(4,1,reference?"番号の意味":l->title,accent);
        ascii(LCD_W-28,3,progress,dim);
        fill(0,14,LCD_W,1,rule);

        if(reference) {
            for(unsigned r=0;r<LESSON_REF_ROWS;r++)
                jp(6,BODY_TOP+(int)r*LINE_H,lesson_reference(r),ink);
        } else {
            for(unsigned r=0;r<LESSON_BODY_ROWS;r++)
                jp(6,BODY_TOP+(int)r*LINE_H,l->body[r],ink);
            jp(6,BODY_TOP+LESSON_BODY_ROWS*LINE_H,l->hint,dim);
            if(verdict[0]) jp(6,92,verdict,cleared?good:accent);
        }

        fill(0,LCD_H-13,LCD_W,1,rule);
        jp(4,LCD_H-12,reference?"Tab:もどる":
           cleared?"Enter:次へ Tab:番号 ←→:章":
                   "Enter:書く Tab:番号 Del:戻す",dim);
        ESP_ERROR_CHECK(board_present(strip_y,strip_h,strip));
    }
}
