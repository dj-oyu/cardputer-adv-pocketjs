#include "pocket_derby.h"
#include "pocket_api.h"
#include "pocket_shiki_frame.h"
#include "derby_ser_int.h"
#include "ui/kasane/ksn_shiki.h"
/* The PIE rotation and cull (derby_ser_pie.h): the kernels on the device
 * (CONFIG_POCKET_DERBY_SER_PIE, default y), their scalar reference on the host
 * harness (DERBY_SERPIE_HOST; bit for bit the kernels' outputs, piesim.py and
 * the device self-test). Neither (r5sim, PIE off): the scalar path only. */
#if (defined(__XTENSA__) && defined(CONFIG_POCKET_DERBY_SER_PIE)) || defined(DERBY_SERPIE_HOST)
#include "derby_ser_pie.h"
#define DERBY_PIE 1
#if defined(__XTENSA__) && defined(CONFIG_POCKET_DERBY_SER_PIE)
#define DSP_ROTATE dsp_rotate_pie
#define DSP_CULL dsp_cull_pie
#else
#define DSP_ROTATE dsp_rotate_ref
#define DSP_CULL dsp_cull_ref
#endif
#endif
#include <math.h>
// sin/cos are QuickJS's Math.sin/Math.cos (pocket_trig.h), so the C series
// still gives the JS's draw inputs bit for bit (docs/vm/qjs-observe.md sec.18).
#include "pocket_trig.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
// Counts this file's allocations when CONFIG_POCKET_SHIKI_MEMSTAT is on
// (docs/kasane/shiki.md sec.4.5); nothing otherwise.
#define SHIKI_MS_FILE SHIKI_MS_DERBY
#include "shiki_memstat.h"
#ifdef POCKET_DERBY_SER_PROBE
#ifdef ESP_PLATFORM
#include "esp_log.h"
#define PROBE_PRINT(...) ESP_LOGI("kasane",__VA_ARGS__)
#else
#include <stdio.h>
#define PROBE_PRINT(...) (printf(__VA_ARGS__),putchar('\n'))
#endif
#endif

/* DERBY WATCH's panning series (ser() in apps/derby/derby_pan.js as of vm/main
 * fac3552) translated to C, docs/apps/derby-ser-native.md. The JS is the
 * specification and this file must produce the very same draw() inputs, bit
 * for bit: every expression keeps the JS's operation order, in double (soft
 * float on the device, as QuickJS's numbers are), with QuickJS's own
 * Math.round/min/max and the libm floor/ceil/sqrt/sin/cos/fmod QuickJS calls.
 * The build keeps the compiler from fusing or reordering (-ffp-contract=off,
 * main/CMakeLists.txt). The host harness checks it against the JS on every
 * frame and on random cameras (tools/games/derby_ser_oracle.js).
 *
 * Why C: the series were 27.6 of the oval bend's 44.25 ms JS turn on the
 * device (docs/apps/derby-camera-history.md). The plans, the VM and the draw
 * path are untouched: each draw goes through pocket_proc_draw_numbers(), the
 * second half of H.draw().
 *
 * Since 2026-11-13 view() and ser() run in integers only (derby_ser_int.c,
 * docs/apps/derby-ser-division.md sec.4, sec.13), the draws going to the plans
 * already in their Q formats (pocket_proc_draw_fixed); the double series is
 * gone (the user's decision). Inputs past the integers' range are clamped
 * into it (L >= 1, s 0.25..32 m, |w| <= 125 m, |g0| <= 10,000 m, the camera
 * within +-6,000 m, f 32..4,095 px, zf 0..20,000 m): the app's never are, and
 * a clamped call only draws another picture. On the oval, view() rotates the
 * chord ends and each ser() culls the chords with the PIE kernels
 * (derby_ser_pie.h) or, after the up key (mode(1)), with derby_ser_int.c's
 * scalar ones: the same binary's A/B. The JS stays the geometry's reference,
 * within a tolerance (tools/games/derby_ser_oracle.js). aim() and the side
 * view still work in doubles. */

#define DERBY_ENDS_MAX 32u
#define DERBY_SECTIONS_MAX 4u
#define SECTION_TERMS 7u    /* length, curvature, x, z, tangent x, z, heading */

/* The PIE kernels' lanes and rotation constants, 16-byte aligned inside the
 * course block (malloc gives less). */
#ifdef DERBY_PIE
typedef struct {dsp_ends E;dsp_rot_k rk;} derby_pie;
#endif
/* One heap block per course() (freed by course() again and reset()): the
 * chord ends (CH) and the sections (OC, none for the straight, whose pose()
 * is [g, w, 1, 0]). */
typedef struct {
    unsigned ends,sections;
    double dnr;
    double *ch,*sec;
#ifdef POCKET_DERBY_COURSE_CACHE
    double *pcache;     /* the ends' poses, made at registration (an experiment) */
#endif
    /* The integer series (derby_ser_int.c, docs/apps/derby-ser-division.md
     * sec.4; course() refuses ends and sections past +-10,000 m). The ends
     * as Q16 (qch), on the oval also their poses (qpos Q16, the unit tangent
     * qt14 Q14 and qt30 Q30, made once here instead of every view()), the
     * scalar path's lanes soa and wide ends, the PIE path's lanes (pie),
     * view()'s keep-mask qvc; the series' chord tables, made on first use (8
     * at most, the least recently used given up). */
    int32_t *qch,(*qpos)[2],(*qt30)[2];
    int16_t (*qt14)[2];
    derby_ends_soa soa;
#ifdef DERBY_PIE
    derby_pie *pie;
#endif
    uint32_t qvc,qwide;
    derby_chord_q *slot[DERBY_Q_SLOTS];
    int32_t key[DERBY_Q_SLOTS][3];
    uint32_t used[DERBY_Q_SLOTS],clock;
    /* The frame's camera for the integers (view()), and the plans checked
     * for the integers' Q formats (plan_q). Here, not static: DIRAM's .bss
     * is every app's, the course block only DERBY's (CLAUDE.md). */
    derby_cam_q camq;
    struct {uint32_t h;const uint8_t *q;} plan_ok[6];   /* prail, t0, t1, hl, crowd */
    unsigned plan_ok_next;
} derby_course;
static derby_course *course;
#if defined(POCKET_DERBY_COURSE_CACHE) || defined(POCKET_DERBY_PROJECT_BATCH)
static DerbyProjectionCounts projection_counts;
void pocket_derby_projection_counts(DerbyProjectionCounts *out){if(out)*out=projection_counts;}
#endif
#ifdef POCKET_DERBY_PROJECT_BATCH
static uint64_t course_generation,view_generation;
#endif
/* view(): the camera pc = [x, z, unit x, unit z, f, distance], the far depth,
 * the scene frame t, the stands' plans and their two constants. fr is
 * sqrt(f*f+25600), ou()'s margin scale: aim() computes it once, by the same
 * expression, instead of every candidate chord (docs/apps/derby-ser-cost.md
 * sec.4 A; bit-identical). */
static struct {
    double pc[6],fr,zf,t,hl_color,cells;
    uint32_t hl,crowd;
    bool set;
    bool pie;               /* this frame's rotation and cull: PIE (else scalar) */
    int32_t cells_q;        /* cells, Q16 */
} view;
/* The rotation and cull: 0 the PIE kernels (default), 1 derby_ser_int.c's
 * scalar ones (pocket.derby.mode(), the up key's A/B on the device). Taken at
 * view(). Without PIE (r5sim, CONFIG_POCKET_DERBY_SER_PIE off) both are the
 * scalar path. */
static int ser_mode=0;
#ifdef DERBY_SER_CHECK
/* Host checks only: the integer series' counters and invariants. */
static derby_ser_stats ser_stats;
void pocket_derby_ser_stats(derby_ser_stats *out){if(out)*out=ser_stats;}
#endif
/* The frame's camera for the integer calls (side(), aim()): a side unit
 * (all Q16: f px, height h m, horizon hy px, x0 m, the centre o and the right
 * bound R px, the left edge e px), or a panning unit (aim(): view.pc). */
static struct {
    int64_t f,h,hy,x0,o,R,e;
    bool K,aimed;
} cam;
/* The inputs clamped into the integers' range (the probe counts them). */
enum {CL_L,CL_S,CL_GW,CL_CAM,CL_N};
#ifdef POCKET_DERBY_SER_PROBE
/* The series' probe (CONFIG_POCKET_DERBY_SER_PROBE, default n; host:
 * -DPOCKET_DERBY_SER_PROBE): which path view() and ser() took (index 0 PIE,
 * 1 scalar), the inputs clamped into the integers' range, the tables that
 * could not be made, and the cycles (pocket_cycles), the Shiki draws inside
 * ser() apart. The rotation and the cull run on both paths every frame, the
 * other path as a shadow whose cycles are taken out of view()'s and ser()'s
 * (rot_cy, cull_cy: both paths on the same frames). One DERBY_SER line a
 * KASANE_PAINT window (pocket_derby_ser_probe_log). docs/apps/derby-ser-division.md
 * sec.12, sec.13. */
enum {FB_MALLOC,FB_TABLE,FB_N};
static struct {
    uint32_t views[2],calls[2],clamp[CL_N],fb[FB_N],tables;
    uint32_t view_cy[2],ser_cy[2],draw_cy[2],draws[2],chords[2];
    uint32_t rot_cy[2],cull_cy[2],culls,cull_diff,wide[2],shadow_acc;
    /* The core's counters (PM0 retired instructions, PM1 instruction-fetch
     * stall cycles: the flash cache's misses show here), whole ser() and its
     * draws, by path (the shadow cull's included). */
    uint32_t insn[2],ist[2],draw_insn[2],draw_ist[2];
    uint32_t draw_acc,draw_n,draw_insn_acc,draw_ist_acc;
    bool in_ser;
} serp;
#ifdef ESP_PLATFORM
/* Armed on first use, as app_session.c's TURNPERF arms them (so not in a
 * build with CONFIG_POCKET_VM_TURNPERF; apptrace off): task level only. */
