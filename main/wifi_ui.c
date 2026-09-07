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

// The 240x135 layout, in one place: the picker scrolls six rows between the
// header rule and the status block, and the two entry views reuse that space.
#define LABEL_X     4
#define LINE_H      12
#define LIST_TOP    18
#define LIST_ROWS_VISIBLE 6
#define STATUS_Y    94
#define FIELD_X     46
#define FIELD_W     (LCD_W-FIELD_X-4)
#define FIELD_CHARS ((FIELD_W-6)/6)     // the 5x7 face on its 6 px advance
#define NAME_CHARS  27                  // the row's name column, before the badges
#define BADGE_X     176                 // padlock
#define BARS_X      196                 // signal


// Three views over one job: choose a network, give it a passphrase, watch the
// sync. The SSID is picked from a scan rather than typed because a typed one
// can only ever fail as WIFI_REASON_NO_AP_FOUND, and that single reason covers
// both "the network is not here" and "you spelled it wrong" — opposite things
// to do about it. Typing survives only as the OTHER... row, for a hidden
// network the scan is not allowed to report.
typedef enum {
    VIEW_LIST = 0,   // the picker
    VIEW_SSID,       // hand-typed name, reached from OTHER...
    VIEW_PSK,        // passphrase for whatever was chosen
} view_t;

// What a row of the picker is. The list is the stored network (when there is
// one), then the scan strongest first, then OTHER... — so Enter on the first
// row is "sync with what I already gave you", which is the common case and the
// one that must not demand the passphrase back.
typedef enum { ROW_SYNC = 0, ROW_NETWORK, ROW_OTHER } row_kind_t;

static char     ssid[WIFI_TIME_SSID_MAX+1];
static char     psk[WIFI_TIME_PSK_MAX+1];
static size_t   ssid_len, psk_len;
static bool     psk_touched;
static bool     chosen_secure=true;   // the picked network wants a passphrase
static char     stored_ssid[WIFI_TIME_SSID_MAX+1];
static bool     stored;               // credentials were in NVS when we looked
static view_t   view;
static unsigned sel, top;             // selected row, first drawn row
static bool     dirty=true;
static char     notice[48];           // formatted, so it needs storage of its own

static wifi_time_network_t nets[WIFI_TIME_SCAN_MAX];
static unsigned nets_n;
static bool     nets_truncated;
static wifi_time_state_t scan_shown=WIFI_TIME_IDLE;
static wifi_time_status_t latest;     // sampled by wifi_ui_dirty(), drawn by draw()

// memset() on a buffer that is dead afterwards is exactly what the compiler is
// allowed to delete. The volatile pointer is what keeps the stores.
static void wipe(char *buf, size_t size) {
    volatile char *p=(volatile char *)buf;
    while(size--) *p++=0;
}

// ---- the picker's rows ----------------------------------------------------

static unsigned row_count(void) { return (stored?1u:0u)+nets_n+1u; }

// Which kind of row `i` is, and for a network row which entry of nets.
static row_kind_t row_at(unsigned i, unsigned *index) {
    unsigned at=i;
    if(stored) { if(at==0) return ROW_SYNC; at--; }
    if(at<nets_n) { if(index) *index=at; return ROW_NETWORK; }
    return ROW_OTHER;
}

static void follow_selection(void) {
    unsigned n=row_count();
    if(sel>=n) sel=n?n-1:0;
    if(sel<top) top=sel;
    else if(sel>=top+LIST_ROWS_VISIBLE) top=sel-LIST_ROWS_VISIBLE+1;
}

// ---- scanning -------------------------------------------------------------

// Only when the radio is free. A scan started while a sync holds it would come
// back ESP_ERR_INVALID_STATE and read to the person as a broken button; the
// state we already have says so without asking.
static void begin_scan(bool announce) {
    if(wifi_time_scan_state()==WIFI_TIME_RUNNING) return;
    if(latest.state==WIFI_TIME_RUNNING) {
        if(announce) snprintf(notice,sizeof notice,"SYNC RUNNING");
        return;
    }
    esp_err_t err=wifi_time_scan_start();
    if(err!=ESP_OK) {
        snprintf(notice,sizeof notice,"SCAN BUSY %s",esp_err_to_name(err));
        return;
    }
    notice[0]=0;
    scan_shown=WIFI_TIME_RUNNING;
    ESP_LOGI(TAG,"WIFI_UI_SCAN");
}

// The results are only written when the scan task finishes, so a state change
// is the whole of "there is something new to copy".
static void take_scan_results(void) {
    nets_n=wifi_time_scan_networks(nets,WIFI_TIME_SCAN_MAX,&nets_truncated);
    follow_selection();
}

