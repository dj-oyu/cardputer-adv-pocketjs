#include "pocket_storage.h"
#include "pocket_api.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- limits
//
// Everything below is enforced before the write reaches flash, because a limit
// an app can talk its way past is worse than no limit: it turns a refusal that
// should arrive as LIMIT_EXCEEDED into an ESP_ERR_NVS_NOT_ENOUGH_SPACE that
// some other namespace pays for.
//
// Section 14 proposes 4096 bytes per value and 16KiB per app. The value size is
// kept; the per-app total is not, and this is a deliberate deviation. The nvs
// partition is 0x6000 = 24KiB (partitions.csv), NVS keeps one 4KiB page in
// reserve for its garbage collection, and an entry is 32 bytes, so the whole
// partition holds about 630 entries — under 20KiB of payload — shared with the
// home settings, the SKK preferences, the tutorial's progress and whatever
// Wi-Fi calibration a later stage stores. A 16KiB app quota would be a promise
// the flash cannot keep, and the app that filled it would break the shell
// rather than itself. 8KiB leaves roughly half the partition for the system.
#define KV_MAX_KEY_BYTES   64
#define KV_MAX_VALUE_BYTES 4096
#define KV_QUOTA_BYTES     8192
#define KV_MAX_DEPTH       16
#define KV_MAX_TIMEOUT_MS  30000

// The same quota expressed in the unit NVS actually rations. entry_cost()
// mirrors the layout NVS documents: one index entry, one item header and the
// payload rounded up to whole entries.
#define KV_ENTRY_BYTES     32
#define KV_QUOTA_ENTRIES   (KV_QUOTA_BYTES / KV_ENTRY_BYTES)
// Refuse a write that would leave the partition with less than this for the
// namespaces the device needs to boot and to remember its settings.
#define KV_RESERVE_ENTRIES 128

// A traversal budget stands in for a cycle check. A value that fits the size
// limit cannot have more nodes than it has bytes, so a value that exceeds this
// is either far too large or self-referential; either way it is refused before
// the walk can run the JS stack out.
#define KV_MAX_NODES 8192

static const char *TAG = "pocket.kv";

// ---------------------------------------------------------------- records
//
// One NVS blob per key: an 8 byte header, the key as the app spelled it, then
// the JSON text. The key is stored because the NVS key is a hash and a hash is
// not a name — reading it back is what turns "this slot exists" into "this slot
// is yours". The revision lives in the record rather than in a counter beside
// it so that an interrupted write cannot leave the two disagreeing.
#define KV_MAGIC 0x314BU   // 'K','1': the record layout below, version 1

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint16_t key_len;
    uint32_t revision;
} kv_header_t;

#define KV_RECORD_MAX (sizeof(kv_header_t) + KV_MAX_KEY_BYTES + KV_MAX_VALUE_BYTES)

typedef struct {
    uint8_t    *blob;       // NULL when the key has no record
    size_t      size;
    kv_header_t header;
    const char *json;       // into blob, and NUL terminated by record_load
    size_t      json_len;
} kv_record_t;

// ------------------------------------------------------------------ state

// Section 3 gives the host the app identity and forbids JS from claiming
// another app's store. Today's session model has no identity to hand over yet —
// one app runs at a time, from the built-in source or the Playground's slot —
// so the owner is a host-settable name with a default, and the isolation is
// already real: each owner gets its own NVS namespace, and nothing an app can
// say changes which namespace it opened. When app registration lands, the host
// calls pocket_storage_set_owner() and the same mechanism separates the apps.
#define KV_DEFAULT_OWNER "local.default"
static char owner[48] = KV_DEFAULT_OWNER;

typedef struct {
    JSValue      object_proto;   // to tell a plain object from an instance
    nvs_handle_t nvs;
    bool         open;
} storage_state_t;

static storage_state_t *state;
static JSClassID        hub_class;

void pocket_storage_set_owner(const char *app_id) {
    snprintf(owner, sizeof(owner), "%s",
             (app_id && app_id[0]) ? app_id : KV_DEFAULT_OWNER);
}