#include "eri.h"
#include "xtensa-debug-module.h"
#include "xtensa/xt_perf_consts.h"
static bool serp_pm_on;
static inline void serp_pm(uint32_t *insn,uint32_t *ist){
    if(!serp_pm_on){
        const uint16_t sels[2]={XTPERF_CNT_INSN,XTPERF_CNT_I_STALL},masks[2]={XTPERF_MASK_INSN_ALL,XTPERF_MASK_I_STALL_ALL};
        for(int id=0;id<2;id++){
            eri_write(ERI_PERFMON_PM0+id*4,0);
            eri_write(ERI_PERFMON_PMCTRL0+id*4,((uint32_t)(sels[id]&PMCTRL_SELECT_MASK)<<PMCTRL_SELECT_SHIFT)|
                                               ((uint32_t)(masks[id]&PMCTRL_MASK_MASK)<<PMCTRL_MASK_SHIFT));
        }
        eri_write(ERI_PERFMON_PGM,PGM_PMEN);
        serp_pm_on=true;
    }
    *insn=eri_read(ERI_PERFMON_PM0);*ist=eri_read(ERI_PERFMON_PM0+4);
}
#else
static inline void serp_pm(uint32_t *insn,uint32_t *ist){*insn=*ist=0;}
#endif
#define SERP(x) do{x;}while(0)
/* A draw inside ser(): its cycles and counts apart from the geometry's. */
#define SERP_DRAW(call) ({uint32_t _i,_s;serp_pm(&_i,&_s);const uint32_t _t=pocket_cycles();JSValue _r=(call); \
    if(serp.in_ser){uint32_t _i1,_s1;const uint32_t _t1=pocket_cycles();serp_pm(&_i1,&_s1); \
        serp.draw_acc+=_t1-_t;serp.draw_n++;serp.draw_insn_acc+=_i1-_i;serp.draw_ist_acc+=_s1-_s;}_r;})
#else
#define SERP(x) do{}while(0)
#define SERP_DRAW(call) (call)
#endif

/* ---- QuickJS's Math, as quickjs.c has it (js_math_round, js_fmin, js_fmax,
 * js_math_min_max): signed zeros and NaN behave as in the JS. */
typedef union { double d; uint64_t u; } f64;
static double js_round(double a){
    f64 u={.d=a};
    const unsigned e=(unsigned)(u.u>>52)&0x7ffu;
    if(e<1023u){
        if(e==1022u&&u.u!=0xbfe0000000000000ull)u.u=(u.u&(1ull<<63))|(1023ull<<52);
        else u.u&=1ull<<63;
    }else if(e<1023u+52u){
        const unsigned s=(unsigned)(u.u>>63);
        const uint64_t one=1ull<<(52u-(e-1023u));
        u.u+=(one>>1)-s;
        u.u&=~(one-1u);
    }
    return u.d;
}
static double js_min(double a,double b){
    if(isnan(a))return a;
    if(isnan(b))return b;
    if(a==0&&b==0){f64 x={.d=a},y={.d=b};x.u|=y.u;return x.d;}
    return a<b?a:b;
}
static double js_max(double a,double b){
    if(isnan(a))return a;
    if(isnan(b))return b;
    if(a==0&&b==0){f64 x={.d=a},y={.d=b};x.u&=y.u;return x.d;}
    return a<b?b:a;
}

/* pose(g, w) of derby_view.js: the point g m along the course, w m out. The
 * JS loop's bound 2 is its section count less one. */
#ifdef POCKET_DERBY_COURSE_CACHE
static void course_pose(const derby_course *path,double g,double w,double o[4]){
    if(!path->sections){o[0]=g;o[1]=w;o[2]=1;o[3]=0;return;}
    unsigned i=0;
    while(i<path->sections-1u&&g>path->sec[SECTION_TERMS*i])g-=path->sec[SECTION_TERMS*i++];
    const double *e=path->sec+SECTION_TERMS*i;
    double x=e[2],z=e[3],c=e[4],d=e[5];
    if(e[1]!=0){
        const double r=1/e[1]+path->dnr,C=pocket_cos(e[6]+e[1]*g),S=pocket_sin(e[6]+e[1]*g);
        x+=(S-d)*r;z-=(C-c)*r;c=C;d=S;g=0;
    }
    o[0]=x+g*c-w*d;o[1]=z+g*d+w*c;o[2]=c;o[3]=d;
}
#endif
static void pose(double g,double w,double o[4]){
    if(!course->sections){o[0]=g;o[1]=w;o[2]=1;o[3]=0;return;}
    unsigned i=0;
    while(i<course->sections-1u&&g>course->sec[SECTION_TERMS*i])g-=course->sec[SECTION_TERMS*i++];
    const double *e=course->sec+SECTION_TERMS*i;
    double x=e[2],z=e[3],c=e[4],d=e[5];
    if(e[1]!=0){
        const double r=1/e[1]+course->dnr,C=pocket_cos(e[6]+e[1]*g),S=pocket_sin(e[6]+e[1]*g);
        x+=(S-d)*r;z-=(C-c)*r;c=C;d=S;g=0;
    }
    o[0]=x+g*c-w*d;o[1]=z+g*d+w*c;o[2]=c;o[3]=d;
}
/* The same on a course not yet published (course()) and for the integer
 * tables' callback (derby_ser_table). */
static void pose_of(void *user,double g,double w,double o[4]){
    derby_course *const saved=course;
    course=user;pose(g,w,o);course=saved;
}
static JSValue fail(JSContext *ctx,const char *op,const char *code,const char *message){
    return pocket_api_throw(ctx,code,op,message,false,POCKET_OUTCOME_NOT_APPLIED);
}
static bool number(JSContext *ctx,JSValueConst v,double *out){
    return JS_IsNumber(v)&&JS_ToFloat64(ctx,out,v)==0;
}
/* A plan handle as dr() reads live[n]: falsy draws nothing (0), else it must
 * be a handle-shaped integer (pocket_proc_draw_numbers looks it up). */
static bool plan_handle(JSContext *ctx,JSValueConst v,uint32_t *out){
    double n;
    *out=0;
    if(!JS_ToBool(ctx,v))return true;
    if(!number(ctx,v,&n)||n<1||n>INT32_MAX||n!=floor(n))return false;
    *out=(uint32_t)n;return true;
}
#ifdef POCKET_DERBY_PROJECT_BATCH
static bool element(JSContext *ctx,JSValueConst array,uint32_t i,double *out){
    JSValue v=JS_GetPropertyUint32(ctx,array,i);
    const bool ok=!JS_IsException(v)&&number(ctx,v,out);
    JS_FreeValue(ctx,v);
    return ok;
}
#endif
/* The integer interface (docs/apps/derby-integer.md): lengths, positions and
 * focal lengths cross as Q16 int32 (metres or px), counts and colours as plain
 * int32. A number that is not an int32 is refused, so the JS never hands this
 * surface a double. */
#define Q16 65536
static bool in32(JSValueConst v,int32_t *out){
    if(JS_VALUE_GET_TAG(v)!=JS_TAG_INT)return false;
    *out=JS_VALUE_GET_INT(v);return true;
}
static bool qn(JSValueConst v,double *out){
    int32_t i;
    if(!in32(v,&i))return false;
    *out=i/65536.0;return true;
}
static bool qelement(JSContext *ctx,JSValueConst array,uint32_t i,double *out){
    JSValue v=JS_GetPropertyUint32(ctx,array,i);
    const bool ok=!JS_IsException(v)&&qn(v,out);
    JS_FreeValue(ctx,v);
    return ok;
}
/* An Int32Array of at least n elements, or NULL. */
static int32_t *i32array(JSContext *ctx,JSValueConst v,size_t n){
    if(JS_GetTypedArrayType(v)!=JS_TYPED_ARRAY_INT32)return NULL;
    size_t offset=0,bytes=0,per=0,size=0;
    JSValue buffer=JS_GetTypedArrayBuffer(ctx,v,&offset,&bytes,&per);
    if(JS_IsException(buffer)){JS_FreeValue(ctx,JS_GetException(ctx));return NULL;}
    uint8_t *data=JS_GetArrayBuffer(ctx,&size,buffer);
    JS_FreeValue(ctx,buffer);
    if(!data||per!=4||bytes<n*4||offset>size||bytes>size-offset)return NULL;
    return (int32_t *)(void *)(data+offset);
}
static bool length_of(JSContext *ctx,JSValueConst array,uint32_t *out){
    double n;
    if(!JS_IsArray(array))return false;
    JSValue v=JS_GetPropertyStr(ctx,array,"length");
    const bool ok=!JS_IsException(v)&&number(ctx,v,&n)&&n>=0&&n<=UINT32_MAX;
    JS_FreeValue(ctx,v);
    if(ok)*out=(uint32_t)n;
    return ok;
}
static JSValue draw(JSContext *ctx,uint32_t handle,const double in[8]){
    return handle?pocket_proc_draw_numbers(ctx,handle,in,8):JS_UNDEFINED;
}