void wifi_ui_open(void) {
    // Wiped on the way out too; this covers the paths that never reach the key
    // handler, such as the out-of-band force stop.
    wipe(psk,sizeof psk);
    psk_len=0; psk_touched=false;
    ssid_len=0; ssid[0]=0;
    chosen_secure=true;
    view=VIEW_LIST; sel=0; top=0;
    notice[0]=0;
    dirty=true;

    // The SSID comes back out of NVS; the passphrase deliberately cannot, so
    // the stored network is offered as a row to sync rather than as a form to
    // fill in again.
    stored = wifi_time_ssid_get(stored_ssid,sizeof stored_ssid)==ESP_OK
             && stored_ssid[0]!='\0';
    if(!stored) stored_ssid[0]=0;
    latest=wifi_time_status();

    // Re-entering mid-scan keeps the scan that is already running, and a
    // finished one keeps its results: the list survives a trip to the home
    // screen, which is what someone who left to read a router label expects.
    scan_shown=wifi_time_scan_state();
    if(scan_shown==WIFI_TIME_OK) take_scan_results();
    else if(scan_shown!=WIFI_TIME_RUNNING) { nets_n=0; nets_truncated=false; }
    follow_selection();
    begin_scan(false);

    // SSIDs and passphrases are ASCII. Without the reset a preedit left over
    // from the Playground commits itself into the first field.
    if(skk_session_ready()) { ime_reset(skk_session()); ime_set_on(skk_session(),false); }
}

// ---- editing --------------------------------------------------------------

static char   *field_buf(void) { return view==VIEW_SSID?ssid:psk; }
static size_t *field_len(void) { return view==VIEW_SSID?&ssid_len:&psk_len; }
static size_t  field_max(void) { return view==VIEW_SSID?WIFI_TIME_SSID_MAX:WIFI_TIME_PSK_MAX; }

static void field_insert(char c) {
    size_t *len=field_len();
    if(*len>=field_max()) { snprintf(notice,sizeof notice,"FIELD FULL"); return; }
    char *buf=field_buf();
    buf[*len]=c; (*len)++; buf[*len]=0;
    if(view==VIEW_PSK) psk_touched=true;
    notice[0]=0;
}

static void field_erase(void) {
    size_t *len=field_len();
    if(!*len) return;
    (*len)--; field_buf()[*len]=0;
    // Backspacing to empty is still an answer: it is how an open network that
    // the scan reported as secured gets an empty passphrase past the guard.
    if(view==VIEW_PSK) psk_touched=true;
    notice[0]=0;
}

// ---- actions --------------------------------------------------------------

// True when NVS now holds what is on the screen.
static bool save(void) {
    if(!ssid_len) { snprintf(notice,sizeof notice,"SSID EMPTY"); return false; }
    // There is no getter for the stored passphrase, so a secured network can
    // never be saved from a field nobody typed into: writing what is on screen
    // would silently turn a WPA network into an open one.
    if(chosen_secure && !psk_touched) {
        snprintf(notice,sizeof notice,"PASSPHRASE REQUIRED");
        return false;
    }
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
    stored=true;
    snprintf(stored_ssid,sizeof stored_ssid,"%s",ssid);
    psk_touched=false;
    snprintf(notice,sizeof notice,"SAVED");
    sound_play(1);
    ESP_LOGI(TAG,"WIFI_UI_SAVED");
    return true;
}

// `write` is false for the stored row, which has nothing new to store and must
// not be made to ask for a passphrase it cannot read back.
static void start_sync(bool write) {
    if(write && !save()) return;
    esp_err_t err=wifi_time_sync_start();
    if(err==ESP_ERR_INVALID_STATE) {
        // The scan and the sync share one radio and one lock. This is what
        // pressing sync during a scan looks like, and it clears on its own.
        snprintf(notice,sizeof notice,
                 wifi_time_scan_state()==WIFI_TIME_RUNNING?"SCAN RUNNING":"RADIO BUSY");
        return;
    }
    if(err!=ESP_OK) { snprintf(notice,sizeof notice,"SYNC %s",esp_err_to_name(err)); return; }
    if(!write) notice[0]=0;
    // Back to the list to watch it. A save just added the stored row, so the
    // selection is put on it rather than left pointing one row off what it was.
    view=VIEW_LIST; sel=0; top=0;
    follow_selection();
    sound_play(1);
    ESP_LOGI(TAG,"WIFI_UI_SYNC");
}

