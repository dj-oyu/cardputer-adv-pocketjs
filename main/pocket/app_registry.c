#include "app_registry.h"
#include "utf8.h"
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ the table
//
// One row per thing this firmware can start. The names are the ones section 3
// uses in its example, and every capability listed below is a name pocket_api.c
// answers for — a manifest that asks for a capability nobody registers refuses
// its own app at start, which is the point, but it should refuse because the
// board cannot do it and not because of a typo here.

static const char *const CAPS_NONE[]   = {NULL};
static const char *const CAPS_KV[]     = {"storage.kv", NULL};
static const char *const CAPS_IMU[]    = {"sensors.imu", NULL};
static const char *const CAPS_PET_OPT[]= {"sensors.imu", "audio.tone", NULL};
static const char *const CAPS_WORK[]   = {"workspace", NULL};
static const char *const CAPS_COMP[]   = {"net.http", "audio.tone", NULL};
static const char *const CAPS_OVERLAY[]= {"ui.overlay", "time", NULL};
static const char *const CAPS_PLAY[]    = {"audio.playback", "fs.volume.sd", NULL};

// The one range this build knows. It is written out per row rather than shared
// so that a row can be moved forward on its own, which is what a version range
// per app is for.
#define API_0_1 ">=0.1.0 <0.2.0"

static const app_manifest_t MANIFESTS[] = {
    // The built-in demo. Legacy, no storage, no works: it draws and counts.
    {.id="local.hello", .title="HELLO WORLD", .entry="apps/hello/main.js",
     .runtime=APP_RUNTIME_LEGACY, .api=NULL,
     .required=CAPS_NONE, .optional=CAPS_NONE, .works=APP_WORKS_NONE},

    // The calibration program exists for a board whose sensor is wrong, so the
    // sensor is required: starting it without one would show axes that are not
    // measurements.
    {.id="local.imucal", .title="IMU CALIBRATION", .entry="apps/imucal/imucal.js",
     .runtime=APP_RUNTIME_LEGACY, .api=NULL,
     .required=CAPS_IMU, .optional=CAPS_NONE, .works=APP_WORKS_NONE},

    // The pet keeps state between runs, so its store is required; the tilt and
    // the sound are what make it pleasant and not what make it work.
    {.id="local.pet", .title="POCKET PET", .entry="apps/pet/pet.js",
     .runtime=APP_RUNTIME_LEGACY, .api=NULL,
     .required=CAPS_KV, .optional=CAPS_PET_OPT, .works=APP_WORKS_NONE},

    {.id="local.companion", .title="PET COMPANION", .entry="apps/companion/companion.js",
     .runtime=APP_RUNTIME_LEGACY, .api=NULL,
     .required=CAPS_KV, .optional=CAPS_COMP, .works=APP_WORKS_NONE},

    // Both of its capabilities are REQUIRED rather than optional, and that is
    // not strictness for its own sake: without the card there is nothing to
    // choose and without playback there is nothing to do with a choice, so an
    // admitted-but-useless session would only be able to draw its own excuse.
    //
    // It is also the first of the audio apps to have a row at all. Until now
    // local.streamplay, local.opusplay, local.mp3play and local.opusfit all
    // fell through app_registry_select() to local.hello and ran under HELLO
    // WORLD's identity -- which is why the log said "APP_ID local.hello" while
    // this app's own source was running. That matters here in a way it did not
    // for them: identity is what pocket_fs_set_owner() and
    // pocket_storage_set_owner() are given, so a player that ever remembers a
    // track would have remembered it as hello's. The other four are left as
    // they are rather than fixed in passing; each is a diagnostic whose owner
    // has never mattered, and changing an app's identity moves its stored
    // files.
    {.id="local.player", .title="MUSIC PLAYER", .entry="apps/player/player.js",
     .runtime=APP_RUNTIME_LEGACY, .api=NULL,
     .required=CAPS_PLAY, .optional=CAPS_NONE, .works=APP_WORKS_NONE},

    // The Playground runs whatever the person typed, so it is the one identity
    // that may reach the whole library: the picker is the person choosing their
    // own work, on the screen they were already editing on.
    {.id="local.playground", .title="PLAYGROUND", .entry="src:0",
     .runtime=APP_RUNTIME_POCKET, .api=API_0_1,
     .required=CAPS_NONE, .optional=CAPS_WORK, .works=APP_WORKS_PICK},

    // A lesson copies a worked example into its own chapter slot and never
    // opens the person's library: section 3 asks in as many words that opening
    // a sample must not overwrite a user's work, and APP_WORKS_SELF is that
    // sentence with teeth. create() still works, so a chapter that teaches
    // saving has something to save into.
    {.id="local.tutorial", .title="TUTORIAL", .entry="src:1",
     .runtime=APP_RUNTIME_POCKET, .api=API_0_1,
     .required=CAPS_NONE, .optional=CAPS_WORK, .works=APP_WORKS_SELF},

    // The overlay of section 3.1. Its runtime is not one of the two above --
    // it is "pocket-overlay" in the document -- but nothing in this file reads
    // the runtime except the API range check, and an overlay is a pocket-API
    // app in every way that check cares about. Both capabilities are REQUIRED
    // rather than optional on purpose: a clock with no clock and an overlay
    // with nowhere to draw are not degraded, they are pointless, and 3.1 asks
    // for that to be said before the guest is built rather than after.
    {.id="local.deskclock", .title="DESK CLOCK", .entry="apps/deskclock/deskclock.js",
     .runtime=APP_RUNTIME_POCKET, .api=API_0_1,
     .required=CAPS_OVERLAY, .optional=CAPS_NONE, .works=APP_WORKS_NONE},

    // A work started by pocket.workspace.run(). Its own identity, so its
    // storage is its own and the Playground's is out of reach.
    {.id=APP_ID_WORK, .title="WORK", .entry="src:work",
     .runtime=APP_RUNTIME_POCKET, .api=API_0_1,
     .required=CAPS_NONE, .optional=CAPS_WORK, .works=APP_WORKS_PICK},
};