static void free_course(derby_course *c){
    if(!c)return;
    for(unsigned i=0;i<DERBY_Q_SLOTS;i++)free(c->slot[i]);
    free(c);
}
#define QRANGE 10000.0      /* m: course() refuses ends and section origins past it */
/* course(ends, sections, dnr): CH, OC (or falsy: the straight) and DNR. */
static JSValue js_course(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="derby.course";
    uint32_t ends=0,sections=0;
    double dnr;
    if(argc<3||!length_of(ctx,argv[0],&ends)||ends<2||
       (JS_ToBool(ctx,argv[1])&&(!length_of(ctx,argv[1],&sections)||!sections))||
       !qn(argv[2],&dnr))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected chord ends, sections or 0, and a Q16 depth");
    if(ends>DERBY_ENDS_MAX||sections>DERBY_SECTIONS_MAX)
        return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"32 chord ends and 4 sections maximum");
    /* Read into a new block first: a bad argument leaves the old course. */
    const size_t doubles=ends+(size_t)SECTION_TERMS*sections;
#ifdef POCKET_DERBY_COURSE_CACHE
    const size_t cache_doubles=sections?4u*ends:0;
#else
    const size_t cache_doubles=0;
#endif
    /* The integer series' part (4-byte aligned): the ends as Q16, and on the
     * oval their poses, the scalar path's lanes and the PIE path's (16-byte
     * aligned by hand). */
#ifdef DERBY_PIE
    const size_t pie_bytes=sizeof(derby_pie)+15u;
#else
    const size_t pie_bytes=0;
#endif
    const size_t qoff=(doubles+cache_doubles)*sizeof(double),
        qbytes=4u*ends+(sections?20u*ends+derby_ends_soa_bytes(ends)+pie_bytes:0u);
    derby_course *c=malloc(sizeof *c+qoff+qbytes);
    if(!c)return fail(ctx,op,POCKET_ERR_OUT_OF_MEMORY,"course allocation failed");
    memset(c,0,sizeof *c);
    c->ends=ends;c->sections=sections;c->dnr=dnr;
    c->qch=(int32_t *)(void *)((uint8_t *)(c+1)+qoff);
    if(sections){
        c->qpos=(int32_t (*)[2])(c->qch+ends);c->qt30=c->qpos+ends;c->qt14=(int16_t (*)[2])(c->qt30+ends);
        derby_ends_soa_init(&c->soa,c->qt14+ends,ends);
#ifdef DERBY_PIE
        c->pie=(derby_pie *)(((uintptr_t)(c->qt14+ends)+derby_ends_soa_bytes(ends)+15u)&~(uintptr_t)15u);
#endif
    }
    c->ch=(double *)(c+1);c->sec=c->ch+ends;
#ifdef POCKET_DERBY_COURSE_CACHE
    c->pcache=c->sec+SECTION_TERMS*sections;
#endif
    bool ok=true;
    for(uint32_t i=0;i<ends&&ok;i++)ok=qelement(ctx,argv[0],i,&c->ch[i]);
    for(uint32_t i=0;i<sections&&ok;i++){
        JSValue row=JS_GetPropertyUint32(ctx,argv[1],i);
        uint32_t n=0;
        double *e=c->sec+SECTION_TERMS*i;
        ok=!JS_IsException(row)&&length_of(ctx,row,&n)&&(n==6||n==SECTION_TERMS);
        e[6]=0;
        for(uint32_t k=0;k<n&&ok;k++)ok=qelement(ctx,row,k,&e[k]);
        /* A bend reads its heading (pose()). The JS gives its radius (0: a
         * straight) and the heading in turns; pose() takes the curvature and
         * radians, as derby_view.js had them. */
        if(ok&&e[1]!=0&&n!=SECTION_TERMS)ok=false;
        if(ok&&e[1]!=0){e[1]=1/e[1];e[6]*=2*M_PI;}
        JS_FreeValue(ctx,row);
    }
    if(!ok){
        free(c);
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"chord ends and section terms must be Q16 integers");
    }
    /* The integer series' range (docs/apps/derby-ser-division.md sec.4.4):
     * every end and section origin within +-10,000 m (the app's are). There
     * is no other path for a course past it. */
    for(unsigned i=0;i<ends&&ok;i++){ok=fabs(c->ch[i])<=QRANGE;c->qch[i]=(int32_t)(c->ch[i]*65536);}
    for(unsigned i=0;i<sections&&ok;i++)ok=fabs(c->sec[SECTION_TERMS*i+2])<=QRANGE&&fabs(c->sec[SECTION_TERMS*i+3])<=QRANGE;
    if(!ok){
        free(c);
        return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"chord ends and section origins within +-10,000 m");
    }
    if(sections){
#ifdef DERBY_PIE
        int32_t nx[DERBY_ENDS_MAX],nz[DERBY_ENDS_MAX];
#endif
        for(unsigned i=0;i<ends;i++){
            double m[4];
            pose_of(c,c->ch[i],0,m);
            c->qpos[i][0]=(int32_t)floor(m[0]*65536+.5);c->qpos[i][1]=(int32_t)floor(m[1]*65536+.5);
            c->qt30[i][0]=(int32_t)floor(m[2]*1073741824.0+.5);c->qt30[i][1]=(int32_t)floor(m[3]*1073741824.0+.5);
            c->qt14[i][0]=(int16_t)floor(m[2]*16384+.5);c->qt14[i][1]=(int16_t)floor(m[3]*16384+.5);
#ifdef DERBY_PIE
            nx[i]=-c->qt30[i][1];nz[i]=c->qt30[i][0];     /* the normal (-tz, tx) */
#endif
        }
#ifdef DERBY_PIE
        dsp_set_course(&c->pie->E,ends,nx,nz);
#endif
    }
#ifdef POCKET_DERBY_COURSE_CACHE
    /* Precompute against the validated new registration before publication. */
    if(sections)for(unsigned i=0;i<ends;i++)course_pose(c,c->ch[i],0,c->pcache+4u*i);
#endif
    free_course(course);
    course=c;
#ifdef POCKET_DERBY_PROJECT_BATCH
    ++course_generation;
#endif
#if defined(POCKET_DERBY_COURSE_CACHE) || defined(POCKET_DERBY_PROJECT_BATCH)
    ++projection_counts.course_replacements;
#endif
#ifdef POCKET_DERBY_COURSE_CACHE
    projection_counts.cache_bytes=(uint32_t)(sections?4u*ends*sizeof(double):0);
    if(sections)projection_counts.registration_poses+=ends;
#endif
    view.set=false;
    return JS_UNDEFINED;
}
/* v within lo..hi (NaN: lo). */
static double clampd(double v,double lo,double hi){return v>=lo?(v<=hi?v:hi):lo;}
/* camq from aim()'s pc, view()'s zf and fr, clamped into the integer series'
 * range (sec.4.4: the camera within +-6,000 m, f 32..4,095 px, zf 0..20,000
 * m; the app's cameras are inside it). A direction that is not a unit (aim()
 * at its own point: 0/0) looks down x. */
static void cam_q(void){
    const double *pc=view.pc;
    const double x=clampd(pc[0],-6000,6000),z=clampd(pc[1],-6000,6000),f=clampd(pc[4],32,4095-1/65536.0),
        zf=clampd(view.zf,0,20000),fr=f==pc[4]?view.fr:sqrt(f*f+25600);
    double dx=pc[2],dz=pc[3];
    if(!(fabs(dx)<=1&&fabs(dz)<=1)){dx=1;dz=0;}
    SERP(if(x!=pc[0]||z!=pc[1]||f!=pc[4]||zf!=view.zf||dx!=pc[2]||dz!=pc[3])serp.clamp[CL_CAM]++);
    derby_cam_q *const q=&course->camq;
    q->ex=(int32_t)floor(x*65536+.5);q->ez=(int32_t)floor(z*65536+.5);
    q->dx=(int32_t)floor(dx*1073741824.0+.5);q->dz=(int32_t)floor(dz*1073741824.0+.5);
    q->cx=(int16_t)floor(dx*16384+.5);q->cz=(int16_t)floor(dz*16384+.5);
    q->f=(int32_t)floor(f*65536+.5);q->invf=(int32_t)floor(4294967296.0/f+.5);
    q->zf=(int32_t)floor(zf*65536+.5);q->zn=q->f/50;
    q->fr3=(int32_t)ceil(fr*8);q->fr16=(int32_t)ceil(fr*65536);
}
/* The frame's rotation of the oval's chord ends on one path: the PIE lanes
 * (derby_ser_pie.h; ends past +-2,047 m leave E.valid) or the scalar ones
 * (derby_ser_int.h; ends past +-2,750 m are qwide). */
static void rotate(bool pie){
    derby_course *const c=course;
#ifdef DERBY_PIE
    if(pie){
        dsp_set_camera(&c->pie->E,&c->pie->rk,&c->qpos[0][0],&c->qpos[0][1],2,c->camq.ex,c->camq.ez,c->camq.dx,c->camq.dz);
        DSP_ROTATE(&c->pie->E,&c->pie->rk);
        return;
    }
#else
    (void)pie;
#endif
    c->qwide=derby_ser_prepare(&c->soa,&c->camq,(const int32_t (*)[2])c->qpos,(const int16_t (*)[2])c->qt14,c->ends);
    derby_ser_rotate_ref(&c->soa,c->camq.cx,c->camq.cz);
}
/* The chords of the oval not wholly out by m (Q16 m) at offset w (Q16 m),
 * the integer ou() on one path: the lanes' cull (PIE: margin m + 1 m, scalar:
 * m + 3 m), the chords past the lanes' range by derby_ser_cull_wide (int64,
 * Q16). Conservative either way: every chord the double ou() keeps is kept
 * (the two keep different extra chords, which lim() then drops). */