static void clear_stored(void) {
    esp_err_t err=wifi_time_credentials_clear();
    wipe(psk,sizeof psk);
    psk_len=0; psk_touched=false;
    ssid_len=0; ssid[0]=0;
    stored=false; stored_ssid[0]=0;
    view=VIEW_LIST; sel=0; top=0;
    follow_selection();
    snprintf(notice,sizeof notice,"%s",err==ESP_OK?"CLEARED":"CLEAR FAILED");
}

static void choose(void) {
    unsigned index=0;
    switch(row_at(sel,&index)) {
    case ROW_SYNC:
        start_sync(false);
        return;
    case ROW_NETWORK:
        snprintf(ssid,sizeof ssid,"%s",nets[index].ssid);
        ssid_len=strlen(ssid);
        chosen_secure=nets[index].secure;
        wipe(psk,sizeof psk); psk_len=0; psk_touched=false;
        view=VIEW_PSK;
        notice[0]=0;
        return;
    case ROW_OTHER:
        // Nothing is known about a network the scan never saw, so it is
        // treated as secured: an open hidden network is reached by clearing
        // the passphrase field, which counts as having answered.
        ssid[0]=0; ssid_len=0;
        chosen_secure=true;
        wipe(psk,sizeof psk); psk_len=0; psk_touched=false;
        view=VIEW_SSID;
        notice[0]=0;
        return;
    }
}

// ---- keys -----------------------------------------------------------------

// keymap.c raises nav on the bare ` ; , . / keys, so k->nav reads KEY_DOWN for
// a perfectly ordinary '.' in a passphrase. Everything below goes through
// k->text, the way codeedit.c and editor.c do; nav is never consulted. The
// picker has nothing to type into, so it accepts the bare ; and . as well as
// their Fn tokens — on that view they are the arrows their legend says.
static bool list_key(const keystroke_t *k) {
    if(k->text[0]=='\0') {
        const char *name=k->text+1;
        size_t n=k->len-1;
        if(n==3&&!memcmp(name,"esc",3)) return false;
        if(n==2&&!memcmp(name,"up",2))   { if(sel) sel--; follow_selection(); return true; }
        if(n==4&&!memcmp(name,"down",4)) { sel++; follow_selection(); return true; }
        return true;
    }
    switch(k->text[0]) {
        case ';': if(sel) sel--; follow_selection(); return true;
        case '.': sel++; follow_selection(); return true;
        case '\n': choose(); return true;
        case 0x12: begin_scan(true); return true;   // C-r
        case 0x0e: clear_stored(); return true;     // C-n
        default: break;
    }
    return true;
}

static bool field_key(const keystroke_t *k) {
    if(k->text[0]=='\0') {
        const char *name=k->text+1;
        size_t n=k->len-1;
        // Escape steps back one view rather than leaving: on the passphrase
        // field the thing you want to undo is almost always the network.
        if(n==3&&!memcmp(name,"esc",3)) {
            wipe(psk,sizeof psk); psk_len=0; psk_touched=false;
            view=VIEW_LIST; notice[0]=0;
            return true;
        }
        if(n==3&&!memcmp(name,"del",3)) { field_erase(); return true; }
        return true;                    // the arrows have nothing to move
    }
    switch(k->text[0]) {
        case '\b': field_erase(); return true;
        case '\n':
            if(view==VIEW_SSID) {
                if(!ssid_len) { snprintf(notice,sizeof notice,"SSID EMPTY"); return true; }
                view=VIEW_PSK;
                return true;
            }
            start_sync(true);
            return true;
        case 0x13: save(); return true;   // C-s
        default: break;
    }
    // ASCII only. An SSID is bytes on the wire and a passphrase is compared
    // byte for byte, so a multi-byte character typed here would be a credential
    // nobody could reproduce at the access point.
    if(k->len==1 && (unsigned char)k->text[0]>=0x20 && (unsigned char)k->text[0]<0x7f)
        field_insert(k->text[0]);
    return true;
}

bool wifi_ui_key(const keystroke_t *k) {
    dirty=true;
    if(k->toggle_ime) return true;      // there is nothing here to type in kana
    if(!k->len) return true;
    if(view==VIEW_LIST) {
        if(list_key(k)) return true;
        wipe(psk,sizeof psk); psk_len=0;
        return false;
    }
    return field_key(k);
}

// ---- drawing --------------------------------------------------------------

static uint16_t *strip;
static int strip_y, strip_h;

typedef struct { uint16_t ink,dim,accent,rule,warn,good; } palette_t;
static palette_t colour;