#define MANIFEST_N (sizeof(MANIFESTS)/sizeof(MANIFESTS[0]))

uint32_t app_registry_owner_hash(const char *id) {
    uint32_t hash=2166136261u;
    for(const char *p=id?id:"";*p;p++) { hash^=(unsigned char)*p; hash*=16777619u; }
    // 0 is reserved for "the host owns this", so an id that hashes there is
    // nudged rather than allowed to claim it.
    return hash?hash:1u;
}

const app_manifest_t *app_registry_find(const char *id) {
    if(!id || !id[0]) return NULL;
    for(size_t i=0;i<MANIFEST_N;i++)
        if(!strcmp(MANIFESTS[i].id,id)) return &MANIFESTS[i];
    return NULL;
}

// Four bytes of .data, and they are the session's identity.
static const app_manifest_t *current = &MANIFESTS[0];

void app_registry_select(const char *id) {
    const app_manifest_t *m=app_registry_find(id);
    if(!m) m=app_registry_find(APP_ID_DEFAULT);
    current=m?m:&MANIFESTS[0];
}

const app_manifest_t *app_registry_current(void) { return current; }

// ------------------------------------------------------------- version ranges

// Parses a dotted triple, stopping at `end`. Returns false on anything that is
// not exactly three decimal fields; a two-field version is refused rather than
// read as x.y.0, because guessing the missing field is how "0.2" comes to
// satisfy "<0.2.0".
static bool parse_version(const char *s, const char *end, unsigned out[3]) {
    for(int field=0;field<3;field++) {
        if(s>=end || *s<'0' || *s>'9') return false;
        unsigned value=0;
        while(s<end && *s>='0' && *s<='9') {
            value=value*10+(unsigned)(*s-'0');
            if(value>99999) return false;
            s++;
        }
        out[field]=value;
        if(field<2) { if(s>=end || *s!='.') return false; s++; }
    }
    return s==end;
}

static int compare(const unsigned a[3], const unsigned b[3]) {
    for(int i=0;i<3;i++) if(a[i]!=b[i]) return a[i]<b[i]?-1:1;
    return 0;
}