static uint32_t cull(bool pie,int32_t w,int32_t m){
    derby_course *const c=course;
    const unsigned chords=c->ends-1u;
    const uint32_t all=chords>=32u?~0u:(1u<<chords)-1u;
    uint32_t keep,wc;
#ifdef DERBY_PIE
    if(pie){
        dsp_cull_k k;
        if(!dsp_cull_consts(&k,c->camq.f,c->camq.fr16,c->camq.zf,w,m))return all;
        keep=c->pie->E.valid&~DSP_CULL(&c->pie->E,&k);
        wc=all&~c->pie->E.valid;
    }else
#else
    (void)pie;
#endif
    {
        derby_cull_q p;
        if(!derby_ser_cull_params(&c->camq,w,m,&p))return all;
        wc=(c->qwide|c->qwide>>1)&all;
        keep=derby_ser_cull_ref(&c->soa,chords,&p)&~wc;
    }
    SERP(serp.wide[pie?0:1]+=(uint32_t)__builtin_popcount(wc));
    for(uint32_t b=wc;b;b&=b-1u){
        const unsigned j=(unsigned)__builtin_ctz(b);
        if(!derby_ser_cull_wide(&c->camq,c->qpos[j],c->qpos[j+1],c->qt30[j],c->qt30[j+1],w,m))keep|=1u<<j;
    }
    return keep;
}
/* cull() on this frame's path; a probe build also runs the other path (a
 * shadow: both paths' cycles on the same frames, its own taken out of the
 * caller's) and counts the chords the two keep differently. */
static uint32_t keep_mask(int32_t w,int32_t m){
#if defined(POCKET_DERBY_SER_PROBE) && defined(DERBY_PIE)
    const int k=view.pie?0:1;
    const uint32_t t0=pocket_cycles(),a=cull(view.pie,w,m),t1=pocket_cycles(),b=cull(!view.pie,w,m),t2=pocket_cycles();
    serp.cull_cy[k]+=t1-t0;serp.cull_cy[k^1]+=t2-t1;serp.shadow_acc+=t2-t1;
    serp.culls++;serp.cull_diff+=(uint32_t)__builtin_popcount(a^b);
    return a;
#elif defined(POCKET_DERBY_SER_PROBE)
    const uint32_t t0=pocket_cycles(),a=cull(false,w,m);
    serp.cull_cy[1]+=pocket_cycles()-t0;serp.culls++;
    return a;
#else
    return cull(view.pie,w,m);
#endif
}
/* view(pc, zf, t, hl, crowd, hlColor, cells): the frame's camera. On the oval
 * it rotates the chord ends and keeps the chords that may be in view (qvc). */
static JSValue js_view(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
#ifdef POCKET_DERBY_PROJECT_BATCH
    ++view_generation;
#endif
    const char *op="derby.view";
    if(!course)return fail(ctx,op,POCKET_ERR_BUSY,"course required");
    /* view(zfExtra, t, hl, crowd, hlColor, cells): the camera is aim()'s;
     * zf is its distance to the aim plus zfExtra. */
    double extra;
    int32_t t,hc;
    bool ok=argc>=6&&cam.aimed&&qn(argv[0],&extra)&&in32(argv[1],&t)&&
       plan_handle(ctx,argv[2],&view.hl)&&plan_handle(ctx,argv[3],&view.crowd)&&
       in32(argv[4],&hc)&&in32(argv[5],&view.cells_q)&&qn(argv[5],&view.cells);
    if(ok){view.zf=view.pc[5]+extra;view.t=t;view.hl_color=hc;}
    /* JS getters may have replaced the course while pc was read. */
    if(!ok||!course){
        view.set=false;
        return fail(ctx,op,!ok?POCKET_ERR_INVALID_ARGUMENT:POCKET_ERR_BUSY,
                    !ok?"expected aim(), Q16 depth, frame, two plans, colour and Q16 cells":"course required");
    }
    cam_q();
#ifdef DERBY_PIE
    view.pie=!ser_mode;
#else
    view.pie=false;
#endif
    if(course->sections){
#if defined(POCKET_DERBY_SER_PROBE) && defined(DERBY_PIE)
        const int k=view.pie?0:1;
        const uint32_t t0=pocket_cycles();
        rotate(view.pie);
        const uint32_t t1=pocket_cycles();
        rotate(!view.pie);
        const uint32_t t2=pocket_cycles();
        serp.rot_cy[k]+=t1-t0;serp.rot_cy[k^1]+=t2-t1;serp.shadow_acc+=t2-t1;
#elif defined(POCKET_DERBY_SER_PROBE)
        const uint32_t t0=pocket_cycles();
        rotate(false);
        serp.rot_cy[1]+=pocket_cycles()-t0;
#else
        rotate(view.pie);
#endif
        course->qvc=keep_mask(1671168,30*Q16);      /* ou(j, 25.5, 30, zf) */
    }else course->qvc=1;
    view.set=true;
    return JS_UNDEFINED;
}
#ifdef POCKET_DERBY_PROJECT_BATCH
/* project(progress[], depths[], subtract, Float64Array output): four doubles
 * per point [numerator, Z, screen-x, reciprocal-Z]. Uses exactly pose/pj's
 * operation order, then 1/Z and numerator*(1/Z). The caller keeps visibility,
 * sort and drawing. Arrays are read completely before borrowing output bytes:
 * user getters may replace course/view or detach buffers. No JS executes
 * after the validation/generation check. No guest result objects are allocated. */
static JSValue js_project(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="derby.project";
    if(!course||!view.set)return fail(ctx,op,POCKET_ERR_BUSY,"view required");
    const uint64_t cg=course_generation,vg=view_generation;
    uint32_t n=0,m=0;double subtract,g[32],w[32];
    bool ok=argc==4&&length_of(ctx,argv[0],&n)&&length_of(ctx,argv[1],&m)&&
            n==m&&n>0&&n<=32&&number(ctx,argv[2],&subtract)&&isfinite(subtract);
    for(unsigned i=0;i<n&&ok;i++)
        ok=element(ctx,argv[0],i,&g[i])&&isfinite(g[i])&&element(ctx,argv[1],i,&w[i])&&isfinite(w[i]);
    if(!ok)return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"1..32 finite progress/depth pairs and subtract required");
    if(!course||!view.set||cg!=course_generation||vg!=view_generation)
        return fail(ctx,op,POCKET_ERR_BUSY,"course or view changed while reading input");
    if(JS_GetTypedArrayType(argv[3])!=JS_TYPED_ARRAY_FLOAT64)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"Float64Array output required");
    size_t offset=0,bytes=0,per=0,size=0;
    JSValue buffer=JS_GetTypedArrayBuffer(ctx,argv[3],&offset,&bytes,&per);
    if(JS_IsException(buffer))return buffer;
    if(JS_IsImmutableArrayBuffer(buffer)>0){
        JS_FreeValue(ctx,buffer);
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"output buffer must be mutable");
    }
    uint8_t *data=JS_GetArrayBuffer(ctx,&size,buffer);
    if(!data||per!=sizeof(double)||bytes<(size_t)n*4*sizeof(double)||offset>size||bytes>size-offset){
        JS_FreeValue(ctx,buffer);
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"output buffer is detached or too short");
    }
    const double *pc=view.pc;
    for(unsigned i=0;i<n;i++){
        double P[4];pose(g[i]-subtract,w[i],P);
        const double a=P[0]-pc[0],b=P[1]-pc[1],Z=(a*pc[2]+b*pc[3])/pc[4];
        const double numerator=a*pc[3]-b*pc[2]+120*Z,r=1/Z;
        const double result[4]={numerator,Z,numerator*r,r};
        memcpy(data+offset+(size_t)i*4*sizeof(double),result,sizeof result);
    }
    JS_FreeValue(ctx,buffer);
    ++projection_counts.project_calls;projection_counts.project_points+=n;
    projection_counts.input_numbers+=2*n+1;projection_counts.output_numbers+=4*n;
    return JS_UNDEFINED;
}
#endif
/* ser(h, w, s, g0, L, a, b): ser(n, w, s, g0, L, zf, a, b) of derby_pan.js,
 * zf from view(). b omitted: the turf (t0, t1). */
/* SP(k): r5sim only (-DR5SIM_DERBY_PROF=1), cycles since the last mark into
 * part k of a call (docs/apps/derby-ser-cost.md sec.2); SC(k) counts. The
 * running mark is global: derby_ser_int.c marks the integer chords too. */
#ifdef R5SIM_DERBY_PROF
uint32_t derby_ser_cy[9],derby_sp_t;
#define SP(k) do{const uint32_t _t=pocket_cycles();derby_ser_cy[k]+=_t-derby_sp_t;derby_sp_t=_t;}while(0)
#else
#define SP(k) do{}while(0)
#endif

/* ---- The series (derby_ser_int.c). A plan takes integer inputs only in the
 * Q formats derby_ser_chord_q writes (checked once a handle: handles are not
 * reused within a session, and reset() forgets them). */
