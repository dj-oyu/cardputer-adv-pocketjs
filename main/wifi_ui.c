#include "wifi_ui.h"
#include "wifi_time.h"
#include "board.h"
#include "paint.h"
#include "skk_session.h"
#include "sound.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define TAG "wifi_ui"

// Two fields and a status block. The fields are append-and-backspace only: a
// caret you move through a column of dots is a caret you cannot see, so the
// arrows buy nothing on the field that would need them most, and an SSID is
// short enough to retype. What the screen owes the person instead is which
// stage failed, which is what wifi_time_status() already carries.
typedef enum { FIELD_SSID = 0, FIELD_PSK, FIELD_N } field_t;

static char     ssid[WIFI_TIME_SSID_MAX+1];
static char     psk[WIFI_TIME_PSK_MAX+1];
static size_t   ssid_len, psk_len;
static bool     ssid_touched, psk_touched;
static bool     stored;            // credentials were in NVS when we opened
static field_t  field;
static bool     dirty=true;
static char     notice[48];        // formatted, so it needs storage of its own
static wifi_time_status_t latest;  // sampled by wifi_ui_dirty(), drawn by draw()

// memset() on a buffer that is dead afterwards is exactly what the compiler is
// allowed to delete. The volatile pointer is what keeps the stores.
static void wipe(char *buf, size_t size) {
    volatile char *p=(volatile char *)buf;
    while(size--) *p++=0;
}

void wifi_ui_open(void) {
    // Wiped on the way out as well; this covers the paths that never reach the
    // key handler, such as the out-of-band force stop.
    wipe(psk,sizeof psk);
    psk_len=0; ssid_len=0; ssid[0]=0;
    ssid_touched=false; psk_touched=false;
    field=FIELD_SSID;
    notice[0]=0;
    dirty=true;

    // The SSID comes back out of NVS; the passphrase deliberately cannot, so
    // that field starts empty however much is stored behind it.
    stored = wifi_time_ssid_get(ssid,sizeof ssid)==ESP_OK && ssid[0]!='\0';
    if(stored) ssid_len=strlen(ssid); else ssid[0]=0;
    latest=wifi_time_status();

    // SSIDs and passphrases are ASCII. Without the reset a preedit left over
    // from the Playground commits itself into the first field.
    if(skk_session_ready()) { ime_reset(skk_session()); ime_set_on(skk_session(),false); }
}

// ---- editing --------------------------------------------------------------

static char   *field_buf(void) { return field==FIELD_SSID?ssid:psk; }
static size_t *field_len(void) { return field==FIELD_SSID?&ssid_len:&psk_len; }
static size_t  field_max(void) { return field==FIELD_SSID?WIFI_TIME_SSID_MAX:WIFI_TIME_PSK_MAX; }

static void field_insert(char c) {
    size_t *len=field_len();
    if(*len>=field_max()) { snprintf(notice,sizeof notice,"FIELD FULL"); return; }
    char *buf=field_buf();
    buf[*len]=c; (*len)++; buf[*len]=0;
    if(field==FIELD_SSID) ssid_touched=true; else psk_touched=true;
    notice[0]=0;
}

static void field_erase(void) {
    size_t *len=field_len();
    if(!*len) return;
    (*len)--; field_buf()[*len]=0;
    if(field==FIELD_SSID) ssid_touched=true; else psk_touched=true;
    notice[0]=0;
}

// ---- actions --------------------------------------------------------------

// True when NVS now holds what is on the screen.
static bool save(void) {
    if(!ssid_len) { snprintf(notice,sizeof notice,"SSID EMPTY"); return false; }
    // There is no getter for the stored passphrase, so an untouched field and a
    // stored key cannot be reconciled: writing what is on screen would silently
    // turn a WPA network into an open one. Ask for it again instead.
    if(stored && !psk_touched) { snprintf(notice,sizeof notice,"RETYPE PASSPHRASE"); return false; }
    esp_err_t err=wifi_time_credentials_set(ssid,psk);
    if(err==ESP_ERR_INVALID_SIZE) {
        // The one length rule the person can act on. Naming the rule is safe;
        // saying how many characters the field currently holds is not.
        snprintf(notice,sizeof notice,"PASSPHRASE 8..63");
        return false;
    }
    if(err!=ESP_OK) {
        snprintf(notice,sizeof notice,"SAVE FAILED %s",esp_err_to_name(err));
        ESP_LOGW(TAG,"WIFI_UI_SAVE_FAILED %s",esp_err_to_name(err));
        return false;
    }
    stored=true; ssid_touched=false; psk_touched=false;
    snprintf(notice,sizeof notice,"SAVED");
    sound_play(1);
    ESP_LOGI(TAG,"WIFI_UI_SAVED");
    return true;
}