bool app_registry_api_ok(const char *range, const char *version) {
    if(!range || !range[0]) return true;
    unsigned have[3];
    if(!version || !parse_version(version,version+strlen(version),have)) return false;

    const char *p=range;
    while(*p) {
        while(*p==' '||*p=='\t'||*p==',') p++;
        if(!*p) break;
        // The operator, longest form first: ">=" must not be read as ">".
        int op;   // -2 <, -1 <=, 0 =, 1 >=, 2 >
        if(p[0]=='>'&&p[1]=='=')      { op=1;  p+=2; }
        else if(p[0]=='<'&&p[1]=='=') { op=-1; p+=2; }
        else if(p[0]=='>')            { op=2;  p+=1; }
        else if(p[0]=='<')            { op=-2; p+=1; }
        else if(p[0]=='='&&p[1]=='=') { op=0;  p+=2; }
        else if(p[0]=='=')            { op=0;  p+=1; }
        else return false;            // a bare version is not this grammar
        const char *end=p;
        while(*end && *end!=' ' && *end!='\t' && *end!=',') end++;
        unsigned want[3];
        if(!parse_version(p,end,want)) return false;
        int c=compare(have,want);
        bool ok = op==1?c>=0 : op==2?c>0 : op==-1?c<=0 : op==-2?c<0 : c==0;
        if(!ok) return false;
        p=end;
    }
    return true;
}

bool app_registry_wants(const app_manifest_t *manifest, const char *capability) {
    if(!manifest || !capability || !capability[0]) return false;
    for(int which=0;which<2;which++) {
        const char *const *names=which?manifest->optional:manifest->required;
        if(!names) continue;
        for(;*names;names++) if(!strcmp(*names,capability)) return true;
    }
    return false;
}

// ----------------------------------------------------------------- admission

bool app_registry_admit(const app_manifest_t *manifest, const char *api_version,
                        bool (*supported)(const char *name, void *user), void *user,
                        char *reason, size_t reason_size) {
    if(reason && reason_size) reason[0]=0;
    if(!manifest) return true;   // nothing registered is nothing to refuse

    // Only the new runtime is held to a version range; see the header.
    if(manifest->runtime==APP_RUNTIME_POCKET &&
       !app_registry_api_ok(manifest->api,api_version)) {
        if(reason && reason_size)
            snprintf(reason,reason_size,"NEEDS API %s",manifest->api?manifest->api:"?");
        return false;
    }
    if(!supported || !manifest->required) return true;
    for(const char *const *name=manifest->required;*name;name++) {
        if(supported(*name,user)) continue;
        if(reason && reason_size) snprintf(reason,reason_size,"NEEDS %s",*name);
        return false;
    }
    // The optional list is not checked on purpose. Section 2 has an app
    // feature-test what it can live without, and a host that refused to start
    // over an optional name would have made it required.
    return true;
}

// ---------------------------------------------------------------- permissions

bool app_registry_may_work(app_works_t level, app_work_op_t op,
                           bool owned, bool picked) {
    if(level==APP_WORKS_NONE) return false;
    switch(op) {
        case APP_WORK_PICK:   return level>=APP_WORKS_PICK;
        case APP_WORK_CREATE: return true;   // into its own works only
        case APP_WORK_READ:
        case APP_WORK_SAVE:
        case APP_WORK_RUN:
            // Its own work always. Anything else only because the person chose
            // it here, in this session, on a host screen — which is section 3's
            // "対象・目的を示すホスト画面" and section 7's "明示的な選択".
            return owned || (picked && level>=APP_WORKS_PICK);
    }
    return false;
}

// --------------------------------------------------------------------- titles

size_t app_registry_clean_title(const char *in, size_t length,
                                char *out, size_t out_size) {
    if(!in || !out || out_size==0) return 0;
    out[0]=0;
    if(length==0 || length>=out_size) return 0;
    if(!utf8_valid(in,length)) return 0;
    for(size_t i=0;i<length;i++) {
        unsigned char c=(unsigned char)in[i];
        if(c<0x20 || c==0x7f) return 0;
    }
    memcpy(out,in,length);
    out[length]=0;
    return length;
}