static bool plan_q(uint32_t h,const uint8_t q[8]){
    if(!h)return true;
#ifdef POCKET_PROC_DRAW_HOOK
    if(pocket_proc_hook_dry)return true;          /* the harness's fuzz: handles not registered */
#endif
    for(unsigned i=0;i<sizeof course->plan_ok/sizeof course->plan_ok[0];i++)if(course->plan_ok[i].h==h)return course->plan_ok[i].q==q;
    const ksn_shiki_plan *pl=pocket_proc_shiki_plan(h);
    if(!pl)return false;
    const bool ok=pl->inputs==8&&!memcmp(pl->input_q,q,8);
    course->plan_ok[course->plan_ok_next].h=h;course->plan_ok[course->plan_ok_next].q=ok?q:NULL;
    course->plan_ok_next=(course->plan_ok_next+1u)%(sizeof course->plan_ok/sizeof course->plan_ok[0]);
    return ok;
}
/* The series (w, s, g0)'s chord table, made on first use. */
static derby_chord_q *series_rows(int32_t w,int32_t s,int32_t g0){
    derby_course *c=course;
    unsigned at=0;
    for(unsigned i=0;i<DERBY_Q_SLOTS;i++){
        if(c->slot[i]&&c->key[i][0]==w&&c->key[i][1]==s&&c->key[i][2]==g0){c->used[i]=++c->clock;return c->slot[i];}
        if(!c->slot[i]||c->used[i]<c->used[at])at=i;
    }
    if(!c->slot[at]&&!(c->slot[at]=malloc((c->ends-1u)*sizeof(derby_chord_q)))){SERP(serp.fb[FB_MALLOC]++);return NULL;}
    c->used[at]=0;
    SERP(serp.tables++);
    if(!derby_ser_table(c->slot[at],c->qch,c->ends,w,s,g0,pose_of,c)){
        SERP(serp.fb[FB_TABLE]++);
        free(c->slot[at]);c->slot[at]=NULL;return NULL;
    }
    c->key[at][0]=w;c->key[at][1]=s;c->key[at][2]=g0;c->used[at]=++c->clock;
    return c->slot[at];
}
typedef struct {JSContext *ctx;const derby_course *c;} ser_emit;
static int emit_q(void *user,uint32_t h,const int32_t in[8]){
    const ser_emit *e=user;
    if(JS_IsException(SERP_DRAW(pocket_proc_draw_fixed(e->ctx,h,in,8,0))))return 1;
    return course!=e->c?2:0;
}
/* v within lo..hi, counted by the probe as clamp k. */
static int32_t clampi(int32_t v,int32_t lo,int32_t hi,int k){
    (void)k;
    if(v>=lo&&v<=hi)return v;
    SERP(serp.clamp[k]++);
    return v<lo?lo:hi;
}
static JSValue js_ser(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
#ifdef R5SIM_DERBY_PROF
    derby_sp_t=pocket_cycles();
#endif
    const char *op="derby.ser";
    uint32_t h;
    int32_t L,a,w,s,g0,b=0;
    const bool turf=argc<7||JS_IsUndefined(argv[6]);
    /* w, s, g0 and b are Q16 m; a is a colour (rails, int) or, for the turf
     * (no b), a Q16 width. */
    if(argc<6||!plan_handle(ctx,argv[0],&h)||!in32(argv[1],&w)||!in32(argv[2],&s)||
       !in32(argv[3],&g0)||!in32(argv[4],&L)||!in32(argv[5],&a)||
       (!turf&&!in32(argv[6],&b)))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected plan, Q16 w, s, g0, L, a and optional Q16 b");
    if(!course||!view.set)return fail(ctx,op,POCKET_ERR_BUSY,"view required");
    /* The integers' range (the app's calls are inside it; L = 0 was the
     * JS's unused pattern mode). */
    L=clampi(L,1,INT32_MAX,CL_L);
    s=clampi(s,Q16/4,32*Q16,CL_S);
    w=clampi(w,-125*Q16,125*Q16,CL_GW);
    g0=clampi(g0,-10000*Q16,10000*Q16,CL_GW);
    const bool stands=!turf&&b<0;
    if(!plan_q(h,turf?derby_q_pt:derby_q_prail)||(stands&&(!plan_q(view.hl,derby_q_hl)||!plan_q(view.crowd,derby_q_crowd))))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"a plan's inputs are not in the series' Q formats");
    /* No table (the native heap, a value past the table's range): nothing drawn. */
    const derby_chord_q *rows=series_rows(w,s,g0);
    if(!rows)return JS_UNDEFINED;
    SP(0);
    const uint32_t keep=course->sections?course->qvc&keep_mask(w,s):course->qvc;
    SP(1);
    const derby_series_q p={.s=s,.g0=g0,.L=L,.a=a,.b=turf?0:b,.turf=turf,.hl_lines=(int32_t)view.hl_color,
        .cells=view.cells_q,.h=h,.hl=view.hl,.crowd=view.crowd};
    ser_emit e={ctx,course};
#ifdef DERBY_SER_CHECK
    derby_ser_stats *const st=&ser_stats;
    ser_stats.last_j=-1;
#else
    derby_ser_stats stats={0},*const st=&stats;    /* nothing reads them on the device */
#endif
    SERP(serp.chords[view.pie?0:1]+=(uint32_t)__builtin_popcount(keep));
    for(uint32_t m=keep;m;m&=m-1u){
        const int rc=derby_ser_chord_q(&course->camq,&p,&rows[__builtin_ctz(m)],(unsigned)__builtin_ctz(m),emit_q,&e,st);
        if(rc==1)return JS_EXCEPTION;
        if(rc)break;
    }
    return JS_UNDEFINED;
}
/* mode(n): the rotation and cull from the next view(): 0 the PIE kernels, 1
 * the scalar ones (the A/B, docs/apps/derby-ser-division.md sec.13). Returns it. */
static JSValue js_mode(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    int32_t m;
    if(argc<1||!in32(argv[0],&m)||m<0||m>1)return fail(ctx,"derby.mode",POCKET_ERR_INVALID_ARGUMENT,"expected 0 or 1");
    ser_mode=m;
    return JS_NewInt32(ctx,m);
}

/* ---- The side view and the projections, in integers (docs/apps/derby-integer.md
 * sec.5.2). The JS keeps every position as Q16 int32 and calls these; they
 * do course()'s, paint()'s and pan()'s divisions in int64 (side) or in the
 * double pose()/pc machinery above (panning), and pass the plans the same
 * numbers derby_view.js passed (pocket_proc_draw_numbers takes doubles). */
#define DNR_Q 720896            /* 11 m */
#define DFR_Q 1481114           /* 22.6 m */
static int64_t fdiv(int64_t a,int64_t b){int64_t q=a/b;if(a%b&&((a<0)!=(b<0)))q--;return q;}
static int64_t cdiv(int64_t a,int64_t b){return -fdiv(-a,b);}
static int64_t rdiv(int64_t a,int64_t b){return fdiv(2*a+b,2*b);}   /* Math.round(a / b), b > 0 */
static int64_t lmin(int64_t a,int64_t b){return a<b?a:b;}
static int64_t lmax(int64_t a,int64_t b){return a>b?a:b;}
static double dq(int64_t q){return (double)q/65536.0;}
/* sx(w, d0), gy(d0), ty(d0, e) of course(): Q16 px. */
static int64_t sx(int64_t w,int64_t d0){return cam.o+fdiv((w-cam.x0)*cam.f,d0);}
static int64_t gy(int64_t d0){return cam.hy+fdiv(cam.h*cam.f,d0);}
static int64_t ty(int64_t d0,int64_t e){return cam.hy+fdiv((cam.h-e)*cam.f,d0);}
static JSValue drawq(JSContext *ctx,uint32_t h,const double in[8]){return draw(ctx,h,in);}

/* side(f, h, hy, x0, k0, k2): a side camera; k2 > k0: the screen's feed
 * inside the face from px k0 to k2 (course()'s K; k0 may be negative). */
static JSValue js_side(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    int32_t f,h,hy,x0,k0,k2;
    if(argc<6||!in32(argv[0],&f)||f<=0||!in32(argv[1],&h)||!in32(argv[2],&hy)||!in32(argv[3],&x0)||
       !in32(argv[4],&k0)||!in32(argv[5],&k2))
        return fail(ctx,"derby.side",POCKET_ERR_INVALID_ARGUMENT,"expected Q16 f, h, hy, x0 and two px ints");
    cam.f=f;cam.h=h;cam.hy=hy;cam.x0=x0;cam.K=k2>k0;
    cam.o=cam.K?(int64_t)(k0+k2)*32768:120*Q16;
    cam.R=cam.K?(int64_t)(k2-1)*Q16:245*Q16;
    cam.e=cam.K?(int64_t)k0*Q16:-5*Q16;
    return JS_UNDEFINED;
}
/* aim(gu, wu, gl, fmin, px, fmax): a panning unit at pose(gu, wu) turned to
 * the leader at gl (shot()), f keeping the leader px long within fmin..fmax:
 * pc = [x, z, unit x, z, f, distance]. Returns f rounded (px). */
static JSValue js_aim(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    double gu,wu,gl;int32_t fmin,px,fmax;
    if(argc<6||!course||!qn(argv[0],&gu)||!qn(argv[1],&wu)||!qn(argv[2],&gl)||!in32(argv[3],&fmin)||
       !in32(argv[4],&px)||!in32(argv[5],&fmax))
        return fail(ctx,"derby.aim",POCKET_ERR_INVALID_ARGUMENT,"expected course(), Q16 g, w, lead and three px ints");
    double q[4],a[4];
    pose(gu,wu,q);pose(gl,(course->dnr+DFR_Q/65536.0)/2,a);
    const double x=a[0]-q[0],z=a[1]-q[1],e=sqrt(x*x+z*z);
    double *pc=view.pc;
    pc[0]=q[0];pc[1]=q[1];pc[2]=x/e;pc[3]=z/e;pc[4]=js_min(fmax,js_max(fmin,px*e/2.4));pc[5]=e;
    view.fr=sqrt(pc[4]*pc[4]+25600);
    cam.aimed=true;view.set=false;
    return JS_NewInt32(ctx,(int32_t)js_round(pc[4]));
}
/* pj(P) of derby_pan.js. */
static void pj(const double P[4],double o[2]){
    const double *pc=view.pc,a=P[0]-pc[0],b=P[1]-pc[1],Z=(a*pc[2]+b*pc[3])/pc[4];
    o[0]=a*pc[3]-b*pc[2]+120*Z;o[1]=Z;
}
/* wide(g, units, far): the panning unit (0..2 of units' [g, w] rows) nearest
 * the aim at g, plus 4 when it is over far m away (wide()). */
