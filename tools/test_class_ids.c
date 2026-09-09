// Class ids across several sessions, against the real quickjs-ng.
//
// WHAT THIS IS FOR. Every pocket surface keeps its JSClassID in a static, and
// for a long time that was fine because every session installed the same
// surfaces in the same order. Overlay sessions broke that: eight surfaces where
// a foreground app installs sixteen, so which class is built FIRST now varies
// between sessions.
//
// The static survives a session. The allocator does not:
//
//     JSClassID JS_NewClassID(JSRuntime *rt, JSClassID *pclass_id) {
//         if (*pclass_id == 0) *pclass_id = rt->js_class_id_alloc++;
//         return *pclass_id;
//     }
//
// The counter lives in the runtime and starts again with each one, while the
// static keeps whatever number an earlier session gave it. So two classes can
// end up holding the SAME id -- each allocated in a different runtime whose
// counter happened to be at the same value -- and the first session that builds
// both of them has the second one refused:
//
//     if (class_id < rt->class_count && rt->class_array[class_id].class_id)
//         return -1;
//
// On the board that surfaced as "pocket.fs could not be built: ESP_FAIL", with
// file_class=65, on a guest using 97,952 bytes of a 163,840-byte ceiling. The
// guest was told "no memory to build this namespace", which is what the lazy
// wrapper calls every failure, and two wrong diagnoses came out of that message
// before the log printed the id.
//
// The board has room for one runtime at a time, so nothing on it can walk four
// in a row. This can, and it is the cheap version of the thing that took three
// reproductions to find.
//
//   bash tools/build_class_ids.sh && /tmp/test-class-ids
//
#include "quickjs.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond,...) do { if(!(cond)) { \
    printf("FAIL %s:%d ",__FILE__,__LINE__); printf(__VA_ARGS__); printf("\n"); \
    failures++; } } while(0)

// ---------------------------------------------------------------- the surfaces
//
// One per class the firmware registers, with the same shape: a static id, a
// static owning runtime, and a build that runs when a session first touches it.
typedef struct {
    const char *name;
    JSClassID   id;
    JSRuntime  *owner;
    JSClassDef  def;
} surface_t;

static surface_t SURFACES[] = {
    {"api.hub",       0, NULL, {.class_name="PocketHub"}},
    {"api.token",     0, NULL, {.class_name="PocketCancel"}},
    {"storage.hub",   0, NULL, {.class_name="StorageHub"}},
    {"fs.file",       0, NULL, {.class_name="PocketFile"}},
    {"ui.screen",     0, NULL, {.class_name="Screen"}},
    {"ui.node",       0, NULL, {.class_name="Node"}},
    {"ui.text",       0, NULL, {.class_name="Text"}},
    {"ui.list",       0, NULL, {.class_name="List"}},
    {"text.session",  0, NULL, {.class_name="TextSession"}},
    {"net.lease",     0, NULL, {.class_name="NetLease"}},
    {"net.response",  0, NULL, {.class_name="NetResponse"}},
    {"workspace.ref", 0, NULL, {.class_name="WorkRef"}},
};
#define SURFACE_N ((int)(sizeof(SURFACES)/sizeof(SURFACES[0])))

// main/pocket/pocket_api.h, copied rather than included: this file must build
// with nothing but quickjs on the include path, and the whole point is that the
// rule is three lines. If it changes there and not here, the next case below
// starts failing, which is the intended way to notice.
static int class_ready(JSRuntime *rt, JSRuntime **owner, JSClassID *id) {
    if(*owner==rt) return 1;
    *id=0;
    *owner=rt;
    return 0;
}

// What build_fs() and its siblings do. Returns 0 on the refusal the board saw.
static int build(JSContext *ctx, surface_t *s, int fixed) {
    JSRuntime *rt=JS_GetRuntime(ctx);
    if(fixed) {
        // The behaviour BEFORE the fix, kept so that the test can show the bug
        // is real rather than only that the fix compiles. A test that only
        // exercises the mended path cannot tell mending from luck.
        if(!class_ready(rt,&s->owner,&s->id)) {
            JS_NewClassID(rt,&s->id);
            if(JS_NewClass(rt,s->id,&s->def)<0) return 0;
        }
        return 1;
    }
    JS_NewClassID(rt,&s->id);
    return JS_NewClass(rt,s->id,&s->def)>=0;
}

static void reset_surfaces(void) {
    for(int i=0;i<SURFACE_N;i++) { SURFACES[i].id=0; SURFACES[i].owner=NULL; }
}