// ------------------------------------------------------------------ names
//
// NVS names hold 15 characters; section 7 keys hold 64 bytes. Both mappings are
// therefore a hash, and both are pure functions of their input so that get, set
// and remove agree without any table. FNV-1a is used for its size, not its
// strength: the store is not adversarial, and the record's own copy of the key
// is what makes a collision visible instead of silent.
static uint64_t fnv1a(const void *data, size_t length) {
    const uint8_t *p = data;
    uint64_t hash = 0xcbf29ce484222325ULL;
    for(size_t i=0;i<length;i++) { hash^=p[i]; hash*=0x100000001b3ULL; }
    return hash;
}

static void namespace_name(char out[NVS_NS_NAME_MAX_SIZE]) {
    uint32_t hash = (uint32_t)fnv1a(owner, strlen(owner));
    // "kv" keeps these out of the way of the host's own named namespaces.
    snprintf(out, NVS_NS_NAME_MAX_SIZE, "kv%08lx", (unsigned long)hash);
}

// 56 bits of the key hash as hex fills the 15 characters exactly. Two distinct
// keys landing on one slot is a 1-in-2^56 event that set() reports as CONFLICT
// rather than resolving by probing: probing would need tombstones to survive
// remove(), and tombstones cost flash entries on every app to protect against
// something no app will meet.
static void record_name(const char *key, size_t length,
                        char out[NVS_KEY_NAME_MAX_SIZE]) {
    uint64_t hash = fnv1a(key, length) & 0x00ffffffffffffffULL;
    snprintf(out, NVS_KEY_NAME_MAX_SIZE, "k%014llx", (unsigned long long)hash);
}

// Section 4 rejects malformed UTF-8 and lone surrogates at a text API, and a
// key is one. QuickJS hands back WTF-8 for an unpaired surrogate, so the check
// on the encoded bytes catches both at once.
static bool utf8_valid(const uint8_t *s, size_t n) {
    for(size_t i=0;i<n;) {
        uint8_t c=s[i];
        size_t  extra;
        uint32_t cp;
        if(c<0x80) { i++; continue; }
        else if((c&0xe0)==0xc0) { extra=1; cp=c&0x1fU; }
        else if((c&0xf0)==0xe0) { extra=2; cp=c&0x0fU; }
        else if((c&0xf8)==0xf0) { extra=3; cp=c&0x07U; }
        else return false;
        if(i+extra>=n) return false;
        for(size_t k=1;k<=extra;k++) {
            if((s[i+k]&0xc0)!=0x80) return false;
            cp=(cp<<6)|(uint32_t)(s[i+k]&0x3fU);
        }
        if(extra==1 && cp<0x80) return false;
        if(extra==2 && cp<0x800) return false;
        if(extra==3 && cp<0x10000) return false;
        if(cp>0x10ffff) return false;
        if(cp>=0xd800 && cp<=0xdfff) return false;   // lone surrogate
        i+=extra+1;
    }
    return true;
}

// ---------------------------------------------------------------- the store

static bool store_open(void) {
    if(!state) return false;
    if(state->open) return true;
    // shell_init() initialises NVS at boot; this repeats the call because it is
    // a no-op once the driver is up and because storage should not depend on
    // the home screen having got there first. The erase-and-retry recovery is
    // deliberately not done here: erasing the partition to make one app's
    // storage work would take the device's settings with it.
    esp_err_t err=nvs_flash_init();
    if(err!=ESP_OK) { ESP_LOGW(TAG,"nvs unavailable: %s",esp_err_to_name(err)); return false; }
    char name[NVS_NS_NAME_MAX_SIZE];
    namespace_name(name);
    err=nvs_open(name,NVS_READWRITE,&state->nvs);
    if(err!=ESP_OK) {
        ESP_LOGW(TAG,"nvs_open(%s): %s",name,esp_err_to_name(err));
        return false;
    }
    state->open=true;
    return true;
}

static void record_free(kv_record_t *record) {
    free(record->blob);
    record->blob=NULL;
}