static JSValue js_wide(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    double g;int32_t far;
    const int32_t *u=argc>=3?i32array(ctx,argv[1],6):NULL;
    if(!u||!course||!qn(argv[0],&g)||!in32(argv[2],&far))
        return fail(ctx,"derby.wide",POCKET_ERR_INVALID_ARGUMENT,"expected Q16 g, 6 Q16 unit terms and m");
    double a[4],q[4],e=1e9;
    int m=0;
    pose(g,(course->dnr+DFR_Q/65536.0)/2,a);
    for(int i=0;i<3;i++){
        pose(u[2*i]/65536.0,u[2*i+1]/65536.0,q);
        const double x=a[0]-q[0],z=a[1]-q[1];
        if(x*x+z*z<e)e=x*x+z*z,m=i;
    }
    return JS_NewInt32(ctx,m+(e>(double)far*far?4:0));
}
/* part(kind, h, ...): one piece of the background on the current camera. */
enum{PART_STANDS,PART_RAIL,PART_TURF,PART_POLES,PART_GATE,PART_VIS,PART_PANVIS,PART_PANPOLES};
static double vq[4];
static JSValue js_part(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="derby.part";
    int32_t kind,a[6]={0};
    uint32_t h;
    if(argc<2||!in32(argv[0],&kind)||!plan_handle(ctx,argv[1],&h))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected a kind and a plan");
    for(int i=0;i<6&&i+2<argc;i++)if(!in32(argv[i+2],&a[i]))return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"int32 arguments only");
    if(!h)return JS_UNDEFINED;
    const int64_t f=cam.f,x0=cam.x0;
    JSValue r=JS_UNDEFINED;
    switch(kind){
    case PART_STANDS:{
        /* a: crowd plan, cells (Q16). Stands at 40 m, a pillar every 12 m. */
        uint32_t hc;
        if(!plan_handle(ctx,argv[2],&hc))return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"crowd plan");
        const int64_t d0=40*Q16,q=f/40,j=fdiv(x0-fdiv(130LL*Q16*Q16,q),12*Q16),A=sx(j*12*Q16,d0),dx=12*q,
            n=lmin(fdiv(700LL*Q16-A,dx),cdiv(250LL*Q16-A,dx)+1),g=gy(d0),rise=-fdiv(q*157286,Q16),c=a[1];
        const double in[8]={dq(A),dq(dx),dq(g),dq(rise),(double)n,0,0,dq(ty(d0,13*Q16+32768))};
        r=drawq(ctx,h,in);
        if(JS_IsException(r))return r;
        const int64_t y=g-fdiv(q*78643,Q16);
        const double in2[8]={dq(A),dq(y),dq(A+n*dx),dq(y),dq(rise),dq(rise),dq(j*12*c),dq((j+n)*12*c)};
        if(hc)r=drawq(ctx,hc,in2);
        break;
    }
    case PART_RAIL:{
        /* a: depth (Q16), colour, post spacing (m). */
        const int64_t d0=a[0],k4=a[2],p=fdiv(f*Q16,d0),s=k4*p,t=fdiv((cam.o-cam.e)*Q16,p),
            n=cam.K?cdiv(x0-t,k4*Q16):fdiv(x0-t,k4*Q16),A=sx(n*k4*Q16,d0),
            c=lmax(0,cam.K?fdiv(cam.R-A,s):lmin(fdiv(700LL*Q16-A,s),cdiv(cam.R-A,s)));
        const double in[8]={dq(A),dq(s),dq(ty(d0,72090)),dq(gy(d0)),(double)c,dq(ty(d0,36045)),a[1],0};
        r=drawq(ctx,h,in);
        break;
    }
    case PART_TURF:{
        /* a: stripe width (m). Lines of constant distance, none whose near end
         * is left of -470 px (the VM's -480 limit). */
        const int64_t w=a[0],q=fdiv(f*Q16,DFR_Q),pn=fdiv(f*Q16,DNR_Q),
            i0=lmax(fdiv(x0-fdiv(125LL*Q16*Q16,q),w*Q16),cdiv(x0-fdiv(590LL*Q16*DNR_Q,f),w*Q16)),
            xf=sx(i0*w*Q16,DFR_Q),xn=sx(i0*w*Q16,DNR_Q),
            n=lmax(0,lmin(cdiv(250LL*Q16-xf,w*q),fdiv(700LL*Q16-xn,w*pn)));
        const double in[8]={dq(xn),dq(xf),dq(w*pn),dq(w*q),dq(gy(DNR_Q)),dq(gy(DFR_Q)),(double)n,(double)(i0&1)};
        r=drawq(ctx,h,in);
        break;
    }
    case PART_POLES:{
        const int64_t q=fdiv(f*Q16,DFR_Q);
        for(int m=200;m<=1000;m+=200){
            const int64_t p=sx((int64_t)m*Q16,DFR_Q);
            const bool e=m==1000;
            if(p<=-40*Q16||p>=280*Q16)continue;
            const double in[8]={dq(p),dq(gy(DFR_Q)),dq(ty(DFR_Q,e?4*Q16:170394)),dq(fdiv((e?36045:19661)*q,Q16)),
                dq(e?sx((int64_t)m*Q16,DNR_Q):p),dq(gy(e?DNR_Q:DFR_Q)),dq(p),dq(gy(DFR_Q))};
            r=drawq(ctx,h,in);
            if(JS_IsException(r))return r;
        }
        break;
    }
    case PART_GATE:{
        /* The stalls at 0 m: q = f sqrt(1.085) / 12 (px a m at the first lane). */
        const int64_t q=fdiv(f*68265,12*Q16),x=fdiv(x0*q,Q16);
        if(x<=-400*Q16||x>=400*Q16)break;
        const double in[8]={dq(-x),dq(fdiv(cam.h*q,Q16)),dq(fdiv((cam.h-170394)*q,Q16)),dq(60402),dq(cam.hy),0,0,0};
        r=drawq(ctx,h,in);
        break;
    }
    case PART_VIS:{
        /* a: the face's rect (px), its colour and depth (m). */
        const double in[8]={a[0]-1,a[1]-1,a[2],a[3],(double)lmax(1,rdiv(f*4,(int64_t)a[5]*10*Q16)),dq(gy((int64_t)a[5]*Q16)),a[4],0};
        r=drawq(ctx,h,in);
        break;
    }
    case PART_PANVIS:{
        /* screen()'s edges vq on a panning unit: the face's lines, a colour. */
        const double m=ceil(10*js_max(vq[1],vq[3]))+1,
            in[8]={vq[0],vq[1],vq[2],vq[3],0,-10/(m-1),m,a[0]},in2[8]={vq[0],vq[1],vq[2],vq[3],0,-10,2,10565};
        r=drawq(ctx,h,in);
        if(JS_IsException(r))return r;
        r=drawq(ctx,h,in2);
        break;
    }
    case PART_PANPOLES:{
        if(!course||!cam.aimed)break;
        for(int m=200;m<=1000;m+=200){
            double P[4],p[2],n[2];
            pose(m,DFR_Q/65536.0,P);pj(P,p);
            const bool e=m==1000;
            const double rr=1/p[1],x=p[0]*rr;
            if(e){pose(m,course->dnr,P);pj(P,n);}else{n[0]=p[0];n[1]=p[1];}
            const double y=n[0]/n[1];
            if(p[1]>.02&&n[1]>.02&&x>-40&&x<280&&y>-400&&y<640){
                const double in[8]={x,28+6*rr,28+(e?2:3.4)*rr,(e?.55:.3)*rr,y,28+6/n[1],x,28+6*rr};
                r=drawq(ctx,h,in);
                if(JS_IsException(r))return r;
            }
        }
        break;
    }
    default:return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"unknown part");
    }
    return r;
}
/* A runner's hip X, scale S (px a unit), ground Y, all Q16 px, and its depth
 * (Q16 m): on the side camera (side()) from x (Q16 m along) and z (Q16 m
 * deep); with pan, on the panning unit (aim()) through pose() and pj()
 * (depth 0: behind the camera). X is held within +-16,000 px. */