// Four bars, because the number behind them is not one a person can act on:
// -55 dBm and -60 dBm pick the same network. RSSI is bucketed rather than
// printed for the same reason the stage name is shown instead of the errno.
static void draw_signal(int x, int y, int rssi) {
    int bars = rssi>=-55 ? 4 : rssi>=-65 ? 3 : rssi>=-75 ? 2 : 1;
    for(int i=0;i<4;i++) {
        int h=2+i*2;
        paint_fill(x+i*5,y+8-h,3,h,i<bars?colour.ink:colour.rule);
    }
}

// A padlock: a 6x4 body under a 4 px shackle. Drawn rather than spelled out
// because the name column is the scarce thing on a 240 px row.
static void draw_lock(int x, int y) {
    paint_fill(x+1,y,4,1,colour.dim);
    paint_fill(x+1,y+1,1,2,colour.dim);
    paint_fill(x+4,y+1,1,2,colour.dim);
    paint_fill(x,y+3,6,4,colour.dim);
}

static void draw_row(int y, unsigned i) {
    bool on = i==sel;
    if(on) paint_fill(0,y-2,LCD_W,LINE_H,board_rgb(16,30,48));
    paint_ascii(LABEL_X,y,on?">":" ",colour.accent);
    unsigned index=0;
    char name[NAME_CHARS+1];
    switch(row_at(i,&index)) {
    case ROW_SYNC:
        // The name column is 27 characters; a 32 byte SSID is cut to fit rather
        // than allowed to run under the signal badges.
        snprintf(name,sizeof name,"SYNC TIME  %.*s",NAME_CHARS-11,stored_ssid);
        paint_ascii(14,y,name,on?colour.ink:colour.dim);
        return;
    case ROW_OTHER:
        paint_ascii(14,y,"OTHER...",on?colour.ink:colour.dim);
        return;
    case ROW_NETWORK:
        snprintf(name,sizeof name,"%.*s",NAME_CHARS,nets[index].ssid);
        paint_ascii(14,y,name,on?colour.ink:colour.dim);
        if(nets[index].secure) draw_lock(BADGE_X,y);
        draw_signal(BARS_X,y,nets[index].rssi);
        return;
    }
}

// The tail of the value, because that is where typing happens and where the
// caret is. A secret field draws one dot per byte of that same window: the same
// shape as any other field, and it says nothing about the whole.
static void draw_field(int y, const char *name, const char *value, size_t len,
                       bool secret) {
    paint_ascii(LABEL_X,y+4,name,colour.accent);
    paint_fill(FIELD_X,y,FIELD_W,1,colour.accent);
    paint_fill(FIELD_X,y+13,FIELD_W,1,colour.accent);

    size_t off = len>FIELD_CHARS ? len-FIELD_CHARS : 0;
    size_t show = len-off;
    int x=FIELD_X+3;
    if(secret) {
        for(size_t i=0;i<show;i++) paint_fill(x+(int)i*6+1,y+8,3,3,colour.ink);
    } else {
        char window[FIELD_CHARS+1];
        memcpy(window,value+off,show);
        window[show]=0;
        paint_ascii(x,y+4,window,colour.ink);
    }
    x+=(int)show*6;
    paint_fill(x,y+3,1,8,colour.accent);
}

// What a stage means to someone who has to fix it. wifi_time.c splits assoc
// from auth precisely so this line can send them to the right place rather
// than to a reset button.
static const char *stage_hint(wifi_time_stage_t stage) {
    switch(stage) {
    case WIFI_TIME_STAGE_NVS:  return "NO CREDENTIALS STORED";
    case WIFI_TIME_STAGE_INIT: return "RADIO DID NOT START";
    case WIFI_TIME_STAGE_ASSOC:return "NETWORK WENT AWAY";
    case WIFI_TIME_STAGE_AUTH: return "PASSPHRASE REJECTED";
    case WIFI_TIME_STAGE_DHCP: return "NO ADDRESS FROM AP";
    case WIFI_TIME_STAGE_SNTP: return "NO ANSWER FROM NTP";
    case WIFI_TIME_STAGE_NONE: break;
    }
    return "";
}