// One session: a runtime, a context, and the surfaces this session touches.
// `touch` is a bitmask, which is how a session's surface set is expressed --
// an overlay touches a handful, a foreground app touches most.
static int session(unsigned touch, int fixed, const char **who) {
    JSRuntime *rt=JS_NewRuntime();
    if(!rt) return -1;
    JSContext *ctx=JS_NewContext(rt);
    if(!ctx) { JS_FreeRuntime(rt); return -1; }
    int bad=-1;
    for(int i=0;i<SURFACE_N;i++) {
        if(!(touch&(1u<<i))) continue;
        if(!build(ctx,&SURFACES[i],fixed)) { bad=i; if(who) *who=SURFACES[i].name; break; }
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return bad;
}

// The two sets that differ on this device, by the numbers in app_session.c.
#define OVERLAY_SET  0x00fu   // api.hub, api.token, storage.hub, fs.file
#define FOREGROUND   0xfffu   // everything

int main(void) {
    // ---- the bug is real -----------------------------------------------------
    //
    // Without the fix. THE NARROW SESSION HAS TO COME FIRST, and finding that
    // out is half of what this file is worth: if the wide session runs first it
    // fills every static, nothing is ever built for the first time later, and
    // the collision cannot happen. That is exactly why the board only failed
    // sometimes -- it depended on which shape of session touched a class first
    // after a boot, which is not something anybody was choosing.
    //
    // Overlay first: four classes take the counter's first four numbers. Then a
    // foreground session, where those four keep their old ids and the other
    // eight allocate from a counter that has started again -- straight onto the
    // ids the first four are already registered at.
    //
    // If this ever stops failing, the model here has drifted from quickjs and
    // the cases below are proving nothing.
    reset_surfaces();
    const char *who=NULL;
    int broke=-1;
    for(int i=0;i<6 && broke<0;i++) {
        unsigned set = (i%2) ? FOREGROUND : OVERLAY_SET;
        broke=session(set,0,&who);
    }
    CHECK(broke>=0,"the unfixed path never collided; the model has drifted");
    if(broke>=0) printf("  (unfixed: %s refused, as on the board)\n",who);

    // ---- four runtimes, alternating shapes ----------------------------------
    reset_surfaces();
    for(int i=0;i<4;i++) {
        unsigned set = (i%2) ? FOREGROUND : OVERLAY_SET;
        int bad=session(set,1,&who);
        CHECK(bad<0,"session %d (%s) refused %s",i,
              set==OVERLAY_SET?"overlay":"foreground",bad<0?"":who);
    }

    // ---- and a longer, less tidy run ---------------------------------------
    //
    // Sixteen sessions whose surface sets vary, including ones that touch a
    // single class and ones that touch none. The point is not the number; it is
    // that no ORDER of first-builds is special.
    reset_surfaces();
    static const unsigned SETS[]={
        OVERLAY_SET, 0x008u, FOREGROUND, 0x000u, 0x0f0u, OVERLAY_SET,
        0xf00u, FOREGROUND, 0x001u, 0x00au, OVERLAY_SET, 0x555u,
        0xaaau, FOREGROUND, 0x008u, OVERLAY_SET,
    };
    for(int i=0;i<(int)(sizeof(SETS)/sizeof(SETS[0]));i++) {
        int bad=session(SETS[i],1,&who);
        CHECK(bad<0,"session %d (set 0x%03x) refused %s",i,SETS[i],
              bad<0?"":who);
    }

    // ---- the retry, which is the other half --------------------------------
    //
    // pocket_api_lazy() leaves the accessor in place when a build fails, so a
    // read a second later tries again. Registering twice in one runtime is
    // exactly what JS_NewClass refuses, so without the owner check the retry
    // could only ever fail -- turning one transient failure into a permanent
    // one. That was the shape on the board: Left and Up both dead until the
    // session was restarted.
    reset_surfaces();
    {
        JSRuntime *rt=JS_NewRuntime();
        JSContext *ctx=JS_NewContext(rt);
        CHECK(build(ctx,&SURFACES[3],1),"first build");
        for(int i=0;i<5;i++)
            CHECK(build(ctx,&SURFACES[3],1),"retry %d must succeed",i);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    // ---- a runtime at a recycled address ------------------------------------
    //
    // The owner check compares pointers, and the allocator is free to hand the
    // next session a runtime at the same address as the last. If it does, the
    // check would skip a registration the new runtime has never seen -- so the
    // firmware clears the owner when the session ends, and this is that path.
    reset_surfaces();
    for(int i=0;i<4;i++) {
        JSRuntime *rt=JS_NewRuntime();
        JSContext *ctx=JS_NewContext(rt);
        CHECK(build(ctx,&SURFACES[3],1),"session %d after a free",i);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        // pocket_fs_reset(): the session ended, so the class belongs to nobody.
        SURFACES[3].owner=NULL;
    }

    printf(failures?"FAILURES %d\n":"ok\n",failures);
    return failures?1:0;
}