static void lane(bool pan,int32_t x,int32_t z,int32_t o[4]){
    int64_t X,S,Y,Z;
    if(pan&&cam.aimed&&course){
        double P[4],p[2];
        pose(x/65536.0-12.5/6,z/65536.0,P);pj(P,p);
        const double r=p[1]>0?1/p[1]:0;
        X=(int64_t)js_round(js_max(-16000,js_min(16000,p[0]*r))*65536);
        S=(int64_t)js_round(r/6*65536);Y=(int64_t)js_round((28+6*r)*65536);
        Z=p[1]>.02?(int64_t)js_round(p[1]*65536):0;
    }else{
        const int64_t q=fdiv(cam.f*Q16,z);
        X=cam.o+fdiv(((int64_t)x-136533-cam.x0)*q,Q16);
        S=q/6;Y=gy(z);Z=z;
    }
    o[0]=(int32_t)lmax(-16000LL*Q16,lmin(16000LL*Q16,X));
    o[1]=(int32_t)S;o[2]=(int32_t)Y;o[3]=(int32_t)lmin(INT32_MAX,Z);
}
/* lanes(xs, lz, out, pan): lane() for each runner, 4 a lane in out. */
static JSValue js_lanes(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const int32_t *xs=argc>=3?i32array(ctx,argv[0],8):NULL,*lz=xs?i32array(ctx,argv[1],8):NULL;
    int32_t *out=lz?i32array(ctx,argv[2],32):NULL;
    if(!out)return fail(ctx,"derby.lanes",POCKET_ERR_INVALID_ARGUMENT,"expected Int32Array(8) x2 and Int32Array(32)");
    const bool pan=argc>=4&&JS_ToBool(ctx,argv[3]);
    for(int l=0;l<8;l++)lane(pan,xs[l],lz[l],out+4*l);
    return JS_UNDEFINED;
}
/* HEAD ON, a still camera 12 m past the line, 2.2 m up, looking back: a
 * horse's x, ground y and px a unit (Q16). */
static void head(int32_t x,int32_t z,int32_t o[3]){
    const int64_t d=lmax(Q16/16,1012LL*Q16-x),q=fdiv(400LL*Q16*Q16,d);
    o[0]=(int32_t)(120*Q16+fdiv(((int64_t)z-1081344)*q,Q16));
    o[1]=(int32_t)(50*Q16+fdiv(q*144179,Q16));
    o[2]=(int32_t)(q/6);
}
/* head(xs, lz, out): head() for each horse, 3 a lane in out. */
static JSValue js_head(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const int32_t *xs=argc>=3?i32array(ctx,argv[0],8):NULL,*lz=xs?i32array(ctx,argv[1],8):NULL;
    int32_t *out=lz?i32array(ctx,argv[2],24):NULL;
    if(!out)return fail(ctx,"derby.head",POCKET_ERR_INVALID_ARGUMENT,"expected Int32Array(8) x2 and Int32Array(24)");
    for(int l=0;l<8;l++)head(xs[l],lz[l],out+3*l);
    return JS_UNDEFINED;
}
/* runners(xs, lz, order, co, ha, hb, mode, close, mask, from): the runners'
 * draws, which derby_view.js made in JS until P4b (docs/apps/derby-runners-native.md;
 * the JS path was retired with the A/B's chunk). order is the JS array of the 8
 * lanes (zo, or ro for HEAD ON), co an Int32Array of the coats (0..7) and
 * silks (8..15). Each runner drawn is two Shiki draws with integer inputs
 * (as drawMany's Int32Array: no conversion), ha then hb, the far one first:
 *   mode 0, the side camera: lanes in order from from, those in mask; ha legs
 *     (X, S, Y, d, coat, 0, 0), hb run (..., silk, silk ^ 0x8410), d = x +
 *     67797 l. Within the screen's face (side()'s K) a runner wholly on it,
 *     else X within -160..400 px. The close-up lane is not drawn: the call
 *     returns its place in order (the JS draws it and calls again from the
 *     next);
 *   mode 1, the panning unit: order sorted far to near first (stably, as the
 *     JS sort was, and written back: the next frame's sort starts from it),
 *     then every lane in front of the camera with X within -160..400 px;
 *   mode 2, HEAD ON: order from its end, every lane: ha frl, hb frb (x, y, s,
 *     d, coat, silk).
 * A handle that is 0 draws nothing (the JS's live[n] not loaded). Returns -1,
 * or the close-up lane's place. A draw that fails throws as drawMany does. */
static JSValue js_runners(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="derby.runners";
    const int32_t *xs=argc>=10?i32array(ctx,argv[0],8):NULL,*lz=xs?i32array(ctx,argv[1],8):NULL,
        *co=lz?i32array(ctx,argv[3],16):NULL;
    uint32_t ha=0,hb=0;
    int32_t mode,close,mask,from,ord[8];
    if(!co||!plan_handle(ctx,argv[4],&ha)||!plan_handle(ctx,argv[5],&hb)||!in32(argv[6],&mode)||mode<0||mode>2||
       !in32(argv[7],&close)||!in32(argv[8],&mask)||!in32(argv[9],&from)||from<0||from>8)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected Int32Array(8) x2, an order, Int32Array(16), two plans and 4 ints");
    for(int i=0;i<8;i++){
        JSValue v=JS_GetPropertyUint32(ctx,argv[2],(uint32_t)i);
        const bool ok=in32(v,&ord[i])&&ord[i]>=0&&ord[i]<8;
        JS_FreeValue(ctx,v);
        if(!ok)return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"order: the 8 lanes");
    }
    int32_t p[8][4];
    if(mode<2)for(int l=0;l<8;l++)lane(mode==1,xs[l],lz[l],p[l]);
    if(mode==1){
        /* zo.sort((a, b) => Z[b] - Z[a]): stable, so by insertion. */
        int32_t s[8];
        memcpy(s,ord,sizeof s);
        for(int i=1;i<8;i++){
            const int32_t k=s[i];
            int j=i;
            for(;j>0&&p[s[j-1]][3]<p[k][3];j--)s[j]=s[j-1];
            s[j]=k;
        }
        for(int i=0;i<8;i++)if(s[i]!=ord[i]){
            ord[i]=s[i];
            if(JS_SetPropertyUint32(ctx,argv[2],(uint32_t)i,JS_NewInt32(ctx,s[i]))<0)return JS_EXCEPTION;
        }
    }
    unsigned k=0;
    for(int i=from;i<8;i++){
        const int l=ord[mode==2?7-i:i];
        int32_t in[7];
        const int32_t d=(int32_t)((int64_t)xs[l]+67797LL*l),coat=co[l],silk=co[8+l];
        unsigned n=7;
        if(mode==2){
            int32_t h[3];
            head(xs[l],lz[l],h);
            in[0]=h[0];in[1]=h[1];in[2]=h[2];in[3]=d;in[4]=coat;in[5]=silk;n=6;
        }else{
            const int64_t X=p[l][0],S=p[l][1];
            if(mode==1){
                if(!p[l][3]||!(X>-160LL*Q16&&X<400LL*Q16))continue;
            }else{
                if(!(mask>>l&1))continue;
                if(l==close)return JS_NewInt32(ctx,i);
                /* 5 * s >> 1 is ToInt32 of the double, then the shift. */
                const int64_t a=(int32_t)(uint32_t)(uint64_t)(5*S)>>1,b=(int32_t)(uint32_t)(uint64_t)(25*S)>>1;
                if(!(cam.K?X-a>=cam.e&&X+b<=cam.R:X>-160LL*Q16&&X<400LL*Q16))continue;
            }
            in[0]=(int32_t)X;in[1]=(int32_t)S;in[2]=p[l][2];in[3]=d;in[4]=coat;in[5]=0;in[6]=0;
        }
        if(!ha||!hb)continue;
        JSValue r=pocket_proc_draw_fixed(ctx,ha,in,n,k++);
        if(JS_IsException(r))return r;
        if(mode<2){in[5]=silk;in[6]=silk^0x8410;}
        r=pocket_proc_draw_fixed(ctx,hb,in,n,k++);
        if(JS_IsException(r))return r;
    }
    return JS_NewInt32(ctx,-1);
}
/* screen(out, pan, x, d, w, b, t): the big screen (centre x, depth d, half
 * width w, bottom b and top t, m) on the side camera (or with pan, the
 * panning unit) as px ints [x0, y0, x1, y1] in out, or 0 when it is out of
 * view or over the map (paint()). On a panning unit it keeps the face's
 * edges for part(PANVIS). */
static JSValue js_screen(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    int32_t *out=argc>=7?i32array(ctx,argv[0],4):NULL,vs[5];
    for(int i=0;i<5&&out;i++)if(!in32(argv[i+2],&vs[i]))out=NULL;
    if(!out||!course)return fail(ctx,"derby.screen",POCKET_ERR_INVALID_ARGUMENT,"expected course(), Int32Array(4), pan and 5 m ints");
    double A[4],B[4];
    pose(vs[0]-vs[2],vs[1],A);pose(vs[0]+vs[2],vs[1],B);
    double v[4];
    if(argc>=2&&JS_ToBool(ctx,argv[1])&&cam.aimed){
        double p[2],q[2];
        pj(A,p);pj(B,q);
        const double x0=p[0]/p[1],x1=q[0]/q[1];
        vq[0]=x0;vq[1]=1/p[1];vq[2]=x1;vq[3]=1/q[1];
        if(!(p[1]>.02&&q[1]>.02&&x0<=239&&x1>=1&&x1-x0>=8&&x1-x0<=160&&28-10*js_max(vq[1],vq[3])>=13))return JS_NewInt32(ctx,0);
        v[0]=js_round(x0);v[1]=ceil(28-10*js_min(vq[1],vq[3]));v[2]=js_round(x1);v[3]=28;
    }else{
        const double f=dq(cam.f),x0=dq(cam.x0),h=dq(cam.h),hy=dq(cam.hy),p=f/A[1];
        v[0]=js_round(120+(A[0]-x0)*p);v[1]=js_round(hy+(h-vs[4])*p);v[2]=js_round(120+(B[0]-x0)*p);v[3]=js_round(hy+(h-vs[3])*p);
        if(v[0]>239||v[2]<1||v[1]<13||v[1]>134||v[2]-v[0]<8||v[2]-v[0]>160)return JS_NewInt32(ctx,0);
    }
    for(int i=0;i<4;i++)out[i]=(int32_t)v[i];
    return JS_NewInt32(ctx,1);
}