static void start_sync(void) {
    // Write only when something was typed: syncing on stored credentials must
    // not demand the passphrase back just to run a second time.
    if((ssid_touched||psk_touched) && !save()) return;
    esp_err_t err=wifi_time_sync_start();
    if(err!=ESP_OK) { snprintf(notice,sizeof notice,"BUSY %s",esp_err_to_name(err)); return; }
    notice[0]=0;
    sound_play(1);
    ESP_LOGI(TAG,"WIFI_UI_SYNC");
}

static void clear_stored(void) {
    esp_err_t err=wifi_time_credentials_clear();
    wipe(psk,sizeof psk);
    psk_len=0; ssid_len=0; ssid[0]=0;
    ssid_touched=false; psk_touched=false; stored=false;
    field=FIELD_SSID;
    snprintf(notice,sizeof notice,"%s",err==ESP_OK?"CLEARED":"CLEAR FAILED");
}

// ---- keys -----------------------------------------------------------------

bool wifi_ui_key(const keystroke_t *k) {
    dirty=true;

    // keymap.c raises nav on the bare ` ; , . / keys, so k->nav reads KEY_DOWN
    // for a perfectly ordinary '.' in an SSID. Every branch below goes through
    // k->text, the way codeedit.c and editor.c do; nav is never consulted.
    if(k->toggle_ime) return true;      // there is nothing here to type in kana
    if(!k->len) return true;

    if(k->text[0]=='\0') {
        const char *name=k->text+1;
        size_t n=k->len-1;
        if(n==3&&!memcmp(name,"esc",3)) { wipe(psk,sizeof psk); psk_len=0; return false; }
        if(n==3&&!memcmp(name,"del",3)) { field_erase(); return true; }
        if(n==2&&!memcmp(name,"up",2))   { field=FIELD_SSID; return true; }
        if(n==4&&!memcmp(name,"down",4)) { field=FIELD_PSK;  return true; }
        return true;                    // left and right have nothing to move
    }

    switch(k->text[0]) {
        case '\b': field_erase(); return true;
        case '\t': field=(field_t)((field+1)%FIELD_N); return true;
        // Enter walks the two fields and then does the thing the screen exists
        // for, so "type, enter, type, enter" is the whole interaction.
        case '\n':
            if(field==FIELD_SSID) { field=FIELD_PSK; return true; }
            start_sync();
            return true;
        case 0x13: save(); return true;         // C-s
        case 0x12: start_sync(); return true;   // C-r
        case 0x0e: clear_stored(); return true; // C-n
        default: break;
    }
    // ASCII only. An SSID is bytes on the wire and a passphrase is compared
    // byte for byte, so a multi-byte character typed here would be a credential
    // nobody could reproduce at the access point.
    if(k->len==1 && (unsigned char)k->text[0]>=0x20 && (unsigned char)k->text[0]<0x7f)
        field_insert(k->text[0]);
    return true;
}

// ---- drawing --------------------------------------------------------------

static uint16_t *strip;
static int strip_y, strip_h;

#define LABEL_X     4
#define FIELD_X     46
#define FIELD_W     (LCD_W-FIELD_X-4)
#define FIELD_CHARS ((FIELD_W-6)/6)     // the 5x7 face on its 6 px advance
#define SSID_Y      20
#define PSK_Y       40
#define FIELD_H     14
#define STATUS_Y    66

typedef struct { uint16_t ink,dim,accent,rule; } palette_t;

// The tail of the value, because that is where typing happens and where the
// caret is. A secret field draws one dot per byte of that same window: the same
// shape as any other field, and it says nothing about the whole.
static void draw_field(int y, const char *name, const char *value, size_t len,
                       bool secret, bool focused, const palette_t *c) {
    paint_ascii(LABEL_X,y+4,name,focused?c->accent:c->dim);
    paint_fill(FIELD_X,y,FIELD_W,1,focused?c->accent:c->rule);
    paint_fill(FIELD_X,y+FIELD_H-1,FIELD_W,1,focused?c->accent:c->rule);

    size_t off = len>FIELD_CHARS ? len-FIELD_CHARS : 0;
    size_t show = len-off;
    int x=FIELD_X+3;
    if(secret) {
        for(size_t i=0;i<show;i++) paint_fill(x+(int)i*6+1,y+8,3,3,c->ink);
    } else {
        char window[FIELD_CHARS+1];
        memcpy(window,value+off,show);
        window[show]=0;
        paint_ascii(x,y+4,window,c->ink);
    }
    x+=(int)show*6;
    if(focused) paint_fill(x,y+3,1,8,c->accent);
}