// ESP_ERR_INVALID_SIZE marks a record this build did not write or cannot trust.
static esp_err_t record_load(const char *name, kv_record_t *out) {
    memset(out,0,sizeof(*out));
    size_t size=0;
    esp_err_t err=nvs_get_blob(state->nvs,name,NULL,&size);
    if(err==ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if(err!=ESP_OK) return err;
    if(size<sizeof(kv_header_t) || size>KV_RECORD_MAX) return ESP_ERR_INVALID_SIZE;
    // One byte past the record: JS_ParseJSON reads buf[buf_len] and the JSON
    // sits at the end of the blob, so the terminator has to be there.
    uint8_t *blob=malloc(size+1);
    if(!blob) return ESP_ERR_NO_MEM;
    err=nvs_get_blob(state->nvs,name,blob,&size);
    if(err!=ESP_OK) { free(blob); return err; }
    blob[size]=0;
    memcpy(&out->header,blob,sizeof(out->header));
    if(out->header.magic!=KV_MAGIC ||
       out->header.key_len>KV_MAX_KEY_BYTES ||
       sizeof(kv_header_t)+out->header.key_len>size) {
        free(blob);
        return ESP_ERR_INVALID_SIZE;
    }
    out->blob=blob;
    out->size=size;
    out->json=(const char *)blob+sizeof(kv_header_t)+out->header.key_len;
    out->json_len=size-sizeof(kv_header_t)-out->header.key_len;
    return ESP_OK;
}

static bool record_is(const kv_record_t *record, const char *key, size_t length) {
    return record->blob && record->header.key_len==length &&
           !memcmp(record->blob+sizeof(kv_header_t),key,length);
}

static size_t entry_cost(size_t bytes) {
    return 2 + (bytes+KV_ENTRY_BYTES-1)/KV_ENTRY_BYTES;
}

// True when a record of `bytes` may be written after `replacing` bytes are
// given back. Both the app's own quota and the partition's floor are checked,
// because the second is what keeps a full store from becoming someone else's
// problem.
static bool room_for(size_t bytes, size_t replacing) {
    size_t used=0;
    if(nvs_get_used_entry_count(state->nvs,&used)!=ESP_OK) return false;
    size_t give_back=replacing?entry_cost(replacing):0;
    size_t want=entry_cost(bytes);
    size_t projected=used>give_back?used-give_back:0;
    if(projected+want>KV_QUOTA_ENTRIES) return false;
    nvs_stats_t stats;
    if(nvs_get_stats(NULL,&stats)!=ESP_OK) return false;
    return stats.available_entries>=want+KV_RESERVE_ENTRIES;
}

// ------------------------------------------------------------- arguments

// Fills `name` from argv[0]. Returns a rejection Promise on failure, and
// JS_UNDEFINED when the key is good.
static JSValue take_key(JSContext *ctx, JSValueConst value, const char *operation,
                        char name[NVS_KEY_NAME_MAX_SIZE]) {
    if(!JS_IsString(value))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                 "key must be a string",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    size_t length=0;
    const char *key=JS_ToCStringLen(ctx,&length,value);
    if(!key) return JS_EXCEPTION;
    JSValue error=JS_UNDEFINED;
    if(length<1 || length>KV_MAX_KEY_BYTES)
        error=pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                "key must be 1 to 64 UTF-8 bytes",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    else if(!utf8_valid((const uint8_t *)key,length))
        error=pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                "key is not well-formed UTF-8",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    else
        record_name(key,length,name);
    JS_FreeCString(ctx,key);
    return error;
}

typedef struct {
    int64_t if_revision;    // -1 for "not given"
    bool    cancelled;
} kv_options_t;

// Section 14 caps a general Promise at 30000ms and section 4 refuses to round a
// request quietly, so an out-of-range timeoutMs is an argument error even
// though this implementation cannot spend it. Options.cancel is checked once,
// here, which is the only moment a synchronous operation has to observe it.
static JSValue take_options(JSContext *ctx, JSValueConst value,
                            const char *operation, kv_options_t *out) {
    out->if_revision=-1;
    out->cancelled=false;
    if(JS_IsUndefined(value) || JS_IsNull(value)) return JS_UNDEFINED;
    if(!JS_IsObject(value))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                 "options must be an object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    JSValue timeout=JS_GetPropertyStr(ctx,value,"timeoutMs");
    if(JS_IsException(timeout)) return JS_EXCEPTION;
    if(!JS_IsUndefined(timeout)) {
        double ms=0;
        bool bad=!JS_IsNumber(timeout) || JS_ToFloat64(ctx,&ms,timeout);
        JS_FreeValue(ctx,timeout);
        if(bad || !isfinite(ms) || ms!=(double)(int64_t)ms ||
           ms<1 || ms>KV_MAX_TIMEOUT_MS)
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                     "timeoutMs must be a whole number of 1 to 30000",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
    } else JS_FreeValue(ctx,timeout);

    JSValue cancel=JS_GetPropertyStr(ctx,value,"cancel");
    if(JS_IsException(cancel)) return JS_EXCEPTION;
    if(!JS_IsUndefined(cancel) && !JS_IsNull(cancel)) {
        if(!pocket_api_is_cancel_token(cancel)) {
            JS_FreeValue(ctx,cancel);
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                     "cancel must be a token from pocket.cancel.source()",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        }
        out->cancelled=pocket_api_cancel_requested(cancel);
    }
    JS_FreeValue(ctx,cancel);

    JSValue revision=JS_GetPropertyStr(ctx,value,"ifRevision");
    if(JS_IsException(revision)) return JS_EXCEPTION;
    if(!JS_IsUndefined(revision)) {
        double r=0;
        bool bad=!JS_IsNumber(revision) || JS_ToFloat64(ctx,&r,revision);
        JS_FreeValue(ctx,revision);
        if(bad || !isfinite(r) || r!=(double)(int64_t)r || r<0 || r>UINT32_MAX)
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                     "ifRevision must be a revision number or 0",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        out->if_revision=(int64_t)r;
    } else JS_FreeValue(ctx,revision);
    return JS_UNDEFINED;
}

// ------------------------------------------------------------ JSON values
//
// JSON.stringify alone does not implement section 7's rule: it drops undefined
// properties, turns a function into a hole and writes NaN as null, all without
// complaint. The value is therefore walked first and refused whole if it holds
// anything the section excludes, and only then stringified.
//
// The walk reads the value the same way JSON.stringify will — own enumerable
// string properties — so the two agree, except for an accessor that answers
// differently on the second read. Section 3 already says this boundary limits
// capability rather than isolating hostile code, and a getter that lies to its
// own app is that kind of code.
static bool value_ok(JSContext *ctx, JSValueConst value, int depth, int *budget,
                     const char **why) {
    if(--*budget<0) { *why="value has too many parts"; return false; }
    if(depth>KV_MAX_DEPTH) { *why="value is nested too deeply, or circular"; return false; }

    if(JS_IsNull(value) || JS_IsBool(value) || JS_IsString(value)) return true;
    if(JS_IsNumber(value)) {
        double n=0;
        if(JS_ToFloat64(ctx,&n,value) || !isfinite(n)) {
            *why="numbers must be finite";
            return false;
        }
        return true;
    }
    if(JS_IsUndefined(value)) { *why="undefined cannot be stored"; return false; }
    if(JS_IsBigInt(value))    { *why="BigInt cannot be stored";    return false; }
    if(JS_IsFunction(ctx,value)) { *why="functions cannot be stored"; return false; }
    if(!JS_IsObject(value)) { *why="value must be JSON compatible"; return false; }

    if(JS_IsArray(value)) {
        int64_t length=0;
        if(JS_GetLength(ctx,value,&length)) { *why="array length is unreadable"; return false; }
        for(int64_t i=0;i<length;i++) {
            JSValue item=JS_GetPropertyUint32(ctx,value,(uint32_t)i);
            if(JS_IsException(item)) { *why="array element is unreadable"; return false; }
            bool ok=value_ok(ctx,item,depth+1,budget,why);
            JS_FreeValue(ctx,item);
            if(!ok) return false;
        }
        return true;
    }

    // Plain means Object.prototype or nothing. A Date, a Map or a class
    // instance stringifies into something that will not come back as itself, so
    // it is refused rather than quietly flattened.
    JSValue proto=JS_GetPrototype(ctx,value);
    bool plain=JS_IsNull(proto) ||
               (state && JS_IsStrictEqual(ctx,proto,state->object_proto));
    JS_FreeValue(ctx,proto);
    if(!plain) { *why="only plain objects can be stored"; return false; }

    JSPropertyEnum *props=NULL;
    uint32_t count=0;
    if(JS_GetOwnPropertyNames(ctx,&props,&count,value,
                              JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY)) {
        *why="object properties are unreadable";
        return false;
    }
    bool ok=true;
    for(uint32_t i=0;i<count && ok;i++) {
        JSValue item=JS_GetProperty(ctx,value,props[i].atom);
        if(JS_IsException(item)) { *why="object property is unreadable"; ok=false; break; }
        ok=value_ok(ctx,item,depth+1,budget,why);
        JS_FreeValue(ctx,item);
    }
    JS_FreePropertyEnum(ctx,props,count);
    return ok;
}

// ------------------------------------------------------------------- get

// Section 4 puts argument errors from a Promise-returning method into the
// rejection rather than a throw, so every exit from get/set/remove goes through
// pocket_api_reject().
//
// The three methods return a Promise that is already settled: the NVS read or
// write runs to completion on the JS task before the Promise leaves. Section 7
// asks that a success answer mean the value is durable, which this satisfies by
// construction, and section 5 requires every resolution to happen on the JS
// task, which a worker cannot do until the host owns a completion queue to hand
// results back through. The price is that a set() stalls the JS turn for the
// length of a flash write. That length has not been measured on this device;
// when a completion queue exists this is the surface to move behind it.
static JSValue js_get(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv) {
    (void)this_val;
    static const char OP[]="storage.get";
    char name[NVS_KEY_NAME_MAX_SIZE];
    JSValue bad=take_key(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,name);
    if(!JS_IsUndefined(bad)) return bad;
    kv_options_t options;
    bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the read",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(!store_open())
        return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,OP,
                                 "the key/value store is not open",true,
                                 POCKET_OUTCOME_NOT_APPLIED);

    size_t length=0;
    const char *key=JS_ToCStringLen(ctx,&length,argv[0]);
    if(!key) return JS_EXCEPTION;
    kv_record_t record;
    esp_err_t err=record_load(name,&record);
    bool mine=record_is(&record,key,length);
    JS_FreeCString(ctx,key);

    if(err==ESP_ERR_INVALID_SIZE) {
        record_free(&record);
        return pocket_api_reject(ctx,POCKET_ERR_CORRUPT_DATA,OP,
                                 "the stored record does not verify",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(err==ESP_ERR_NO_MEM) {
        record_free(&record);
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                 "no memory to read the record",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(err!=ESP_OK) {
        ESP_LOGW(TAG,"get: %s",esp_err_to_name(err));
        record_free(&record);
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,
                                 "the store could not be read",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    // A slot holding someone else's key is a hash collision, and for a reader
    // that is simply an absent key.
    if(!mine) { record_free(&record); return pocket_api_settled(ctx,JS_NULL,false); }

    // Section 7 keeps a stored null distinct from a missing key: this is the
    // record, so it answers with value:null rather than null.
    JSValue value=JS_ParseJSON(ctx,record.json,record.json_len,"storage");
    uint32_t revision=record.header.revision;
    record_free(&record);
    if(JS_IsException(value)) {
        JS_FreeValue(ctx,JS_GetException(ctx));
        return pocket_api_reject(ctx,POCKET_ERR_CORRUPT_DATA,OP,
                                 "the stored value is not JSON",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    JSValue result=JS_NewObject(ctx);
    if(JS_IsException(result)) { JS_FreeValue(ctx,value); return result; }
    JS_SetPropertyStr(ctx,result,"value",value);
    JS_SetPropertyStr(ctx,result,"revision",JS_NewUint32(ctx,revision));
    return pocket_api_settled(ctx,result,false);
}

// ------------------------------------------------------------------- set

static JSValue js_set(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv) {
    (void)this_val;
    static const char OP[]="storage.set";
    char name[NVS_KEY_NAME_MAX_SIZE];
    JSValue bad=take_key(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,name);
    if(!JS_IsUndefined(bad)) return bad;
    kv_options_t options;
    bad=take_options(ctx,argc>2?argv[2]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;

    JSValueConst value=argc>1?argv[1]:JS_UNDEFINED;
    const char *why="value must be JSON compatible";
    int budget=KV_MAX_NODES;
    if(!value_ok(ctx,value,0,&budget,&why)) {
        // An unreadable property leaves QuickJS's exception pending; the
        // rejection carries the reason, so the stale exception is cleared.
        if(JS_HasException(ctx)) JS_FreeValue(ctx,JS_GetException(ctx));
        return pocket_api_reject(ctx,
                                 budget<0?POCKET_ERR_LIMIT_EXCEEDED:POCKET_ERR_INVALID_ARGUMENT,
                                 OP,why,false,POCKET_OUTCOME_NOT_APPLIED);
    }

    JSValue json=JS_JSONStringify(ctx,value,JS_UNDEFINED,JS_UNDEFINED);
    if(JS_IsException(json)) {
        JS_FreeValue(ctx,JS_GetException(ctx));
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "value could not be encoded",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    size_t json_len=0;
    const char *text=JS_ToCStringLen(ctx,&json_len,json);
    JS_FreeValue(ctx,json);
    if(!text) return JS_EXCEPTION;
    if(json_len>KV_MAX_VALUE_BYTES) {
        JS_FreeCString(ctx,text);
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "the encoded value is over 4096 bytes",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(options.cancelled) {
        JS_FreeCString(ctx,text);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the write",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(!store_open()) {
        JS_FreeCString(ctx,text);
        return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,OP,
                                 "the key/value store is not open",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }

    size_t key_len=0;
    const char *key=JS_ToCStringLen(ctx,&key_len,argv[0]);
    if(!key) { JS_FreeCString(ctx,text); return JS_EXCEPTION; }

    // Declared before the first bail-out so that every exit runs the one
    // clean-up at `done`.
    JSValue     answer=JS_UNDEFINED, result=JS_UNDEFINED;
    kv_record_t old;
    kv_header_t header={.magic=KV_MAGIC,.key_len=(uint16_t)key_len,.revision=1};
    uint32_t    current=0;
    size_t      size=sizeof(kv_header_t)+key_len+json_len;
    uint8_t    *blob=NULL;
    esp_err_t   err=record_load(name,&old);
    if(err==ESP_ERR_INVALID_SIZE) {
        // A record that does not verify is overwritten rather than mourned: the
        // caller is replacing that key anyway, and refusing would leave the app
        // with a key it can neither read nor repair.
        ESP_LOGW(TAG,"replacing an unreadable record");
        record_free(&old);
        memset(&old,0,sizeof(old));
        err=ESP_OK;
    }
    if(err!=ESP_OK) {
        answer=pocket_api_reject(ctx,
                                 err==ESP_ERR_NO_MEM?POCKET_ERR_OUT_OF_MEMORY:POCKET_ERR_IO_ERROR,
                                 OP,"the store could not be read",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
        goto done;
    }
    if(old.blob && !record_is(&old,key,key_len)) {
        answer=pocket_api_reject(ctx,POCKET_ERR_CONFLICT,OP,
                                 "this key collides with another key already stored",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
        goto done;
    }
    // A missing record is revision 0, which gives ifRevision:0 the meaning
    // "create this key, and fail if someone else got there first".
    current=old.blob?old.header.revision:0;
    if(options.if_revision>=0 && (uint32_t)options.if_revision!=current) {
        answer=pocket_api_reject(ctx,POCKET_ERR_CONFLICT,OP,
                                 "the stored revision has moved on",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
        goto done;
    }

    if(!room_for(size,old.blob?old.size:0)) {
        answer=pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "the store has no room for this value",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
        goto done;
    }
    blob=malloc(size);
    if(!blob) {
        answer=pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                 "no memory to build the record",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
        goto done;
    }
    header.revision=current+1;
    memcpy(blob,&header,sizeof(header));
    memcpy(blob+sizeof(header),key,key_len);
    memcpy(blob+sizeof(header)+key_len,text,json_len);

    // NVS writes the replacement before it retires the old entry, so a power
    // cut between them leaves one whole value or the other — the atomic
    // replacement section 7 requires, without a second copy in RAM.
    err=nvs_set_blob(state->nvs,name,blob,size);
    if(err==ESP_OK) err=nvs_commit(state->nvs);
    free(blob);
    if(err!=ESP_OK) {
        ESP_LOGW(TAG,"set: %s",esp_err_to_name(err));
        bool full=err==ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        // nvs_set_blob either wrote the whole record or none of it, but a
        // failing commit leaves which one it was undecided.
        answer=pocket_api_reject(ctx,
                                 full?POCKET_ERR_LIMIT_EXCEEDED:POCKET_ERR_IO_ERROR,OP,
                                 full?"the store is full":"the value could not be written",
                                 !full,
                                 full?POCKET_OUTCOME_NOT_APPLIED:POCKET_OUTCOME_UNKNOWN);
        goto done;
    }
    result=JS_NewObject(ctx);
    if(JS_IsException(result)) { answer=result; goto done; }
    JS_SetPropertyStr(ctx,result,"revision",JS_NewUint32(ctx,header.revision));
    answer=pocket_api_settled(ctx,result,false);
done:
    record_free(&old);
    JS_FreeCString(ctx,key);
    JS_FreeCString(ctx,text);
    return answer;
}

// ---------------------------------------------------------------- remove

static JSValue js_remove(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val;
    static const char OP[]="storage.remove";
    char name[NVS_KEY_NAME_MAX_SIZE];
    JSValue bad=take_key(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,name);
    if(!JS_IsUndefined(bad)) return bad;
    kv_options_t options;
    bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the removal",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(!store_open())
        return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,OP,
                                 "the key/value store is not open",true,
                                 POCKET_OUTCOME_NOT_APPLIED);

    size_t key_len=0;
    const char *key=JS_ToCStringLen(ctx,&key_len,argv[0]);
    if(!key) return JS_EXCEPTION;
    kv_record_t record;
    esp_err_t err=record_load(name,&record);
    // Removing a key that is not there, or whose slot a collision gave to
    // someone else, has already achieved what the caller asked for. An
    // unreadable record is erased: the caller wanted the key gone.
    bool erase=(err==ESP_ERR_INVALID_SIZE) || record_is(&record,key,key_len);
    record_free(&record);
    JS_FreeCString(ctx,key);
    if(err!=ESP_OK && err!=ESP_ERR_INVALID_SIZE)
        return pocket_api_reject(ctx,
                                 err==ESP_ERR_NO_MEM?POCKET_ERR_OUT_OF_MEMORY:POCKET_ERR_IO_ERROR,
                                 OP,"the store could not be read",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(!erase) return pocket_api_settled(ctx,JS_UNDEFINED,false);

    err=nvs_erase_key(state->nvs,name);
    if(err==ESP_OK) err=nvs_commit(state->nvs);
    if(err!=ESP_OK && err!=ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG,"remove: %s",esp_err_to_name(err));
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,
                                 "the key could not be removed",true,
                                 POCKET_OUTCOME_UNKNOWN);
    }
    return pocket_api_settled(ctx,JS_UNDEFINED,false);
}

// ------------------------------------------------------------ capability

static const pocket_limit_t storage_limits[] = {
    {.name="maxKeyBytes",   .kind=POCKET_LIMIT_INT, .number=KV_MAX_KEY_BYTES},
    {.name="maxValueBytes", .kind=POCKET_LIMIT_INT, .number=KV_MAX_VALUE_BYTES},
    {.name="quotaBytes",    .kind=POCKET_LIMIT_INT, .number=KV_QUOTA_BYTES},
    {.name="maxDepth",      .kind=POCKET_LIMIT_INT, .number=KV_MAX_DEPTH},
    {.name="maxTimeoutMs",  .kind=POCKET_LIMIT_INT, .number=KV_MAX_TIMEOUT_MS},
    {.kind=POCKET_LIMIT_END},
};

// available is an observation, so the probe reports whether the namespace is
// open — and opens it, once, so that the first answer is the true one rather
// than a pessimistic guess.
static void storage_probe(const pocket_capability_t *capability, bool *available,
                          const char **reason) {
    (void)capability;
    *available=store_open();
    *reason=*available?NULL:POCKET_REASON_DISABLED;
}

static const pocket_capability_t storage_capability = {
    .name="storage.kv", .supported=true, .available=true,
    .limits=storage_limits, .probe=storage_probe,
};

// ---------------------------------------------------------------- install

static void hub_finalizer(JSRuntime *rt, JSValueConst value) {
    storage_state_t *st=JS_GetOpaque(value,hub_class);
    if(!st) return;
    JS_FreeValueRT(rt,st->object_proto);
    // The handle belongs to the session, not to the device: closing it here is
    // what keeps a run from leaving an NVS handle behind for the next one.
    if(st->open) nvs_close(st->nvs);
    if(state==st) state=NULL;
    free(st);
}

static void hub_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark) {
    storage_state_t *st=JS_GetOpaque(value,hub_class);
    if(st) JS_MarkValue(rt,st->object_proto,mark);
}

static const JSClassDef hub_class_def = {
    .class_name="PocketStorageHub", .finalizer=hub_finalizer, .gc_mark=hub_mark,
};

static JSValue object_prototype(JSContext *ctx) {
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue object=JS_GetPropertyStr(ctx,global,"Object");
    JSValue proto=JS_GetPropertyStr(ctx,object,"prototype");
    JS_FreeValue(ctx,object);
    JS_FreeValue(ctx,global);
    return proto;
}

// Everything here runs on the first read of pocket.storage: the class, the
// per-realm state and the three functions. An app that never stores anything
// pays for none of it.
static esp_err_t build_storage(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    JSRuntime *rt=JS_GetRuntime(ctx);
    JS_NewClassID(rt,&hub_class);
    if(JS_NewClass(rt,hub_class,&hub_class_def)<0) return ESP_FAIL;
    if(state) return ESP_ERR_INVALID_STATE;

    storage_state_t *st=calloc(1,sizeof(*st));
    if(!st) return ESP_ERR_NO_MEM;
    st->object_proto=JS_UNDEFINED;
    JSValue hub=JS_NewObjectClass(ctx,hub_class);
    if(JS_IsException(hub)) { free(st); return ESP_ERR_NO_MEM; }
    JS_SetOpaque(hub,st);
    state=st;
    st->object_proto=object_prototype(ctx);

    JS_DefinePropertyValueStr(ctx,ns,"get",
        JS_NewCFunction(ctx,js_get,"get",2),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"set",
        JS_NewCFunction(ctx,js_set,"set",3),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"remove",
        JS_NewCFunction(ctx,js_remove,"remove",2),JS_PROP_ENUMERABLE);
    // The hub hangs off the namespace so the realm owns the state and the
    // finalizer runs with it -- which is also what ends this state's life,
    // there being no pocket_storage_reset() to call.
    JS_DefinePropertyValueStr(ctx,ns,"__hub",hub,0);
    return ESP_OK;
}

esp_err_t pocket_storage_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    // Replaces the declared storage.kv entry in place; section 2's contract for
    // it goes from supported=false to a live capability with enforced limits.
    // Eager, so a feature test answers without building anything.
    esp_err_t err=pocket_api_register(&storage_capability);
    if(err!=ESP_OK) return err;
    return pocket_api_lazy(ctx,"storage",build_storage,NULL);
}