#ifdef POCKET_DERBY_SER_PROBE
/* view() and ser() timed whole (the probe above), the shadow path's cycles
 * taken out. */
static JSValue js_view_serp(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    serp.shadow_acc=0;
    const uint32_t t=pocket_cycles();
    const JSValue r=js_view(ctx,self,argc,argv);
    const int k=view.pie?0:1;
    serp.view_cy[k]+=pocket_cycles()-t-serp.shadow_acc;serp.views[k]++;
    return r;
}
static JSValue js_ser_serp(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    serp.draw_acc=serp.draw_n=serp.draw_insn_acc=serp.draw_ist_acc=serp.shadow_acc=0;serp.in_ser=true;
    uint32_t i0,s0,i1,s1;
    serp_pm(&i0,&s0);
    const uint32_t t=pocket_cycles();
    const JSValue r=js_ser(ctx,self,argc,argv);
    const uint32_t dt=pocket_cycles()-t-serp.shadow_acc;
    serp_pm(&i1,&s1);
    serp.in_ser=false;
    const int k=view.pie?0:1;
    serp.calls[k]++;serp.ser_cy[k]+=dt;serp.draw_cy[k]+=serp.draw_acc;serp.draws[k]+=serp.draw_n;
    serp.insn[k]+=i1-i0;serp.ist[k]+=s1-s0;serp.draw_insn[k]+=serp.draw_insn_acc;serp.draw_ist[k]+=serp.draw_ist_acc;
    return r;
}
#define js_view js_view_serp
#define js_ser js_ser_serp
void pocket_derby_ser_probe_log(unsigned frames){
    if(serp.views[0]+serp.views[1]+serp.calls[0]+serp.calls[1]){
        /* Per panning frame (a view()), not per tick: a frame() the turn
         * budget parks runs on in a continuation tick (ticks= counts ticks).
         * X/Y: PIE/scalar. ser_cy, draw_cy, view_cy, insn, ist: the frames
         * on that path; rot_cy, cull_cy: every frame, both paths (the other
         * one a shadow); culls: cull calls, cull_diff: chords the two paths
         * keep differently (their margins differ), wide: chords culled in
         * int64 past the lanes' range. */
        const unsigned v=serp.views[0]+serp.views[1],f=v?v:1u,f0=serp.views[0]?serp.views[0]:1u,
            f1=serp.views[1]?serp.views[1]:1u;
        PROBE_PRINT("DERBY_SER mode=%s ticks=%u views=%u/%u ser=%u/%u clamp=L:%u,s:%u,gw:%u,cam:%u "
                    "fb=malloc:%u,table:%u tables=%u ser_cy=%u/%u draw_cy=%u/%u view_cy=%u/%u "
                    "rot_cy=%u/%u cull_cy=%u/%u culls=%u cull_diff=%u wide=%u/%u draws=%u/%u chords=%u/%u "
                    "insn=%u/%u draw_insn=%u/%u ist=%u/%u draw_ist=%u/%u",
                    ser_mode?"SCALAR":"PIE",frames,(unsigned)serp.views[0],(unsigned)serp.views[1],
                    (unsigned)serp.calls[0],(unsigned)serp.calls[1],(unsigned)serp.clamp[CL_L],(unsigned)serp.clamp[CL_S],
                    (unsigned)serp.clamp[CL_GW],(unsigned)serp.clamp[CL_CAM],(unsigned)serp.fb[FB_MALLOC],
                    (unsigned)serp.fb[FB_TABLE],(unsigned)serp.tables,
                    (unsigned)(serp.ser_cy[0]/f0),(unsigned)(serp.ser_cy[1]/f1),(unsigned)(serp.draw_cy[0]/f0),
                    (unsigned)(serp.draw_cy[1]/f1),(unsigned)(serp.view_cy[0]/f0),(unsigned)(serp.view_cy[1]/f1),
                    (unsigned)(serp.rot_cy[0]/f),(unsigned)(serp.rot_cy[1]/f),(unsigned)(serp.cull_cy[0]/f),
                    (unsigned)(serp.cull_cy[1]/f),(unsigned)serp.culls,(unsigned)serp.cull_diff,
                    (unsigned)serp.wide[0],(unsigned)serp.wide[1],
                    (unsigned)serp.draws[0],(unsigned)serp.draws[1],(unsigned)serp.chords[0],(unsigned)serp.chords[1],
                    (unsigned)(serp.insn[0]/f0),(unsigned)(serp.insn[1]/f1),(unsigned)(serp.draw_insn[0]/f0),
                    (unsigned)(serp.draw_insn[1]/f1),(unsigned)(serp.ist[0]/f0),(unsigned)(serp.ist[1]/f1),
                    (unsigned)(serp.draw_ist[0]/f0),(unsigned)(serp.draw_ist[1]/f1));
    }
    memset(&serp,0,sizeof serp);
}
#endif
#ifdef R5SIM_DERBY_PROF
/* r5sim only (tools/r5sim, -DR5SIM_DERBY_PROF=1): cycles in each native
 * function and the Shiki draws' cycles inside it; the turn's Shiki carry
 * (the draws taken out here, for r5sim's per-turn total). */
uint32_t derby_prof_cy[12],derby_prof_shk[12],derby_prof_n[12],derby_prof_draws[12];
pocket_shiki_turn derby_prof_carry;
#define PROF(i,f) static JSValue f##_p(JSContext *c,JSValueConst s,int a,JSValueConst *v){ \
    pocket_shiki_turn x,y;pocket_shiki_turn_take(&x); \
    derby_prof_carry.draws+=x.draws;derby_prof_carry.total_cy+=x.total_cy;derby_prof_carry.engine_cy+=x.engine_cy; \
    const uint32_t t=pocket_cycles();JSValue r=f(c,s,a,v);derby_prof_cy[i]+=pocket_cycles()-t;derby_prof_n[i]++; \
    pocket_shiki_turn_take(&y);derby_prof_shk[i]+=y.total_cy;derby_prof_draws[i]+=y.draws; \
    derby_prof_carry.draws+=y.draws;derby_prof_carry.total_cy+=y.total_cy;derby_prof_carry.engine_cy+=y.engine_cy; \
    return r;}
PROF(0,js_view) PROF(1,js_ser) PROF(2,js_side) PROF(3,js_aim) PROF(4,js_wide) PROF(5,js_part)
PROF(6,js_lanes) PROF(7,js_head) PROF(8,js_runners) PROF(9,js_screen) PROF(10,js_course)
#define js_view js_view_p
#define js_ser js_ser_p
#define js_side js_side_p
#define js_aim js_aim_p
#define js_wide js_wide_p
#define js_part js_part_p
#define js_lanes js_lanes_p
#define js_head js_head_p
#define js_runners js_runners_p
#define js_screen js_screen_p
#define js_course js_course_p
#endif
esp_err_t pocket_derby_fill(JSContext *ctx,JSValueConst ns,void *unused){
    (void)unused;
    static const JSCFunctionListEntry methods[]={
        JS_CFUNC_DEF("course",3,js_course),
        JS_CFUNC_DEF("view",7,js_view),
#ifdef POCKET_DERBY_PROJECT_BATCH
        JS_CFUNC_DEF("project",4,js_project),
#endif
        JS_CFUNC_DEF("ser",7,js_ser),
        JS_CFUNC_DEF("mode",1,js_mode),
        JS_CFUNC_DEF("side",6,js_side),
        JS_CFUNC_DEF("aim",6,js_aim),
        JS_CFUNC_DEF("wide",3,js_wide),
        JS_CFUNC_DEF("part",8,js_part),
        JS_CFUNC_DEF("lanes",4,js_lanes),
        JS_CFUNC_DEF("head",3,js_head),
        JS_CFUNC_DEF("runners",10,js_runners),
        JS_CFUNC_DEF("screen",7,js_screen),
    };
    return JS_SetPropertyFunctionList(ctx,ns,methods,(int)(sizeof methods/sizeof methods[0]))<0?
           ESP_ERR_NO_MEM:ESP_OK;
}
/* Every number here is refused past by js_course(). */
static const pocket_limit_t derby_limits[]={
    {.name="maxEnds",.kind=POCKET_LIMIT_INT,.number=DERBY_ENDS_MAX},
    {.name="maxSections",.kind=POCKET_LIMIT_INT,.number=DERBY_SECTIONS_MAX},
    {0},
};
static const pocket_capability_t capability={.name="derby.series",.supported=true,
    .available=true,.limits=derby_limits};
esp_err_t pocket_derby_install(JSContext *ctx,void *unused){
    (void)unused;
    esp_err_t err=pocket_api_register(&capability);
    return err==ESP_OK?pocket_api_lazy(ctx,"derby",pocket_derby_fill,NULL):err;
}
void pocket_derby_reset(void){
    free_course(course);
    course=NULL;
#ifdef POCKET_DERBY_PROJECT_BATCH
    ++course_generation;++view_generation;
#endif
#if defined(POCKET_DERBY_COURSE_CACHE) || defined(POCKET_DERBY_PROJECT_BATCH)
    projection_counts.cache_bytes=0;
#endif
    memset(&view,0,sizeof view);
    memset(&cam,0,sizeof cam);
    /* The path back to its default: the next session's JS starts its A/B
     * flag at PIE (derby_view.js AB = 0), and a mode(1) left by this one
     * would run it on the scalar path under the PIE label. */
    ser_mode=0;
}