// What a stage means to someone who has to fix it. wifi_time.c splits assoc
// from auth precisely so this line can send them to the right field rather
// than to a reset button.
static const char *stage_hint(wifi_time_stage_t stage) {
    switch(stage) {
    case WIFI_TIME_STAGE_NVS:  return "NO CREDENTIALS STORED";
    case WIFI_TIME_STAGE_INIT: return "RADIO DID NOT START";
    case WIFI_TIME_STAGE_ASSOC:return "NETWORK NOT FOUND";
    case WIFI_TIME_STAGE_AUTH: return "PASSPHRASE REJECTED";
    case WIFI_TIME_STAGE_DHCP: return "NO ADDRESS FROM AP";
    case WIFI_TIME_STAGE_SNTP: return "NO ANSWER FROM NTP";
    case WIFI_TIME_STAGE_NONE: break;
    }
    return "";
}

bool wifi_ui_dirty(void) {
    // The sync task runs on its own; this poll is how the screen learns that it
    // moved. It is a spinlock-guarded struct copy, and only this screen makes
    // it — the home screen's frame is untouched.
    wifi_time_status_t now=wifi_time_status();
    if(now.state!=latest.state||now.stage!=latest.stage||now.rssi!=latest.rssi||
       strcmp(now.ip,latest.ip)!=0) {
        // The end of an attempt is the one thing worth hearing without looking.
        // wifi_time.c has already logged SYNC_OK / SYNC_FAILED.
        if(now.state!=latest.state&&now.state==WIFI_TIME_OK)     sound_play(1);
        if(now.state!=latest.state&&now.state==WIFI_TIME_FAILED) sound_play(2);
        latest=now;
        dirty=true;
    }
    return dirty;
}

void wifi_ui_draw(void) {
    dirty=false;
    strip=board_strip();
    const palette_t c={
        .ink=board_rgb(220,230,242), .dim=board_rgb(92,116,146),
        .accent=board_rgb(120,200,255), .rule=board_rgb(22,38,58),
    };
    const uint16_t warn=board_rgb(240,180,110), good=board_rgb(150,220,160);

    // Composed once, then drawn into all seventeen strips.
    char line1[42], line2[42];
    uint16_t state_colour=c.dim;
    switch(latest.state) {
    case WIFI_TIME_RUNNING:
        snprintf(line1,sizeof line1,"SYNCING  %s",wifi_time_stage_name(latest.stage));
        snprintf(line2,sizeof line2,"%s",latest.ip);
        state_colour=c.accent;
        break;
    case WIFI_TIME_OK:
        snprintf(line1,sizeof line1,"CLOCK SET FROM NTP (UTC)");
        snprintf(line2,sizeof line2,"%s  RSSI %d",latest.ip,latest.rssi);
        state_colour=good;
        break;
    case WIFI_TIME_FAILED:
        snprintf(line1,sizeof line1,"FAILED AT %s",wifi_time_stage_name(latest.stage));
        snprintf(line2,sizeof line2,"%s",stage_hint(latest.stage));
        state_colour=warn;
        break;
    case WIFI_TIME_IDLE:
        snprintf(line1,sizeof line1,"%s",stored?"CREDENTIALS STORED":"NO CREDENTIALS");
        snprintf(line2,sizeof line2,"C-R SYNCS THE CLOCK");
        break;
    }

    for(strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        strip_h=LCD_H-strip_y<STRIP_H?LCD_H-strip_y:STRIP_H;
        paint_begin(strip,strip_y,strip_h);
        for(int i=0;i<LCD_W*strip_h;i++) strip[i]=board_rgb(6,11,20);

        paint_ascii(LABEL_X,3,"WIFI TIME SYNC",c.dim);
        if(notice[0]) paint_ascii(110,3,notice,warn);
        paint_fill(0,13,LCD_W,1,c.rule);

        draw_field(SSID_Y,"SSID",ssid,ssid_len,false,field==FIELD_SSID,&c);
        draw_field(PSK_Y ,"PASS",psk ,psk_len ,true ,field==FIELD_PSK ,&c);

        paint_fill(0,STATUS_Y-6,LCD_W,1,c.rule);
        paint_ascii(LABEL_X,STATUS_Y,line1,state_colour);
        paint_ascii(LABEL_X,STATUS_Y+12,line2,c.dim);

        paint_fill(0,LCD_H-11,LCD_W,1,c.rule);
        paint_ascii(LABEL_X,LCD_H-8,"TAB FIELD C-S SAVE C-R SYNC ESC BACK",c.dim);
        ESP_ERROR_CHECK(board_present(strip_y,strip_h,strip));
    }
}