bool wifi_ui_dirty(void) {
    // The sync and scan tasks run on their own; these polls are how the screen
    // learns that they moved. Both are spinlock-guarded reads, and only this
    // screen makes them — the home screen's frame is untouched.
    wifi_time_state_t scan=wifi_time_scan_state();
    if(scan!=scan_shown) {
        scan_shown=scan;
        if(scan==WIFI_TIME_OK) take_scan_results();
        else if(scan==WIFI_TIME_FAILED) { nets_n=0; follow_selection(); }
        dirty=true;
    }
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

// The scan's own line, which is a different thing from the sync's: running,
// failed and found-nothing are three answers, not one empty list.
static void scan_line(char *out, size_t size) {
    switch(scan_shown) {
    case WIFI_TIME_RUNNING: snprintf(out,size,"SCANNING"); return;
    case WIFI_TIME_FAILED:  snprintf(out,size,"SCAN FAILED"); return;
    case WIFI_TIME_OK:
        if(!nets_n) { snprintf(out,size,"NO NETWORKS"); return; }
        snprintf(out,size,nets_truncated?"%u NETWORKS +MORE":"%u NETWORKS",nets_n);
        return;
    case WIFI_TIME_IDLE:    snprintf(out,size,"C-R SCANS"); return;
    }
}

void wifi_ui_draw(void) {
    dirty=false;
    strip=board_strip();
    colour=(palette_t){
        .ink=board_rgb(220,230,242), .dim=board_rgb(92,116,146),
        .accent=board_rgb(120,200,255), .rule=board_rgb(22,38,58),
        .warn=board_rgb(240,180,110), .good=board_rgb(150,220,160),
    };

    // Composed once, then drawn into all seventeen strips.
    char head[24], line1[42], line2[42];
    scan_line(head,sizeof head);
    uint16_t state_colour=colour.dim;
    switch(latest.state) {
    case WIFI_TIME_RUNNING:
        snprintf(line1,sizeof line1,"SYNCING  %s",wifi_time_stage_name(latest.stage));
        snprintf(line2,sizeof line2,"%s",latest.ip);
        state_colour=colour.accent;
        break;
    case WIFI_TIME_OK:
        snprintf(line1,sizeof line1,"CLOCK SET FROM NTP (UTC)");
        snprintf(line2,sizeof line2,"%s  RSSI %d",latest.ip,latest.rssi);
        state_colour=colour.good;
        break;
    case WIFI_TIME_FAILED:
        snprintf(line1,sizeof line1,"FAILED AT %s",wifi_time_stage_name(latest.stage));
        snprintf(line2,sizeof line2,"%s",stage_hint(latest.stage));
        state_colour=colour.warn;
        break;
    case WIFI_TIME_IDLE:
        snprintf(line1,sizeof line1,"%s",stored?"CREDENTIALS STORED":"NO CREDENTIALS");
        snprintf(line2,sizeof line2,"PICK A NETWORK TO BEGIN");
        break;
    }
    // A notice is what the last key press did, so it outranks the standing
    // status on the one line they share.
    if(notice[0]) snprintf(line2,sizeof line2,"%.*s",(int)sizeof line2-1,notice);

    const char *foot = view==VIEW_LIST ? "ENTER SELECT  C-R RESCAN  ESC BACK"
                     : view==VIEW_SSID ? "ENTER NEXT  ESC LIST"
                                       : "ENTER SAVE+SYNC  C-S SAVE  ESC LIST";

    for(strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        strip_h=LCD_H-strip_y<STRIP_H?LCD_H-strip_y:STRIP_H;
        paint_begin(strip,strip_y,strip_h);
        for(int i=0;i<LCD_W*strip_h;i++) strip[i]=board_rgb(6,11,20);

        paint_ascii(LABEL_X,3,"WI-FI",colour.dim);
        if(view==VIEW_LIST) paint_ascii(46,3,head,colour.accent);
        paint_fill(0,13,LCD_W,1,colour.rule);

        if(view==VIEW_LIST) {
            unsigned n=row_count();
            for(unsigned r=0;r<LIST_ROWS_VISIBLE&&top+r<n;r++)
                draw_row(LIST_TOP+(int)r*LINE_H,top+r);
        } else {
            paint_ascii(LABEL_X,LIST_TOP,"NETWORK",colour.dim);
            paint_ascii(66,LIST_TOP,view==VIEW_SSID?"(HIDDEN)":ssid,colour.ink);
            if(view==VIEW_SSID) draw_field(LIST_TOP+18,"NAME",ssid,ssid_len,false);
            else                draw_field(LIST_TOP+18,"PASS",psk,psk_len,true);
            if(view==VIEW_PSK && !chosen_secure)
                paint_ascii(LABEL_X,LIST_TOP+38,"OPEN NETWORK - LEAVE EMPTY",colour.dim);
        }

        paint_fill(0,STATUS_Y-6,LCD_W,1,colour.rule);
        paint_ascii(LABEL_X,STATUS_Y,line1,state_colour);
        paint_ascii(LABEL_X,STATUS_Y+12,line2,notice[0]?colour.warn:colour.dim);

        paint_fill(0,LCD_H-11,LCD_W,1,colour.rule);
        paint_ascii(LABEL_X,LCD_H-8,foot,colour.dim);
        ESP_ERROR_CHECK(board_present(strip_y,strip_h,strip));
    }
}
