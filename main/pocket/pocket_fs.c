#include "pocket_fs.h"
#include "pocket_api.h"
#include "srcstore.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include "sd_media.h"
#include "sd_picker.h"
#include "file_picker.h"

static const char *TAG = "pocket.fs";

// ---------------------------------------------------------------- the store
//
// There is no filesystem on this device and there is no room to introduce one.
// FATFS with wear levelling wants a partition of its own, and every partition
// in partitions.csv is spoken for: the SKK dictionary and the Japanese font are
// reserved by tools/check_flash.py, and `storage` already holds srcstore's 16
// source slots at a fixed offset that predates slots existing. Reformatting
// `storage` would take the person's saved programs with it, which section 10 of
// docs/filesystem-api.md names as the thing not to do.
//
// So app:/ is a small copy-on-write store written here, in the region behind
// srcstore's slots. One sector is one block; a block is a 24-byte header and up
// to 4072 bytes of payload; an object -- a file or a directory -- is one INODE
// block that names the sectors holding its data. Nothing is ever rewritten in
// place: a new version goes to free sectors and becomes visible the moment its
// inode lands. That single write is the commit point, and it is what makes
// atomicReplace true.
//
// The inode is what keeps the host out of DRAM. Because the block list lives on
// flash, RAM holds two 64-bit allocation bitmaps and six bytes per object -- 504
// bytes for the whole store -- and a lookup pays one ~120-byte read for the one
// candidate a name hash did not rule out. A design that cached names and block
// maps in RAM instead measured 3080 bytes and would have grown with the region;
// this one does not.
//
// What it deliberately is not: crash-safe. The design gives the property by
// construction, but section 6 asks for a power-cut test before the feature is
// claimed and this session had no board to cut power to. crashSafeReplace is
// therefore false. See the durability note further down for what that costs.

#define FS_SECTOR      4096u
// Just past srcstore. Its layout is private to srcstore.c, so the arithmetic is
// repeated here and asserted: a slot is two 12 KiB faces, sixteen slots is
// 384 KiB. If SRC_MAX ever grows past 8 KiB the assert fires and this offset
// moves, rather than the two stores quietly overlapping.
#define SRC_HDR_BYTES  16u
#define SRC_BLOCK      ((((SRC_HDR_BYTES+SRC_MAX+FS_SECTOR-1)/FS_SECTOR))*FS_SECTOR)
#define FS_BASE        ((uint32_t)(SRC_SLOT_COUNT*SRC_BLOCK*2))
#define FS_SECTORS     64u
#define FS_SIZE        (FS_SECTORS*FS_SECTOR)
_Static_assert(FS_BASE==0x60000u, "srcstore no longer ends where app:/ begins");

// ------------------------------------------------------------------ limits
//
// Every number below is checked before the operation touches flash. Section 2
// says a capability's limits are the build's hard limits, and CLAUDE.md is
// blunter: publish what the code enforces, not what the specification proposed.
// Where the two differ the deviation is spelled out.

#define FS_MAX_NAME     64      // section 2: one component, UTF-8 bytes
#define FS_MAX_PATH    256      // section 2: the whole path
#define FS_MAX_DEPTH     8      // section 2
#define FS_MAX_OBJECTS  24      // files plus directories, all owners together
#define FS_CHUNK      1024      // section 8: one read() or write()
#define FS_MAX_HANDLES   2      // section 8
#define FS_MAX_CURSORS   2      // section 4
#define FS_CURSOR_US 30000000   // section 4: 30s of no use expires a cursor
#define FS_LIST_DEFAULT  8
#define FS_LIST_MAX     16
#define FS_TEXT_MAX   8192      // section 7: readText/writeText
#define FS_MAX_TIMEOUT_MS 30000

#define FS_BLOCK_PAYLOAD (FS_SECTOR-sizeof(fs_blk_t))    // 4072

// Section 8 proposes a 64 KiB app quota and a maximum file of
// min(volume, quota headroom, 2GiB-1). The quota is kept, charged in whole
// sectors because whole sectors is what the flash rations: 16 sectors.
//
// The maximum file is NOT 2GiB-1 and is not the quota either. A replace holds
// the old version and the new one at once (section 6 requires exactly that),
// so a file has to fit twice inside the quota along with a metadata block each:
// 2*(ceil(N/4072)+1) <= 16 gives N <= 28504. 24576 is the round number under
// it, and it is what maxFileBytes publishes.
#define FS_QUOTA_SECTORS 16
#define FS_QUOTA_BYTES   (FS_QUOTA_SECTORS*FS_SECTOR)
#define FS_MAX_FILE      24576u
#define FS_MAX_BLOCKS    ((FS_MAX_FILE+FS_BLOCK_PAYLOAD-1)/FS_BLOCK_PAYLOAD)  // 7
// Left for the writes that empty the store: a remove has to write a tombstone,
// and a store with no free sector could not be emptied.
#define FS_RESERVE_SECTORS 4

// ------------------------------------------------------------ block format
//
// A block is one sector: a 24-byte header and up to 4072 bytes of payload. The
// header is written together with the payload and the CRC covers the payload,
// so a torn write is caught by arithmetic rather than by trusting that a magic
// number implies the bytes behind it.

#define FS_MAGIC   0x31534650u    // "PFS1"
#define FS_ERASED  0xffffffffu
#define FS_NO_SECTOR 0xffu

#define FS_KIND_DATA 0u
#define FS_KIND_FILE 1u
#define FS_KIND_DIR  2u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t owner;     // FNV-1a of the owning app id
    uint32_t gen;       // this object's version; the highest one is the live one
    uint16_t obj;       // object id, 1..FS_MAX_OBJECTS
    uint16_t index;     // 0 = inode, 1.. = data block index-1
    uint16_t len;       // payload bytes
    uint16_t kind;      // FS_KIND_*, FS_KIND_DATA on a data block
    uint32_t crc;       // esp_crc32_le of the payload
} fs_blk_t;
_Static_assert(sizeof(fs_blk_t)==24, "the block header must stay 4-byte tidy");

// The inode: the payload of a block with index 0, followed by the name.
//
// It NAMES the sectors holding its data. That one field is what keeps the host
// index small: with the block list on flash there is nothing in RAM that has to
// remember where a file's bytes are, and finding them is an array lookup rather
// than a scan. A sector number fits in a byte because the region is 64 sectors;
// growing it past 255 means widening this field and the format with it.
//
// It is also the whole of the commit protocol. Writing this block publishes a
// version: before it lands the new data sectors are referenced by nothing, and
// after it lands the old ones are. Recovery needs no journal and no ordering
// rule beyond "the highest gen for an object wins".
typedef struct __attribute__((packed)) {
    uint32_t size;              // file bytes; 0 for a directory
    int64_t  mtime_ms;          // Unix ms, or 0 when the clock could not be trusted
    uint16_t parent;            // containing directory, 0 for the volume root
    uint16_t name_len;
    uint16_t blocks;            // valid entries in sector[]
    uint16_t reserved;
    uint8_t  sector[FS_MAX_BLOCKS];
    uint8_t  pad;
} fs_inode_t;
_Static_assert(sizeof(fs_inode_t)==28, "the inode payload must stay packed");

#define FS_INODE_MAX (sizeof(fs_inode_t)+FS_MAX_NAME)

// ------------------------------------------------------------- the index
//
// What the host keeps in RAM, and deliberately no more:
//
//   two 64-bit words   which sectors are spoken for and which need an erase
//   six bytes per id   where the inode is, what it is, whose child it is, and
//                      16 bits of its name
//
// The six-byte digest can only ever say NO. A name matches when the hash and
// the parent match AND the inode read from flash agrees, which is the same
// hash-then-verify shape pocket_storage.c uses for its NVS keys: a hash is not
// a name, and the stored copy is what turns "this could be it" into "this is
// it". A lookup therefore costs one small flash read, not 64 bytes of DRAM per
// file for the whole run.
//
// A tiny window of decoded inodes sits behind object(), so the walk of a path
// or the building of a listing does not re-read the same inode. Any change to
// the store bumps `mutations`, which is also the window's epoch, so a stale
// entry cannot survive a write.

typedef struct {
    uint8_t  sector;    // the sector holding this object's inode
    uint8_t  kind;      // FS_KIND_FILE / FS_KIND_DIR; 0 when the id is free
    uint16_t parent;
    uint16_t hash;      // 16 bits of the name, enough to pick one candidate
} fs_dir_t;

// A decoded inode. Only ever lives in the window below or on a caller's stack.
typedef struct {
    uint32_t owner;
    uint32_t gen;
    uint32_t size;
    int64_t  mtime_ms;
    uint16_t parent;
    uint16_t blocks;
    uint8_t  kind;
    uint8_t  name_len;
    uint8_t  sector[FS_MAX_BLOCKS];
    char     name[FS_MAX_NAME];
} fs_obj_t;

#define FS_WINDOW 3

typedef struct {
    uint16_t id;        // 0 marks an empty slot
    uint32_t epoch;     // the mutation count this was read at
    fs_obj_t o;
} fs_win_t;

typedef struct {
    uint64_t used;      // one bit per sector: referenced, or held by a writer
    uint64_t dirty;     // one bit per sector: holds bytes, needs an erase first
    fs_dir_t dir[FS_MAX_OBJECTS];
    fs_win_t win[FS_WINDOW];
    uint32_t mutations;
    uint16_t mine;      // sectors the current owner holds, for the quota
    uint8_t  next_win;
} fs_index_t;

// The index is the only heap this surface holds, so its size is asserted rather
// than left to be discovered by an app that runs out of room. Growing
// FS_MAX_OBJECTS, FS_SECTORS or the window costs internal DRAM here and nowhere
// else, and the assert is what makes that visible at build time.
_Static_assert(sizeof(fs_index_t)==504, "the app: index changed size");

static const esp_partition_t *part;
static fs_index_t            *store;
static bool                   mount_failed;

// ------------------------------------------------------------------ owner

#define FS_DEFAULT_OWNER "local.default"
static char     owner_id[48] = FS_DEFAULT_OWNER;
static uint32_t owner_hash;

static uint32_t fnv1a(const void *data, size_t length) {
    const uint8_t *p=data;
    uint32_t hash=0x811c9dc5u;
    for(size_t i=0;i<length;i++) { hash^=p[i]; hash*=0x01000193u; }
    // 0 and the erased word are reserved for "no owner here", so a hash landing
    // on either is nudged rather than allowed to look like an empty sector.
    return (hash==0||hash==FS_ERASED)?1u:hash;
}

static uint16_t name_hash(const char *name, size_t length) {
    uint32_t h=fnv1a(name,length);
    return (uint16_t)(h^(h>>16));
}

void pocket_fs_set_owner(const char *app_id) {
    snprintf(owner_id,sizeof(owner_id),"%s",
             (app_id&&app_id[0])?app_id:FS_DEFAULT_OWNER);
    owner_hash=fnv1a(owner_id,strlen(owner_id));
}

// ------------------------------------------------------------ flash access

static uint32_t sector_at(unsigned s) { return FS_BASE+(uint32_t)s*FS_SECTOR; }

#define BIT(s)      (1ull<<(s))
#define IS_USED(s)  ((store->used&BIT(s))!=0)
#define IS_DIRTY(s) ((store->dirty&BIT(s))!=0)

static bool sector_erase(unsigned s) {
    if(esp_partition_erase_range(part,sector_at(s),FS_SECTOR)!=ESP_OK) {
        // Left dirty rather than free: a sector whose erase failed may hold
        // anything, and writing over it would produce a block that verifies
        // nowhere. It stays out of the allocator until an erase does succeed.
        ESP_LOGW(TAG,"erase of sector %u failed",s);
        store->used&=~BIT(s);
        store->dirty|=BIT(s);
        return false;
    }
    store->used&=~BIT(s);
    store->dirty&=~BIT(s);
    return true;
}

// Gives a sector back. Only ever called for a sector this owner holds, which is
// why the quota counter can be maintained without asking whose it was.
// True when the sector is genuinely gone. sector_erase() can fail, and for a
// removal that matters: the inode is then still on flash and still readable
// while the index says the object is not there, so it comes back at the next
// mount. Writes do not care -- a new inode with a higher generation wins over a
// stale one -- which is why only object_remove() reads this.
static bool sector_release(unsigned s) {
    if(s>=FS_SECTORS||!IS_USED(s)) return true;
    if(store->mine) store->mine--;
    return sector_erase(s);
}

// A sector to write into, erased and ready. Already-erased sectors go first
// because they need no erase; a dirty one costs the time a 4 KiB erase takes on
// this flash, which is why a writer only allocates when its staging buffer is
// actually full.
static int sector_take(void) {
    for(unsigned pass=0;pass<2;pass++)
        for(unsigned s=0;s<FS_SECTORS;s++) {
            if(IS_USED(s)) continue;
            if(pass==0&&IS_DIRTY(s)) continue;
            if(pass==1&&!sector_erase(s)) continue;
            store->used|=BIT(s);
            store->mine++;
            return (int)s;
        }
    return -1;
}

static unsigned sectors_free(void) {
    unsigned n=0;
    for(unsigned s=0;s<FS_SECTORS;s++) if(!IS_USED(s)) n++;
    return n;
}

// True when `want` more sectors may be taken: both the app's own quota and the
// floor that keeps a full store still emptiable.
static bool room_for(unsigned want) {
    if(store->mine+want>FS_QUOTA_SECTORS) return false;
    return sectors_free()>=want+FS_RESERVE_SECTORS;
}

// Writes one block into an already-erased sector.
static bool block_write(unsigned s, const fs_blk_t *hdr,
                        const void *payload, size_t len) {
    uint8_t head[sizeof(fs_blk_t)];
    memcpy(head,hdr,sizeof(head));
    if(esp_partition_write(part,sector_at(s),head,sizeof(head))!=ESP_OK) goto bad;
    if(len) {
        size_t body=len&~(size_t)3, tail=len-body;
        if(body&&esp_partition_write(part,sector_at(s)+sizeof(fs_blk_t),
                                     payload,body)!=ESP_OK) goto bad;
        if(tail) {
            // Flash writes want whole words; the last few bytes ride in a word
            // padded with the erased value so the rest of the sector is
            // untouched. srcstore.c does the same thing for the same reason.
            uint8_t word[4]={0xff,0xff,0xff,0xff};
            memcpy(word,(const uint8_t *)payload+body,tail);
            if(esp_partition_write(part,sector_at(s)+sizeof(fs_blk_t)+body,
                                   word,sizeof(word))!=ESP_OK) goto bad;
        }
    }
    return true;
bad:
    ESP_LOGW(TAG,"write to sector %u failed",s);
    store->dirty|=BIT(s);
    return false;
}

static bool block_header(unsigned s, fs_blk_t *out) {
    return esp_partition_read(part,sector_at(s),out,sizeof(*out))==ESP_OK;
}

// Confirms a block's payload against the CRC in its header without holding the
// payload: 256 bytes at a time, chaining the CRC. A reader verifies a block once
// when it first touches it, so a sequential read pays this per block rather than
// per read() -- and no reader needs a 4 KiB buffer of its own.
static bool block_verify(unsigned s, const fs_blk_t *hdr) {
    uint8_t piece[256];
    uint32_t crc=0, left=hdr->len, off=sector_at(s)+sizeof(fs_blk_t);
    while(left) {
        size_t n=left<sizeof(piece)?left:sizeof(piece);
        if(esp_partition_read(part,off,piece,n)!=ESP_OK) return false;
        crc=esp_crc32_le(crc,piece,n);
        off+=n; left-=(uint32_t)n;
    }
    return crc==hdr->crc;
}

// ------------------------------------------------------------------ inodes

// Reads and verifies the inode in sector `s`. This is the one flash read the
// small index trades DRAM for, and it is ~120 bytes.
static bool inode_read(unsigned s, fs_obj_t *out) {
    fs_blk_t hdr;
    if(s>=FS_SECTORS||!block_header(s,&hdr)) return false;
    if(hdr.magic!=FS_MAGIC||hdr.index!=0||hdr.len<sizeof(fs_inode_t)||
       hdr.len>FS_INODE_MAX||(hdr.kind!=FS_KIND_FILE&&hdr.kind!=FS_KIND_DIR))
        return false;
    uint8_t payload[FS_INODE_MAX];
    if(esp_partition_read(part,sector_at(s)+sizeof(hdr),payload,hdr.len)!=ESP_OK)
        return false;
    if(esp_crc32_le(0,payload,hdr.len)!=hdr.crc) return false;
    fs_inode_t node;
    memcpy(&node,payload,sizeof(node));
    if(node.name_len<1||node.name_len>FS_MAX_NAME||
       sizeof(node)+node.name_len!=hdr.len||node.blocks>FS_MAX_BLOCKS)
        return false;
    memset(out,0,sizeof(*out));
    out->owner=hdr.owner; out->gen=hdr.gen; out->kind=(uint8_t)hdr.kind;
    out->size=node.size; out->mtime_ms=node.mtime_ms; out->parent=node.parent;
    out->blocks=node.blocks; out->name_len=(uint8_t)node.name_len;
    memcpy(out->sector,node.sector,FS_MAX_BLOCKS);
    memcpy(out->name,payload+sizeof(node),node.name_len);
    return true;
}

// The decoded inode for a live object, through the window. NULL when the id is
// free or its inode no longer reads back.
//
// The pointer is valid until the next call: the window is three slots deep so
// that a path walk and a listing do not thrash, but a caller that needs two
// objects at once copies what it needs. Any mutation invalidates every slot.
static const fs_obj_t *object(uint16_t id) {
    if(id<1||id>FS_MAX_OBJECTS||!store) return NULL;
    fs_dir_t *d=&store->dir[id-1];
    if(!d->kind) return NULL;
    for(int i=0;i<FS_WINDOW;i++)
        if(store->win[i].id==id&&store->win[i].epoch==store->mutations)
            return &store->win[i].o;
    fs_win_t *w=&store->win[store->next_win];
    store->next_win=(uint8_t)((store->next_win+1)%FS_WINDOW);
    w->id=0;
    if(!inode_read(d->sector,&w->o)) {
        ESP_LOGW(TAG,"object %u has no readable inode",(unsigned)id);
        return NULL;
    }
    w->id=id;
    w->epoch=store->mutations;
    return &w->o;
}

// ------------------------------------------------------------------ mount
//
// One pass over 64 sector headers picks the newest inode for each object, and a
// second reads those inodes to learn which sectors they reference. Everything
// referenced becomes used; everything else that holds bytes becomes dirty and
// is erased only when the allocator needs it, so an interrupted write costs a
// sector until something asks for it and never costs correctness.
//
// There is no superblock on purpose. One would save this scan and cost a write
// to the same sector on every operation, which is a wear hotspot the size of
// this region does not justify.
static bool mount(void) {
    if(store) return true;
    if(mount_failed) return false;
    part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,0x42,"storage");
    if(!part||FS_BASE+FS_SIZE>part->size) {
        ESP_LOGW(TAG,"no room for the store in the storage partition");
        mount_failed=true;
        return false;
    }
    store=calloc(1,sizeof(*store));
    if(!store) { ESP_LOGW(TAG,"no memory for the index"); mount_failed=true; return false; }

    // Two candidates per object, not one. A commit writes the inode's header
    // first and its payload after, so a power cut in between leaves a sector
    // with the new generation and a CRC that matches nothing -- and the old,
    // intact inode is still on flash. Keeping only the newest turned that into
    // "the object was removed" and lost the whole file, which is worse than
    // either outcome atomicReplace promises. 48 bytes of stack.
    uint8_t  inode_at[FS_MAX_OBJECTS], inode_alt[FS_MAX_OBJECTS];
    uint32_t inode_gen[FS_MAX_OBJECTS], inode_alt_gen[FS_MAX_OBJECTS];
    memset(inode_at,FS_NO_SECTOR,sizeof(inode_at));
    memset(inode_alt,FS_NO_SECTOR,sizeof(inode_alt));
    memset(inode_gen,0,sizeof(inode_gen));
    memset(inode_alt_gen,0,sizeof(inode_alt_gen));

    for(unsigned s=0;s<FS_SECTORS;s++) {
        fs_blk_t hdr;
        if(!block_header(s,&hdr)) { store->dirty|=BIT(s); continue; }
        if(hdr.magic==FS_ERASED) continue;              // erased: free and clean
        store->dirty|=BIT(s);                           // until something claims it
        if(hdr.magic!=FS_MAGIC||hdr.obj<1||hdr.obj>FS_MAX_OBJECTS||
           hdr.len>FS_BLOCK_PAYLOAD||hdr.index!=0) continue;
        int i=hdr.obj-1;
        if(inode_at[i]==FS_NO_SECTOR||hdr.gen>=inode_gen[i]) {
            inode_alt[i]=inode_at[i]; inode_alt_gen[i]=inode_gen[i];
            inode_at[i]=(uint8_t)s;   inode_gen[i]=hdr.gen;
        } else if(inode_alt[i]==FS_NO_SECTOR||hdr.gen>=inode_alt_gen[i]) {
            inode_alt[i]=(uint8_t)s;  inode_alt_gen[i]=hdr.gen;
        }
    }

    unsigned live=0;
    for(int i=0;i<FS_MAX_OBJECTS;i++) {
        if(inode_at[i]==FS_NO_SECTOR) continue;
        fs_obj_t o;
        // A removal erases the inode, so an object with no readable inode at
        // all is an object that is gone. But an unreadable *newest* inode is a
        // commit that was cut in half, and the version before it is still here
        // and still whole -- that is the older-or-newer atomicReplace promises,
        // and reading only the newest delivered neither.
        uint8_t at=inode_at[i];
        if(!inode_read(at,&o)) {
            if(inode_alt[i]==FS_NO_SECTOR||!inode_read(inode_alt[i],&o)) continue;
            at=inode_alt[i];
            ESP_LOGW(TAG,"object %d: newest inode is torn, using generation %u",
                     i+1,(unsigned)inode_alt_gen[i]);
        }
        store->dir[i].sector=at;
        store->dir[i].kind=o.kind;
        store->dir[i].parent=o.parent;
        store->dir[i].hash=name_hash(o.name,o.name_len);
        store->used|=BIT(at);
        store->dirty&=~BIT(at);
        if(o.owner==owner_hash) store->mine++;
        for(unsigned b=0;b<o.blocks;b++) {
            unsigned s=o.sector[b];
            if(s>=FS_SECTORS) continue;
            fs_blk_t hdr;
            // The inode owns the sector either way; a header that disagrees is
            // logged and the read of that block will fail its CRC, which is a
            // more useful answer than making the whole file disappear.
            if(!block_header(s,&hdr)||hdr.magic!=FS_MAGIC||hdr.obj!=i+1||
               hdr.index!=b+1)
                ESP_LOGW(TAG,"object %d block %u points at a stranger",i+1,b+1);
            store->used|=BIT(s);
            store->dirty&=~BIT(s);
            if(o.owner==owner_hash) store->mine++;
        }
        live++;
    }
    ESP_LOGI(TAG,"app:/ mounted at 0x%06x: %u objects, %u/%u sectors free, "
             "index %u bytes",(unsigned)FS_BASE,live,sectors_free(),
             (unsigned)FS_SECTORS,(unsigned)sizeof(fs_index_t));
    return true;
}

// -------------------------------------------------------------------- paths
//
// Section 2 of docs/filesystem-api.md, enforced literally. No percent decoding,
// no Unicode normalisation, no `.` or `..`: "%2e%2e" is a name of six
// characters here and cannot climb out of a volume, which is the whole point.

#define VOL_APP    0
#define VOL_ASSETS 1
#define VOL_SD     2

typedef struct {
    uint8_t  volume;
    uint8_t  depth;
    uint16_t len;
    char     text[FS_MAX_PATH+1];       // normalised, one trailing slash gone
    uint16_t off[FS_MAX_DEPTH];         // component starts inside text
    uint8_t  size[FS_MAX_DEPTH];
} fs_path_t;

// Section 4 refuses malformed UTF-8 and lone surrogates at a text API, and a
// path is one. QuickJS hands back WTF-8 for an unpaired surrogate, so checking
// the encoded bytes catches both at once. Lifted from pocket_storage.c, which
// found this first.
static bool utf8_valid(const uint8_t *s, size_t n) {
    for(size_t i=0;i<n;) {
        uint8_t c=s[i];
        size_t extra; uint32_t cp;
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
        if(extra==1&&cp<0x80) return false;
        if(extra==2&&cp<0x800) return false;
        if(extra==3&&cp<0x10000) return false;
        if(cp>0x10ffff) return false;
        if(cp>=0xd800&&cp<=0xdfff) return false;
        i+=extra+1;
    }
    return true;
}

// The portable-name rule of section 2: no control characters, no backslash,
// none of <>:"|?*, and no name ending in a space or a period. Those last two
// are what a PC filesystem silently rewrites, so a name carrying one is refused
// here rather than becoming a different name on an SD card later.
static bool name_ok(const char *s, size_t n) {
    if(n<1||n>FS_MAX_NAME) return false;
    if(n==1&&s[0]=='.') return false;
    if(n==2&&s[0]=='.'&&s[1]=='.') return false;
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)s[i];
        if(c<0x20||c==0x7f) return false;
        if(c=='\\'||c=='/'||c=='<'||c=='>'||c==':'||c=='"'||
           c=='|'||c=='?'||c=='*') return false;
    }
    if(s[n-1]==' '||s[n-1]=='.') return false;
    return utf8_valid((const uint8_t *)s,n);
}

static const char *VOLUME_NAME[3]={"app","assets","sd"};

// Parses an absolute virtual path. Returns NULL on success, or the reason the
// caller should put in an INVALID_ARGUMENT.
static const char *path_parse(const char *text, size_t len, fs_path_t *out) {
    if(len<1||len>FS_MAX_PATH) return "path must be 1 to 256 UTF-8 bytes";
    const char *colon=memchr(text,':',len);
    if(!colon) return "path must start with a volume, as in app:/";
    size_t scheme=(size_t)(colon-text);
    out->volume=0xff;
    for(int v=0;v<3;v++)
        if(scheme==strlen(VOLUME_NAME[v])&&!memcmp(text,VOLUME_NAME[v],scheme))
            out->volume=(uint8_t)v;
    if(out->volume==0xff) return "unknown volume";
    if(scheme+1>=len||text[scheme+1]!='/') return "path must be absolute";

    size_t i=scheme+2;
    // Root aside, exactly one trailing slash is allowed and is removed.
    if(len>i&&text[len-1]=='/') len--;
    out->depth=0;
    while(i<len) {
        size_t start=i;
        while(i<len&&text[i]!='/') i++;
        size_t n=i-start;
        if(n==0) return "path has an empty element";
        if(out->depth>=FS_MAX_DEPTH) return "path is deeper than 8 elements";
        if(!name_ok(text+start,n)) return "path element is not a portable name";
        out->off[out->depth]=(uint16_t)start;
        out->size[out->depth]=(uint8_t)n;
        out->depth++;
        if(i<len) i++;                  // step over the separator
    }
    memcpy(out->text,text,len);
    out->text[len]=0;
    out->len=(uint16_t)len;
    return NULL;
}

static const char *path_leaf(const fs_path_t *p, size_t *len) {
    if(!p->depth) { *len=0; return NULL; }
    *len=p->size[p->depth-1];
    return p->text+p->off[p->depth-1];
}

// --------------------------------------------------------- object lookup

// The digest narrows the field without touching flash; the inode decides.
// Kind and parent are exact, the hash is 16 bits, so the usual cost of a lookup
// is one inode read for the one candidate that survives.
static uint16_t child_of(uint16_t parent, const char *name, size_t n) {
    uint16_t want=name_hash(name,n);
    for(int i=0;i<FS_MAX_OBJECTS;i++) {
        const fs_dir_t *d=&store->dir[i];
        if(!d->kind||d->parent!=parent||d->hash!=want) continue;
        const fs_obj_t *o=object((uint16_t)(i+1));
        // Byte comparison of the stored name, which is what caseSensitive:true
        // reports: nothing here folds case or normalises. The owner is checked
        // in the same read, which is what keeps the shared root from letting
        // one app see another's files.
        if(o&&o->owner==owner_hash&&o->name_len==n&&!memcmp(o->name,name,n))
            return (uint16_t)(i+1);
    }
    return 0;
}

// Walks a parsed app:/ path. `stop_short` leaves the last element unresolved
// and reports the parent, which is what create, mkdir and rename need.
// Returns 0 when something on the way is missing or is not a directory;
// *why is then the error code to reject with.
static uint16_t resolve(const fs_path_t *p, bool stop_short, uint16_t *parent,
                        const char **why) {
    // stop_short on the root would ask for the parent of the volume itself.
    // Callers reject a rootless operation before getting here.
    unsigned last=(stop_short&&p->depth)?(unsigned)p->depth-1u:p->depth;
    uint16_t at=0, prev=0;
    *why=NULL;
    for(unsigned i=0;i<last;i++) {
        uint16_t next=child_of(at,p->text+p->off[i],p->size[i]);
        if(!next) { *why=POCKET_ERR_NOT_FOUND; return 0; }
        // Every element but the last one named has to be a directory; the last
        // one is whatever the caller asked about. The digest answers this, so
        // an intermediate element costs no second read.
        if(store->dir[next-1].kind!=FS_KIND_DIR&&i+1<last)
            { *why="NOT_DIRECTORY"; return 0; }
        prev=at; at=next;
    }
    if(stop_short) {
        // `at` is now the directory the leaf would live in.
        if(at&&store->dir[at-1].kind!=FS_KIND_DIR) { *why="NOT_DIRECTORY"; return 0; }
        if(parent) *parent=at;
        return 0;
    }
    if(parent) *parent=prev;
    return at;
}

// ----------------------------------------------------------- the assets

// The firmware already carries its bundled JavaScript in flash, mapped and
// addressable, so assets:/ serves those bytes where they lie: no copy, no
// second budget, and a read costs exactly the bytes the app asked for.
// EMBED_TXTFILES appends a NUL that is not part of the file, hence the -1.
extern const char asset_hello_start[] asm("_binary_main_js_start");
extern const char asset_hello_end[]   asm("_binary_main_js_end");
extern const char asset_imucal_start[] asm("_binary_imucal_js_start");
extern const char asset_imucal_end[]   asm("_binary_imucal_js_end");

// `text` is not decoration: EMBED_TXTFILES appends a NUL and EMBED_FILES does
// not, and asset_size() has to know which. It is recorded per row rather than
// guessed from the extension, because the cost of guessing wrong on a binary is
// one lost byte at the very end -- the quietest possible failure.
typedef struct { const char *name; const char *start, *end; bool text; } fs_asset_t;
static const fs_asset_t ASSETS[]={
    {"hello.js",  asset_hello_start,  asset_hello_end,  true},
    {"imucal.js", asset_imucal_start, asset_imucal_end, true},
    // chime.wav/chime.pok/tone.pok and the three test-*.mp3 fixtures that used
    // to live here were the dev/test rows for apps/streamplay, apps/opusplay,
    // apps/opusfit and apps/mp3play -- all four apps and their assets were
    // removed once the MP3/Opus decoders they exercised were verified
    // (docs/common-api.md 9.1-9.1.3 keeps the measurements they produced).
    // apps/player is the shipped feature that uses pocket.audio.player now,
    // and it supplies its own sources rather than reading from assets:/.
};
#define ASSET_COUNT (sizeof(ASSETS)/sizeof(ASSETS[0]))

static int asset_find(const fs_path_t *p) {
    if(p->depth!=1) return -1;
    size_t n=0;
    const char *leaf=path_leaf(p,&n);
    for(unsigned i=0;i<ASSET_COUNT;i++)
        if(strlen(ASSETS[i].name)==n&&!memcmp(ASSETS[i].name,leaf,n)) return (int)i;
    return -1;
}

static uint32_t asset_size(int i) {
    size_t n=(size_t)(ASSETS[i].end-ASSETS[i].start);
    // Drop the terminator EMBED_TXTFILES adds, and ONLY then: the last byte of
    // a WAV is a sample, not a NUL, and subtracting it would truncate every
    // binary asset by one byte for as long as nobody looked at the end of one.
    if(!ASSETS[i].text) return (uint32_t)n;
    return n?(uint32_t)(n-1):0;
}

// ------------------------------------------------------------- the clock
//
// Section 4 asks for a null rather than a wrong timestamp. This applies the
// rule solar_time.c uses without depending on its in-RAM flag: the chip powers
// up at the Unix epoch, so a clock that is past 2001 was set by something and is
// worth recording, and one that is not is simply unknown.
static int64_t wall_ms(void) {
    struct timeval tv;
    if(gettimeofday(&tv,NULL)) return 0;
    if(tv.tv_sec<1000000000) return 0;
    return (int64_t)tv.tv_sec*1000+(int64_t)(tv.tv_usec/1000);
}

// -------------------------------------------------------------- publishing

// An id claimed by an open writer. The digest still says free, so an
// uncommitted create is invisible to stat and list; this only stops a second
// writer from taking the same id.
static uint8_t pending[FS_MAX_OBJECTS];

static uint16_t object_claim(void) {
    for(int i=0;i<FS_MAX_OBJECTS;i++)
        if(!store->dir[i].kind&&!pending[i]) { pending[i]=1; return (uint16_t)(i+1); }
    return 0;
}

static void object_unclaim(uint16_t id) { pending[id-1]=0; }

// Writes the inode that publishes a version, then gives back whatever the
// previous version used and this one does not.
//
// The write of this one block is the commit point of the whole store. Until it
// lands, the new data sectors are named by no inode and a reader still finds
// the old version whole; after it lands the old sectors are the unreferenced
// ones. That is atomicReplace, and it needs no journal because the inode is the
// journal.
static bool inode_write(uint16_t id, uint16_t kind, uint16_t parent,
                        const char *name, size_t name_len, uint32_t gen,
                        uint32_t size, const uint8_t *sectors, uint16_t blocks) {
    fs_obj_t old;
    uint8_t  old_at=store->dir[id-1].sector;
    bool     had=store->dir[id-1].kind&&inode_read(old_at,&old);

    uint8_t    payload[FS_INODE_MAX];
    fs_inode_t node={.size=size,.mtime_ms=wall_ms(),.parent=parent,
                     .name_len=(uint16_t)name_len,.blocks=blocks};
    memset(node.sector,FS_NO_SECTOR,sizeof(node.sector));
    if(sectors&&blocks) memcpy(node.sector,sectors,blocks);
    memcpy(payload,&node,sizeof(node));
    memcpy(payload+sizeof(node),name,name_len);
    size_t len=sizeof(node)+name_len;

    int s=sector_take();
    if(s<0) return false;
    fs_blk_t hdr={.magic=FS_MAGIC,.owner=owner_hash,.gen=gen,.obj=id,.index=0,
                  .len=(uint16_t)len,.kind=kind,
                  .crc=esp_crc32_le(0,payload,len)};
    if(!block_write((unsigned)s,&hdr,payload,len)) {
        sector_release((unsigned)s);
        return false;
    }

    store->dir[id-1]=(fs_dir_t){.sector=(uint8_t)s,.kind=(uint8_t)kind,
                                .parent=parent,.hash=name_hash(name,name_len)};
    store->mutations++;     // also the window epoch, so no stale inode survives

    if(had) {
        for(unsigned b=0;b<old.blocks;b++) {
            bool still=false;
            for(unsigned k=0;k<blocks&&!still;k++) still=sectors[k]==old.sector[b];
            if(!still) sector_release(old.sector[b]);
        }
        sector_release(old_at);
    }
    return true;
}

// Retires an object. Removal is the erase of its inode and nothing else: a data
// block no inode names is unreferenced, and mount() hands it back to the
// allocator. A crash during the erase leaves either an inode that still reads
// back -- the file survives -- or one that does not -- the file is gone. There
// is no third state, so there is no tombstone to write and none to reclaim.
// False when the inode's own erase failed. The index is cleared either way --
// the allocator retries the erase when it needs the sector, so the object may
// vanish later -- and the caller reports that with outcome "unknown" rather
// than a resolved promise, because neither answer is knowable from here.
// A data block that would not erase is not reported: those are reclaimed
// lazily and a stale one is unreachable without its inode.
static bool object_remove(uint16_t id) {
    fs_obj_t o;
    uint8_t  at=store->dir[id-1].sector;
    bool     had=inode_read(at,&o);
    bool     gone=sector_release(at);
    store->dir[id-1]=(fs_dir_t){0};
    store->mutations++;
    if(had) for(unsigned b=0;b<o.blocks;b++) sector_release(o.sector[b]);
    return gone;
}

// Parent chains never cross owners: a lookup only ever returns an id whose
// inode named this owner, so nothing but the shared root can hold another
// app's children, and the root is never asked here.
static bool dir_has_children(uint16_t id) {
    for(int i=0;i<FS_MAX_OBJECTS;i++)
        if(store->dir[i].kind&&store->dir[i].parent==id) return true;
    return false;
}

// ------------------------------------------------------------- JS helpers

typedef struct { bool cancelled; } fs_options_t;

// timeoutMs and cancel, checked exactly as pocket_storage.c checks them:
// section 4 refuses to round an out-of-range deadline quietly, and cancel is
// observed once because that is the only moment a synchronous operation has.
static JSValue take_options(JSContext *ctx, JSValueConst value,
                            const char *op, fs_options_t *out) {
    out->cancelled=false;
    if(JS_IsUndefined(value)||JS_IsNull(value)) return JS_UNDEFINED;
    if(!JS_IsObject(value))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                 "options must be an object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValue timeout=JS_GetPropertyStr(ctx,value,"timeoutMs");
    if(JS_IsException(timeout)) return JS_EXCEPTION;
    if(!JS_IsUndefined(timeout)) {
        double ms=0;
        bool bad=!JS_IsNumber(timeout)||JS_ToFloat64(ctx,&ms,timeout);
        JS_FreeValue(ctx,timeout);
        if(bad||!isfinite(ms)||ms!=(double)(int64_t)ms||ms<1||ms>FS_MAX_TIMEOUT_MS)
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                     "timeoutMs must be a whole number of 1 to 30000",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
    } else JS_FreeValue(ctx,timeout);

    JSValue cancel=JS_GetPropertyStr(ctx,value,"cancel");
    if(JS_IsException(cancel)) return JS_EXCEPTION;
    if(!JS_IsUndefined(cancel)&&!JS_IsNull(cancel)) {
        if(!pocket_api_is_cancel_token(cancel)) {
            JS_FreeValue(ctx,cancel);
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                     "cancel must be a token from pocket.cancel.source()",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        }
        out->cancelled=pocket_api_cancel_requested(cancel);
    }
    JS_FreeValue(ctx,cancel);
    return JS_UNDEFINED;
}

// Reads a string option, rejecting when it is present but not a string.
// Returns false with *bad holding the rejection the caller should return.
static bool take_string(JSContext *ctx, JSValueConst options, const char *key,
                        const char *op, char *out, size_t cap, JSValue *bad) {
    *bad=JS_UNDEFINED;
    out[0]=0;
    if(!JS_IsObject(options)) return true;
    JSValue v=JS_GetPropertyStr(ctx,options,key);
    if(JS_IsException(v)) { *bad=JS_EXCEPTION; return false; }
    if(JS_IsUndefined(v)||JS_IsNull(v)) { JS_FreeValue(ctx,v); return true; }
    if(!JS_IsString(v)) {
        JS_FreeValue(ctx,v);
        char why[64];
        snprintf(why,sizeof(why),"%s must be a string",key);
        *bad=pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,why,false,
                               POCKET_OUTCOME_NOT_APPLIED);
        return false;
    }
    const char *s=JS_ToCString(ctx,v);
    JS_FreeValue(ctx,v);
    if(!s) { *bad=JS_EXCEPTION; return false; }
    snprintf(out,cap,"%s",s);
    JS_FreeCString(ctx,s);
    return true;
}

// A whole-number option in [lo,hi], or `fallback` when absent.
static bool take_int(JSContext *ctx, JSValueConst options, const char *key,
                     const char *op, int64_t lo, int64_t hi, int64_t fallback,
                     int64_t *out, JSValue *bad) {
    *bad=JS_UNDEFINED;
    *out=fallback;
    if(!JS_IsObject(options)) return true;
    JSValue v=JS_GetPropertyStr(ctx,options,key);
    if(JS_IsException(v)) { *bad=JS_EXCEPTION; return false; }
    if(JS_IsUndefined(v)||JS_IsNull(v)) { JS_FreeValue(ctx,v); return true; }
    double n=0;
    bool wrong=!JS_IsNumber(v)||JS_ToFloat64(ctx,&n,v);
    JS_FreeValue(ctx,v);
    if(wrong||!isfinite(n)||n!=(double)(int64_t)n||(int64_t)n<lo||(int64_t)n>hi) {
        char why[96];
        snprintf(why,sizeof(why),"%s must be a whole number of %lld to %lld",
                 key,(long long)lo,(long long)hi);
        *bad=pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,why,false,
                               POCKET_OUTCOME_NOT_APPLIED);
        return false;
    }
    *out=(int64_t)n;
    return true;
}

// A path argument, parsed and pointed at a volume this build serves.
static JSValue take_path(JSContext *ctx, JSValueConst value, const char *op,
                         fs_path_t *out) {
    if(!JS_IsString(value))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                 "path must be a string",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    size_t len=0;
    const char *text=JS_ToCStringLen(ctx,&len,value);
    if(!text) return JS_EXCEPTION;
    const char *why=path_parse(text,len,out);
    JS_FreeCString(ctx,text);
    if(why)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,why,false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(out->volume==VOL_SD) {
        // The grant is tested before the media state, and the order is a
        // contract rather than a preference: section 8 asks an unauthorised
        // area to answer PERMISSION_DENIED without revealing what is there, and
        // checking the card first would let an ungranted app tell a mounted
        // card from an empty slot by which error came back.
        const sd_media_t *m=sd_media();
        if(!m->granted)
            return pocket_api_reject(ctx,POCKET_ERR_PERMISSION_DENIED,op,
                                     "no folder on sd: has been granted to this app",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        if(m->state!=SD_MEDIA_READY)
            return pocket_api_reject(ctx,POCKET_ERR_DISCONNECTED,op,
                                     "the card is not mounted",true,
                                     POCKET_OUTCOME_NOT_APPLIED);
    }
    if(out->volume==VOL_APP&&!mount())
        return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,op,
                                 "the app: store could not be mounted",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    return JS_UNDEFINED;
}

// Opaque to the app, and section 4 asks only that it be stable within a mount
// generation. Object id plus version is exactly that and nothing more.
static void revision_of(char out[24], uint16_t obj, uint32_t gen) {
    snprintf(out,24,"%u.%lu",(unsigned)obj,(unsigned long)gen);
}

// The revision of an object whose inode may have become unreadable. Such an
// inode matches no revision an app is holding, so 0 is the honest answer and
// the CONFLICT the caller then reports is the right one.
static void revision_of_object(char out[24], uint16_t id) {
    const fs_obj_t *o=object(id);
    revision_of(out,id,o?o->gen:0);
}

static JSValue entry_new(JSContext *ctx, const char *path, const char *name,
                         size_t name_len, bool directory, int64_t size,
                         int64_t mtime, const char *revision, bool accessible) {
    JSValue e=JS_NewObject(ctx);
    if(JS_IsException(e)) return e;
    JS_SetPropertyStr(ctx,e,"name",JS_NewStringLen(ctx,name,name_len));
    JS_SetPropertyStr(ctx,e,"path",JS_NewString(ctx,path));
    JS_SetPropertyStr(ctx,e,"kind",JS_NewString(ctx,directory?"directory":"file"));
    JS_SetPropertyStr(ctx,e,"sizeBytes",directory?JS_NULL:JS_NewInt64(ctx,size));
    JS_SetPropertyStr(ctx,e,"modifiedUnixMs",
                      mtime>0?JS_NewInt64(ctx,mtime):JS_NULL);
    JS_SetPropertyStr(ctx,e,"revision",JS_NewString(ctx,revision));
    // Section 2: a name this API could not have created is LISTED and marked
    // rather than hidden or rewritten -- an app that cannot see a file cannot
    // explain why it cannot open it. Everything app: and assets: hold passed
    // name_ok() on the way in, so only the card ever answers false here.
    JS_SetPropertyStr(ctx,e,"accessible",JS_NewBool(ctx,accessible));
    return e;
}

// Builds "<directory path>/<name>", which is what Entry.path has to be.
static void path_join(char out[FS_MAX_PATH+1], const fs_path_t *dir,
                      const char *name, size_t name_len) {
    size_t base=dir->len;
    // A root path already ends in its slash: "app:/" + "a", not "app://a".
    bool slash=dir->depth>0;
    if(base+(slash?1u:0u)+name_len>FS_MAX_PATH) { out[0]=0; return; }
    memcpy(out,dir->text,base);
    if(slash) out[base++]='/';
    memcpy(out+base,name,name_len);
    out[base+name_len]=0;
}

// Rebuilds the virtual path of an app: object by walking parents to the root.
// A path can always be rebuilt because every object names its parent, which is
// also why a rename costs one metadata block and no walk of the subtree.
//
// Built right to left, copying each name as it is read: object() hands back a
// window slot that the next call may reuse, so holding a pointer across the
// walk would be holding a name that has already been replaced.
static void object_path(uint16_t id, char out[FS_MAX_PATH+1]) {
    char   tail[FS_MAX_PATH+2];
    size_t end=sizeof(tail)-1;
    tail[end]=0;
    for(uint16_t at=id,step=0;at&&step<FS_MAX_DEPTH;step++) {
        const fs_obj_t *o=object(at);
        if(!o) break;
        uint16_t parent=o->parent;      // read before the window moves on
        size_t   n=o->name_len;
        if(end<n+1) break;
        end-=n;
        memcpy(tail+end,o->name,n);
        tail[--end]='/';
        at=parent;
    }
    snprintf(out,FS_MAX_PATH+1,"app:%s",tail+end);
}

// The Entry for an app: object, given the path it was reached by.
static JSValue entry_of_object(JSContext *ctx, const char *path, uint16_t id) {
    const fs_obj_t *o=object(id);
    if(!o) return JS_NULL;
    char rev[24];
    revision_of(rev,id,o->gen);
    return entry_new(ctx,path,o->name,o->name_len,o->kind==FS_KIND_DIR,
                     o->size,o->mtime_ms,rev,true);
}

// ------------------------------------------------------------- file handles
//
// Two, per section 8. A handle is a slot plus a handle number, so a wrapper the
// app kept after close() finds no slot and answers CLOSED rather than reaching
// a slot that has since been reused. That is the shape pocket_ui.c settled on
// for its nodes, and the reason is the same.

// An sd: writer's two names for the same file. The temporary it stages into is
// not here: it is this path plus SD_TEMP_SUFFIX, rebuilt on the stack at the
// three moments that need it, because a second copy would be a second thing to
// keep in step.
typedef struct {
    char fspath[SD_FSPATH_MAX];   // the FatFs path the bytes will land at
    char vpath[FS_MAX_PATH+1];    // the app's name for it, for the Entry
} sd_writer_t;

#define MODE_READ    0
#define MODE_CREATE  1
#define MODE_REPLACE 2
#define MODE_APPEND  3

typedef struct {
    uint32_t handle;        // 0 marks a free slot
    uint8_t  mode;
    uint8_t  volume;
    int8_t   asset;         // index into ASSETS, -1 on app:
    uint16_t obj;           // app: object, claimed even by an uncommitted create
    uint16_t parent;
    uint8_t  name_len;
    char     name[FS_MAX_NAME];
    uint32_t pos;           // tell(): the byte position of the last finished I/O
    uint32_t size;          // read: the file size; writers: bytes accepted
    uint32_t gen;           // the generation a writer is building
    uint16_t next_index;    // the data block the staging buffer belongs to
    uint8_t *buf;           // FS_BLOCK_PAYLOAD of staging; writers only
    uint16_t buf_len;
    // The block list, copied from the inode at open and republished at commit.
    // A reader holds it so that read() is an array lookup and no flash read at
    // all beyond the bytes themselves; a writer builds it as blocks land.
    uint8_t  sector[FS_MAX_BLOCKS];
    // The sectors THIS writer allocated and has not published. An append starts
    // with a sector list it does not own -- those blocks belong to the version
    // on flash -- so discarding has to erase these and only these.
    uint8_t  own[FS_MAX_BLOCKS+1];
    uint8_t  nown;
    int16_t  verified;      // sector whose CRC a reader has already checked
    bool     broken;        // a failed write refuses everything afterwards
    // sd: only. The handle carries the media generation it was opened under, so
    // a card swapped underneath it is detectable rather than merely unlucky --
    // section 3 requires that a removal invalidate handles and that reinsertion
    // not revive them, and a generation compare is how that is enforced at the
    // one place it matters, which is the next use of the handle.
    FILE    *sd;
    uint32_t sd_gen;
    // A digest of the FatFs path, for the exclusion rule of section 5. The
    // handle has no room for 320 bytes of path and does not need them: a
    // collision costs a spurious BUSY, which fails closed, while storing the
    // path would cost 640 bytes of internal DRAM for every app that never
    // touches a card.
    uint32_t sd_key;
    // Where an sd: writer will publish, and what the app calls that place.
    // Allocated when such a handle opens and freed when it closes, so a reader
    // -- and an app that never touches a card -- pays a pointer for it. 577
    // bytes of heap while a write is in flight, against 640 bytes of permanent
    // internal DRAM if the paths lived in this struct.
    sd_writer_t *sd_w;
} fs_file_t;

static fs_file_t files[FS_MAX_HANDLES];
static uint32_t  next_handle=1;
static JSClassID file_class;
// Which runtime file_class belongs to. Cleared by pocket_fs_reset() as well as
// compared against, because the allocator can hand the next session a runtime
// at the same address -- and skipping registration there would leave the class
// unregistered in a realm about to use it.
static JSRuntime *class_rt;

static fs_file_t *file_of(uint32_t handle) {
    if(!handle) return NULL;
    for(int i=0;i<FS_MAX_HANDLES;i++)
        if(files[i].handle==handle) return &files[i];
    return NULL;
}

// Erases whatever a writer has staged in flash but not published. Safe to run
// on a reader (it owns no sectors) and safe to run twice.
static void writer_discard(fs_file_t *f) {
    while(f->nown) sector_release(f->own[--f->nown]);
}

static void file_close(fs_file_t *f, bool discard) {
    if(!f->handle) return;
    if(discard) writer_discard(f);
    if(f->volume==VOL_SD) {
        // THIS LOOKS LIKE A LEAK AND IS NOT. The FILE* is touched only while
        // the card that made it is still the card in the slot. After a removal the FatFs mount is gone and the fd
        // belongs to a VFS that has been unregistered, so closing it would be a
        // call into a driver that no longer exists; the handle is dropped
        // instead, and what it held went with the mount.
        bool live=sd_generation_valid(sd_media(),f->sd_gen);
        if(f->sd&&live) fclose(f->sd);
        f->sd=NULL;
        // Section 5: an uncommitted create or replace loses its temporary
        // version here, which is what makes those modes safe to abandon.
        if(discard&&live&&f->sd_w&&f->mode!=MODE_READ&&f->mode!=MODE_APPEND) {
            char temp[SD_FSPATH_MAX+sizeof(SD_TEMP_SUFFIX)];
            snprintf(temp,sizeof temp,"%s%s",f->sd_w->fspath,SD_TEMP_SUFFIX);
            unlink(temp);
        }
        free(f->sd_w);
    }
    if(f->mode==MODE_CREATE&&f->obj&&pending[f->obj-1]) object_unclaim(f->obj);
    free(f->buf);
    memset(f,0,sizeof(*f));
}

// Section 5: readers may share a file, a writer may not share it with anyone.
// The check is by object, and by (parent,name) for a create whose object does
// not exist yet, so two creates of the same new name also collide.
static bool file_busy(uint8_t volume, uint16_t obj, uint16_t parent,
                      const char *name, size_t name_len, bool writing) {
    for(int i=0;i<FS_MAX_HANDLES;i++) {
        fs_file_t *f=&files[i];
        if(!f->handle||f->volume!=volume) continue;
        bool same=(obj&&f->obj==obj)||
                  (f->name_len==name_len&&f->parent==parent&&
                   !memcmp(f->name,name,name_len));
        if(!same) continue;
        if(writing||f->mode!=MODE_READ) return true;
    }
    return false;
}

// True when any handle is open on `obj` or on anything below it. rename and
// remove ask this because section 4 makes them BUSY while a handle is out.
static bool object_busy(uint16_t obj) {
    for(int i=0;i<FS_MAX_HANDLES;i++) {
        if(!files[i].handle||files[i].volume!=VOL_APP) continue;
        uint16_t at=files[i].obj;
        // An uncommitted create has no parent recorded in the index yet -- its
        // metadata block is what would record it -- so the walk starts from the
        // directory the handle was opened in. Without this, removing that
        // directory would succeed and the commit would publish a file whose
        // parent no longer exists.
        if(at&&pending[at-1]) {
            if(at==obj) return true;
            at=files[i].parent;
        }
        for(;at;at=store->dir[at-1].parent)
            if(at==obj) return true;
    }
    return false;
}

static bool writer_owns(const fs_file_t *f, uint8_t s) {
    for(unsigned i=0;i<f->nown;i++) if(f->own[i]==s) return true;
    return false;
}

static void writer_drop_own(fs_file_t *f, uint8_t s) {
    for(unsigned i=0;i<f->nown;i++)
        if(f->own[i]==s) { f->own[i]=f->own[--f->nown]; return; }
}

// Pushes the staging buffer out as one data block, and records the sector in
// the list the inode will publish.
//
// A partial block written by flush() is rewritten into a fresh sector when more
// bytes arrive. The one it replaces is released only if this writer allocated
// it: on an append the block being extended belongs to the version already on
// flash, and erasing that would destroy data an inode still names.
static bool writer_put_block(fs_file_t *f) {
    if(f->next_index>FS_MAX_BLOCKS||!room_for(1)) return false;
    int s=sector_take();
    if(s<0) return false;
    fs_blk_t hdr={.magic=FS_MAGIC,.owner=owner_hash,.gen=f->gen,.obj=f->obj,
                  .index=f->next_index,.len=f->buf_len,.kind=FS_KIND_DATA,
                  .crc=esp_crc32_le(0,f->buf,f->buf_len)};
    if(!block_write((unsigned)s,&hdr,f->buf,f->buf_len)) {
        sector_release((unsigned)s);
        return false;
    }
    uint8_t prev=f->sector[f->next_index-1];
    f->sector[f->next_index-1]=(uint8_t)s;
    f->own[f->nown++]=(uint8_t)s;
    if(prev!=FS_NO_SECTOR&&writer_owns(f,prev)) {
        writer_drop_own(f,prev);
        sector_release(prev);
    }
    if(f->buf_len>=FS_BLOCK_PAYLOAD) { f->next_index++; f->buf_len=0; }
    return true;
}

// The blocks the version being built will have once its inode lands.
static uint16_t writer_blocks(const fs_file_t *f) {
    return (uint16_t)(f->next_index-1+(f->buf_len?1:0));
}

// After a publish the staged sectors belong to the inode, not to the writer.
static void writer_published(fs_file_t *f) { f->nown=0; }

// -------------------------------------------------------------- file reads

// Copies at most `want` bytes from `pos` into `out`. Returns the byte count, 0
// at end of file, or -1 with *code set when a block does not verify.
static int file_bytes(fs_file_t *f, uint32_t pos, uint32_t want, uint8_t *out,
                      const char **code) {
    *code=NULL;
    if(pos>=f->size) return 0;
    uint32_t left=f->size-pos;
    if(want>left) want=left;
    if(f->volume==VOL_SD) {
        // The one place a removal is enforced for a read: section 3 asks that a
        // card going away end pending I/O with DISCONNECTED and that a
        // reinsertion not revive the handle, and a generation compare is what
        // both of those are.
        if(!sd_generation_valid(sd_media(),f->sd_gen)) {
            *code=POCKET_ERR_DISCONNECTED;
            return -1;
        }
        if(fseek(f->sd,(long)pos,SEEK_SET)!=0) { *code=POCKET_ERR_IO_ERROR; return -1; }
        size_t got=fread(out,1,want,f->sd);
        if(got==0&&ferror(f->sd)) {
            // A read that failed is the only removal signal this board has.
            sd_media_note_error(ESP_ERR_TIMEOUT);
            *code=POCKET_ERR_IO_ERROR;
            return -1;
        }
        return (int)got;
    }
    if(f->volume==VOL_ASSETS) {
        // Mapped flash: this is a memcpy out of the firmware image, and the
        // only copy that exists is the one the app receives.
        memcpy(out,ASSETS[f->asset].start+pos,want);
        return (int)want;
    }
    uint16_t idx=(uint16_t)(pos/FS_BLOCK_PAYLOAD+1);
    uint32_t within=pos%FS_BLOCK_PAYLOAD;
    uint32_t room=(uint32_t)FS_BLOCK_PAYLOAD-within;
    if(want>room) want=room;                    // never span two blocks
    // The handle carries the inode's block list, so finding the bytes is an
    // array lookup. This is the whole reason the host index needs no map of
    // which sector holds what.
    int s=(idx<=FS_MAX_BLOCKS)?f->sector[idx-1]:FS_NO_SECTOR;
    if(s==FS_NO_SECTOR) { *code=POCKET_ERR_CORRUPT_DATA; return -1; }
    if(f->verified!=(int16_t)s) {
        fs_blk_t hdr;
        if(!block_header((unsigned)s,&hdr)||!block_verify((unsigned)s,&hdr)) {
            *code=POCKET_ERR_CORRUPT_DATA;
            return -1;
        }
        if(within+want>hdr.len) { *code=POCKET_ERR_CORRUPT_DATA; return -1; }
        f->verified=(int16_t)s;
    }
    if(esp_partition_read(part,sector_at((unsigned)s)+sizeof(fs_blk_t)+within,
                          out,want)!=ESP_OK) {
        *code=POCKET_ERR_IO_ERROR;
        return -1;
    }
    return (int)want;
}

// The sd: operations the File methods and the entry points below dispatch
// into. They are DEFINED next to the capability that publishes them, at the end
// of this file, because they share nothing with the app: store above; they are
// declared here because the calls come first.
static JSValue sd_open(JSContext *ctx, const fs_path_t *p, uint8_t mode,
                       const char *want, const char *op, fs_file_t *f);
static JSValue sd_file_write(JSContext *ctx, fs_file_t *f, const char *op,
                             const uint8_t *data, size_t len);
static JSValue sd_publish(JSContext *ctx, const char *op, FILE *fh,
                          const char *fspath, const char *vpath, bool replacing);
static JSValue sd_fail(JSContext *ctx, const char *op, int e, const char *what,
                       const char *outcome);
static JSValue sd_mkdir(JSContext *ctx, const fs_path_t *p, const char *op,
                        bool recursive);
static JSValue sd_remove(JSContext *ctx, const fs_path_t *p, const char *op,
                         const char *want);
static JSValue sd_rename(JSContext *ctx, const fs_path_t *from,
                         const fs_path_t *to, const char *op, const char *want);
static JSValue sd_read_text(JSContext *ctx, const fs_path_t *p, const char *op,
                            uint32_t cap);
static JSValue sd_write_text(JSContext *ctx, const fs_path_t *p, const char *op,
                             const char *text, size_t len, bool replace,
                             const char *want);
static uint32_t sd_key_of(const char *fspath);
static bool sd_busy(uint32_t key, bool writing);

// Section 8's min(volume, quota, 2GiB-1), which here is the 2GiB-1: it is also
// the largest size the uint32 positions in this file can carry.
#define SD_MAX_FILE 2147483647u

// True while a handle still refers to the card it was opened on. Section 3
// wants a removal to end pending I/O and a reinsertion not to revive anything,
// and this compare is where both happen.
static bool sd_handle_live(const fs_file_t *f) {
    return f->volume!=VOL_SD||sd_generation_valid(sd_media(),f->sd_gen);
}

// --------------------------------------------------------- the File methods

static fs_file_t *this_file(JSValueConst self) {
    return file_of((uint32_t)(uintptr_t)JS_GetOpaque(self,file_class));
}

#define FILE_OR_REJECT(op) \
    fs_file_t *f=this_file(self); \
    if(!f) return pocket_api_reject(ctx,POCKET_ERR_CLOSED,(op), \
                                    "this file is closed",false, \
                                    POCKET_OUTCOME_NOT_APPLIED)

static JSValue js_file_read(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv) {
    static const char OP[]="fs.file.read";
    FILE_OR_REJECT(OP);
    if(f->mode!=MODE_READ)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "this file was not opened for reading",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    double want=0;
    if(argc<1||!JS_IsNumber(argv[0])||JS_ToFloat64(ctx,&want,argv[0])||
       !isfinite(want)||want!=(double)(int64_t)want||want<1||want>FS_CHUNK)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "maxBytes must be a whole number of 1 to 1024",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    fs_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the read",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    // One stack buffer, at most 1024 bytes, and it is gone when this returns.
    // What the app keeps is the Uint8Array, so a read costs the guest exactly
    // the bytes it asked for and the host keeps nothing.
    uint8_t chunk[FS_CHUNK];
    const char *code=NULL;
    int n=file_bytes(f,f->pos,(uint32_t)want,chunk,&code);
    if(n<0)
        return pocket_api_reject(ctx,code,OP,
                                 !strcmp(code,POCKET_ERR_CORRUPT_DATA)
                                     ?"a block of this file does not verify"
                                     :"the block could not be read",
                                 strcmp(code,POCKET_ERR_CORRUPT_DATA)!=0,
                                 POCKET_OUTCOME_NOT_APPLIED);
    // Section 5: null means end of file and nothing else. A short read is
    // ordinary and the caller simply asks again.
    if(n==0) return pocket_api_settled(ctx,JS_NULL,false);
    f->pos+=(uint32_t)n;
    return pocket_api_settled(ctx,JS_NewUint8ArrayCopy(ctx,chunk,(size_t)n),false);
}

static JSValue js_file_write(JSContext *ctx, JSValueConst self,
                             int argc, JSValueConst *argv) {
    static const char OP[]="fs.file.write";
    FILE_OR_REJECT(OP);
    if(f->mode==MODE_READ)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "this file was not opened for writing",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(f->broken)
        return pocket_api_reject(ctx,POCKET_ERR_CLOSED,OP,
                                 "an earlier write failed and closed this writer",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    size_t len=0;
    uint8_t *data=argc>0?JS_GetUint8Array(ctx,&len,argv[0]):NULL;
    if(!data) {
        JS_FreeValue(ctx,JS_GetException(ctx));
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "data must be a Uint8Array",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(len>FS_CHUNK)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "one write takes 0 to 1024 bytes",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    fs_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the write",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(!sd_handle_live(f))
        return pocket_api_reject(ctx,POCKET_ERR_DISCONNECTED,OP,
                                 "the card this file was opened on is gone",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    // Section 5 makes a zero-length write a no-op with no side effect, which
    // includes not moving tell() and not touching flash.
    if(len==0) return pocket_api_settled(ctx,JS_NewInt32(ctx,0),false);
    if(f->volume==VOL_SD) return sd_file_write(ctx,f,OP,data,len);
    if((uint64_t)f->size+len>FS_MAX_FILE)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "a file on this volume holds at most 24576 bytes",
                                 false,POCKET_OUTCOME_NOT_APPLIED);

    size_t done=0;
    while(done<len) {
        size_t room=FS_BLOCK_PAYLOAD-f->buf_len;
        size_t take=len-done<room?len-done:room;
        // Section 4: the bytes are copied at call acceptance, so what the app
        // does with its array afterwards cannot reach the flash.
        memcpy(f->buf+f->buf_len,data+done,take);
        f->buf_len+=(uint16_t)take;
        done+=take;
        if(f->buf_len>=FS_BLOCK_PAYLOAD) {
            if(f->next_index>FS_MAX_BLOCKS||!writer_put_block(f)) {
                // Section 5 asks a failed write to report how far it got and to
                // close the writer, because the temporary version is now of
                // unknown length. Nothing published changes either way.
                f->broken=true;
                f->size+=(uint32_t)done;
                JSValue e=pocket_api_error(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                           "the store has no room for this file",
                                           false,POCKET_OUTCOME_NOT_APPLIED);
                JS_SetPropertyStr(ctx,e,"bytesTransferred",
                                  JS_NewInt64(ctx,(int64_t)done));
                return pocket_api_settled(ctx,e,true);
            }
        }
    }
    f->size+=(uint32_t)len;
    f->pos=f->size;
    return pocket_api_settled(ctx,JS_NewInt64(ctx,(int64_t)len),false);
}

static JSValue js_file_seek(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv) {
    static const char OP[]="fs.file.seek";
    FILE_OR_REJECT(OP);
    // Section 5: read handles only, absolute, 0 to the file size inclusive.
    if(f->mode!=MODE_READ)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "only a read handle can seek",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    double where=0;
    if(argc<1||!JS_IsNumber(argv[0])||JS_ToFloat64(ctx,&where,argv[0])||
       !isfinite(where)||where!=(double)(int64_t)where||where<0||
       where>(double)f->size)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "offsetBytes must be from 0 to the file size",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    fs_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    f->pos=(uint32_t)where;
    return pocket_api_settled(ctx,JS_NewInt64(ctx,(int64_t)f->pos),false);
}

static JSValue js_file_tell(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    fs_file_t *f=this_file(self);
    if(!f) return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"fs.file.tell",
                                   "this file is closed",false,NULL);
    return JS_NewInt64(ctx,(int64_t)f->pos);
}

// Sends the staged bytes to the backend. For create and replace that is a
// synchronisation of the temporary version and NOT a publication -- section 5
// is explicit that only commit() publishes. For append it is the publication,
// because an append has no commit.
static JSValue js_file_flush(JSContext *ctx, JSValueConst self,
                             int argc, JSValueConst *argv) {
    static const char OP[]="fs.file.flush";
    FILE_OR_REJECT(OP);
    if(f->mode==MODE_READ)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "a read handle has nothing to flush",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(f->broken)
        return pocket_api_reject(ctx,POCKET_ERR_CLOSED,OP,
                                 "an earlier write failed and closed this writer",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    fs_options_t options;
    JSValue bad=take_options(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    if(!sd_handle_live(f))
        return pocket_api_reject(ctx,POCKET_ERR_DISCONNECTED,OP,
                                 "the card this file was opened on is gone",false,
                                 POCKET_OUTCOME_UNKNOWN);
    if(f->volume==VOL_SD) {
        // For create and replace this synchronises the TEMPORARY and publishes
        // nothing, which is section 5's rule; for append it is the publication,
        // an append having no commit to reach.
        int fd=fileno(f->sd);
        if(fflush(f->sd)!=0||(fd>=0&&fsync(fd)!=0)) {
            f->broken=true;
            return sd_fail(ctx,OP,errno,"the bytes could not be synchronised",
                           POCKET_OUTCOME_UNKNOWN);
        }
        return pocket_api_settled(ctx,JS_UNDEFINED,false);
    }
    if(f->buf_len&&!writer_put_block(f)) {
        f->broken=true;
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "the store has no room for this file",false,
                                 POCKET_OUTCOME_UNKNOWN);
    }
    if(f->mode==MODE_APPEND) {
        // The name and parent do not change, but the inode has to be rewritten
        // to name the new block list and the new length. inode_write() releases
        // the sector the extended tail block used to live in.
        const fs_obj_t *o=object(f->obj);
        if(!o) {
            f->broken=true;
            return pocket_api_reject(ctx,POCKET_ERR_CORRUPT_DATA,OP,
                                     "this file no longer has an inode",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        }
        char     name[FS_MAX_NAME];
        uint8_t  name_len=o->name_len;
        uint16_t parent=o->parent;
        memcpy(name,o->name,name_len);
        if(!inode_write(f->obj,FS_KIND_FILE,parent,name,name_len,f->gen,
                        f->size,f->sector,writer_blocks(f))) {
            f->broken=true;
            return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,
                                     "the appended bytes could not be published",
                                     true,POCKET_OUTCOME_UNKNOWN);
        }
        // Those sectors belong to the inode now; a later discard must not take
        // them back. The next write rewrites the tail into a fresh sector.
        writer_published(f);
        f->gen++;
    }
    return pocket_api_settled(ctx,JS_UNDEFINED,false);
}

static JSValue js_file_commit(JSContext *ctx, JSValueConst self,
                              int argc, JSValueConst *argv) {
    static const char OP[]="fs.file.commit";
    FILE_OR_REJECT(OP);
    if(f->mode!=MODE_CREATE&&f->mode!=MODE_REPLACE)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "only create and replace commit",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(f->broken)
        return pocket_api_reject(ctx,POCKET_ERR_CLOSED,OP,
                                 "an earlier write failed and closed this writer",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    fs_options_t options;
    JSValue bad=take_options(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    if(options.cancelled) {
        // Cancelled before the metadata lands, so nothing was published and the
        // temporary version goes back to the allocator.
        file_close(f,true);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the commit",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(f->volume==VOL_SD) {
        if(!sd_handle_live(f)) {
            // The temporary is on a card that is not here; nothing to erase and
            // nothing published.
            file_close(f,true);
            return pocket_api_reject(ctx,POCKET_ERR_DISCONNECTED,OP,
                                     "the card this file was opened on is gone",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        }
        // The handle ends here either way, per section 5, so the slot is given
        // back before the publish rather than after: what sd_publish() reports
        // is about the file, not about the handle.
        FILE       *fh=f->sd;
        sd_writer_t *w=f->sd_w;
        bool replacing=f->mode==MODE_REPLACE;
        f->sd=NULL;
        f->sd_w=NULL;
        file_close(f,false);
        JSValue answer=sd_publish(ctx,OP,fh,w->fspath,w->vpath,replacing);
        free(w);
        return answer;
    }
    if(f->buf_len&&!writer_put_block(f)) {
        f->broken=true;
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "the store has no room for this file",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    // A zero-byte commit is a legitimate empty file, not a refusal.
    uint16_t obj=f->obj, parent=f->parent;
    char name[FS_MAX_NAME]; uint8_t name_len=f->name_len;
    memcpy(name,f->name,name_len);
    if(!inode_write(obj,FS_KIND_FILE,parent,name,name_len,f->gen,
                    f->size,f->sector,writer_blocks(f))) {
        f->broken=true;
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,
                                 "the new version could not be published",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    writer_published(f);
    pending[obj-1]=0;
    // The handle is closed by a successful commit, per section 5, and the Entry
    // is built from the store rather than from the handle so that it reports
    // what was actually published.
    char path[FS_MAX_PATH+1];
    object_path(obj,path);
    JSValue entry=entry_of_object(ctx,path,obj);
    file_close(f,false);
    return pocket_api_settled(ctx,entry,false);
}

static JSValue js_file_close(JSContext *ctx, JSValueConst self,
                             int argc, JSValueConst *argv) {
    (void)ctx; (void)argc; (void)argv;
    fs_file_t *f=this_file(self);
    // Section 5 makes close() idempotent and synchronous. An uncommitted
    // create or replace loses its temporary version here, which is the whole
    // reason those modes are safe to abandon.
    if(f) { JS_SetOpaque(self,NULL); file_close(f,true); }
    return JS_UNDEFINED;
}

// -------------------------------------------------------------- fs errors
//
// Section 8 of docs/filesystem-api.md adds these to the common set. They are
// spelled here rather than in pocket_api.h because that header belongs to the
// foundation and this surface is the only thing that raises them. The
// consequence worth knowing: pocket.errorCodes lists the common codes only, so
// an app compares error.code against these strings.
#define FS_ERR_ALREADY_EXISTS "ALREADY_EXISTS"
#define FS_ERR_NOT_DIRECTORY  "NOT_DIRECTORY"
#define FS_ERR_IS_DIRECTORY   "IS_DIRECTORY"
#define FS_ERR_NOT_EMPTY      "NOT_EMPTY"
#define FS_ERR_READ_ONLY      "READ_ONLY"
#define FS_ERR_NO_SPACE       "NO_SPACE"
#define FS_ERR_QUOTA          "QUOTA_EXCEEDED"
#define FS_ERR_CROSS_DEVICE   "CROSS_DEVICE"
#define FS_ERR_STALE_CURSOR   "STALE_CURSOR"

// The sd: backend is defined further down, next to the capability that
// publishes it; the operations above dispatch into it by volume.
static JSValue sd_stat(JSContext *ctx, const fs_path_t *p, const char *op);
static const char *sd_errno_code(int e);
static int sd_list_page(JSContext *ctx, const fs_path_t *p, uint16_t at,
                        unsigned limit, JSValue entries, bool *more, int *err,
                        uint16_t *next_at);
// ------------------------------------------------------------- volume info

static void feature(JSContext *ctx, JSValue o, const char *name, bool on) {
    JS_SetPropertyStr(ctx,o,name,JS_NewBool(ctx,on));
}

// `touch` is section 3's one difference between volumes() and space(): the
// first must cause no media access and the second is allowed to ask. It is a
// parameter rather than two functions because everything else about the two
// answers is identical, and a copy would drift.
static JSValue volume_info(JSContext *ctx, uint8_t volume, bool touch) {
    JSValue v=JS_NewObject(ctx);
    if(JS_IsException(v)) return v;
    bool app=volume==VOL_APP;
    bool sd=volume==VOL_SD;
    const sd_media_t *media=sd_media();
    bool ready=app?(store!=NULL):(sd?media->state==SD_MEDIA_READY:true);
    JS_SetPropertyStr(ctx,v,"id",JS_NewString(ctx,VOLUME_NAME[volume]));
    // Section 3 has four states and sd: is the only volume that uses more than
    // one of them. ABSENT covers "no card" and "card removed" alike -- this
    // board has no card-detect pin, so the two are not distinguishable and
    // pretending otherwise would be an invention.
    const char *state=ready?"ready":"error";
    if(sd&&!ready) state=media->state==SD_MEDIA_ERROR?"error":"absent";
    JS_SetPropertyStr(ctx,v,"state",JS_NewString(ctx,state));
    // app: and assets: cannot be removed or remounted, so their generation is a
    // constant; sd: carries the real one, which moves on every mount.
    JS_SetPropertyStr(ctx,v,"generation",JS_NewInt32(ctx,
        sd?(int32_t)media->generation:1));
    JS_SetPropertyStr(ctx,v,"readOnly",JS_NewBool(ctx,!app&&!sd));
    // Byte comparison of names on the volumes this firmware implements, and NOT
    // on the card: FatFs folds case when it matches a long name, so two names
    // an app thinks are different are one file there. Section 2 asks this flag
    // to say so rather than leaving the app to find out.
    JS_SetPropertyStr(ctx,v,"caseSensitive",sd?JS_FALSE:JS_TRUE);

    if(sd) {
        // Section 3: freeBytes is the physical volume's, quota is the app's --
        // and no quota is enforced on the card, so those two are null rather
        // than 0. `limits` reports only what the code enforces and so does this.
        uint64_t capacity=0,freebytes=0;
        bool asked=touch&&ready&&sd_media_space(&capacity,&freebytes);
        JS_SetPropertyStr(ctx,v,"capacityBytes",
                          asked?JS_NewInt64(ctx,(int64_t)capacity):JS_NULL);
        JS_SetPropertyStr(ctx,v,"freeBytes",
                          asked?JS_NewInt64(ctx,(int64_t)freebytes):JS_NULL);
        JS_SetPropertyStr(ctx,v,"quotaBytes",JS_NULL);
        JS_SetPropertyStr(ctx,v,"usedBytes",JS_NULL);
    } else if(app&&ready) {
        JS_SetPropertyStr(ctx,v,"capacityBytes",JS_NewInt64(ctx,FS_SIZE));
        JS_SetPropertyStr(ctx,v,"freeBytes",
                          JS_NewInt64(ctx,(int64_t)sectors_free()*FS_SECTOR));
        JS_SetPropertyStr(ctx,v,"quotaBytes",JS_NewInt64(ctx,FS_QUOTA_BYTES));
        JS_SetPropertyStr(ctx,v,"usedBytes",
                          JS_NewInt64(ctx,(int64_t)store->mine*FS_SECTOR));
    } else if(app) {
        JS_SetPropertyStr(ctx,v,"capacityBytes",JS_NULL);
        JS_SetPropertyStr(ctx,v,"freeBytes",JS_NULL);
        JS_SetPropertyStr(ctx,v,"quotaBytes",JS_NULL);
        JS_SetPropertyStr(ctx,v,"usedBytes",JS_NULL);
    } else {
        int64_t total=0;
        for(unsigned i=0;i<ASSET_COUNT;i++) total+=asset_size((int)i);
        JS_SetPropertyStr(ctx,v,"capacityBytes",JS_NewInt64(ctx,total));
        JS_SetPropertyStr(ctx,v,"freeBytes",JS_NewInt64(ctx,0));
        // Section 3 keeps null and 0 apart: an app has no quota on a read-only
        // volume, which is not the same as a quota of nothing.
        JS_SetPropertyStr(ctx,v,"quotaBytes",JS_NULL);
        JS_SetPropertyStr(ctx,v,"usedBytes",JS_NULL);
    }

    JSValue f=JS_NewObject(ctx);
    bool writes=app||sd;
    feature(ctx,f,"read",true);
    feature(ctx,f,"write",writes);
    feature(ctx,f,"directories",writes);
    feature(ctx,f,"seekRead",true);
    feature(ctx,f,"replace",writes);
    // app:  the metadata block that publishes a version is one write, and until
    //       it lands a reader sees the old version whole.
    // sd:   a temporary beside the target, published by a rename.
    // Both are atomicReplace: no reader and no listing meets a partial file.
    feature(ctx,f,"atomicReplace",writes);
    // And this is NOT claimed, on either volume. Section 6 asks for a power-cut
    // test before the flag goes true and nobody has cut power to this board.
    // The design gives the property on app:; FAT does not give it at all. The
    // consequence for sd: is that create and replace refuse the default
    // durability -- see take_durability().
    feature(ctx,f,"crashSafeReplace",false);
    feature(ctx,f,"append",writes);
    feature(ctx,f,"rename",writes);
    JS_SetPropertyStr(ctx,v,"features",f);
    return v;
}

static JSValue js_volumes(JSContext *ctx, JSValueConst self,
                          int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    // Section 3: volumes() causes no media access, and an unsupported volume is
    // simply not listed. All three are implemented now, so all three are here
    // and sd: reports whatever state it is already in -- which before the
    // person has chosen a folder is "absent" with a card in the slot and
    // "absent" with an empty one, because nothing has looked.
    mount();
    JSValue a=JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx,a,0,volume_info(ctx,VOL_APP,false));
    JS_SetPropertyUint32(ctx,a,1,volume_info(ctx,VOL_ASSETS,false));
    JS_SetPropertyUint32(ctx,a,2,volume_info(ctx,VOL_SD,false));
    return a;
}

// onVolumeChange, and the reason this surface has a pump at all.
//
// sd: is the one volume here that moves: absent to ready when the person grants
// a folder, and back when an operation finds the card gone. Both of those
// happen in the WRONG PLACE to tell an app about -- one inside the picker's key
// handling, the other inside sd_fail(), which is reached from the middle of a
// native fs call. Delivering there would run a guest callback while this file
// is part way through an operation, and the listener could close the very
// handle that operation is working on.
//
// So nothing is delivered where the state moves. pocket_fs_pump() compares what
// the media says now against what was last announced, once a frame, on the JS
// task, with no fs call in flight -- the same shape every other surface uses
// and for the same reason. A change that happens and is undone inside one frame
// is not announced, which is correct: section 3 makes state an observation, and
// nobody observed that one.
static pocket_sub_slot_t volume_slots[2];
static pocket_sub_table_t volume_table={
    .slots=volume_slots,.count=2,.tag="pocket.fs",.what="onVolumeChange",
    .close_on_throw=false,
};

static JSValue js_on_volume_change(JSContext *ctx, JSValueConst self,
                                   int argc, JSValueConst *argv) {
    (void)self;
    volume_table.ctx=ctx;
    return pocket_api_sub_open(ctx,&volume_table,argc>0?argv[0]:JS_UNDEFINED,
                               "fs.onVolumeChange",
                               "two volume listeners are already open",NULL);
}

// Whether pocket.fs was ever read. The pump below and the reset at the end of
// this file both have real work to do -- flash erases among it -- and a run
// that never opened a file must not pay for either.
static bool built;

// What was last announced. Set at session start to what the media already says,
// so an app that subscribes hears about the NEXT change rather than about the
// state it could have read from volumes() in the same breath.
static uint32_t sd_told_gen;
static uint8_t  sd_told_state;

static void sd_told_now(void) {
    const sd_media_t *m=sd_media();
    sd_told_gen=m->generation;
    sd_told_state=(uint8_t)m->state;
}

static bool sd_volume_payload(JSContext *ctx, int slot, void *user,
                              JSValue *payload) {
    (void)slot; (void)user;
    // Section 3 forbids volumes() from touching the medium and this is the same
    // answer, so `touch` is false: an event says what changed, it does not go
    // and ask the card something new.
    *payload=volume_info(ctx,VOL_SD,false);
    return true;
}

void pocket_fs_pump(void) {
    if(!built) return;                 // nobody read pocket.fs; nothing to tell
    const sd_media_t *m=sd_media();
    if(m->generation==sd_told_gen&&(uint8_t)m->state==sd_told_state) return;
    sd_told_now();
    if(!volume_table.ctx) return;
    pocket_api_sub_deliver(&volume_table,sd_volume_payload,NULL);
    // available moves with the state -- sd_probe() reads the same two fields --
    // so capabilities.onChange hears it here too rather than only on the next
    // capabilities.get().
    pocket_api_capability_changed("fs.volume.sd");
}

static JSValue js_space(JSContext *ctx, JSValueConst self,
                        int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="fs.space";
    fs_path_t p;
    JSValue bad=take_path(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&p);
    if(!JS_IsUndefined(bad)) return bad;
    fs_options_t options;
    bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    // Section 3 has space() ask the medium. On app: the store IS the medium's
    // answer and was built from the sector headers rather than cached from a
    // guess; on sd: this is the one call that reaches f_getfree.
    return pocket_api_settled(ctx,volume_info(ctx,p.volume,true),false);
}

// ------------------------------------------------------------------- stat

static JSValue js_stat(JSContext *ctx, JSValueConst self,
                       int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="fs.stat";
    fs_path_t p;
    JSValue bad=take_path(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&p);
    if(!JS_IsUndefined(bad)) return bad;
    fs_options_t options;
    bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,"cancelled",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    if(!p.depth)
        // The volume root. It always exists and it is always a directory, so
        // an app can stat it to learn a volume is reachable without a name.
        return pocket_api_settled(ctx,
            entry_new(ctx,p.text,VOLUME_NAME[p.volume],
                      strlen(VOLUME_NAME[p.volume]),true,0,0,"root",true),false);

    if(p.volume==VOL_SD) return sd_stat(ctx,&p,OP);

    if(p.volume==VOL_ASSETS) {
        int a=asset_find(&p);
        if(a<0) return pocket_api_reject(ctx,POCKET_ERR_NOT_FOUND,OP,
                                         "no such asset",false,NULL);
        size_t n=0;
        const char *leaf=path_leaf(&p,&n);
        // Assets are part of the firmware image, so their revision is the
        // build and never moves inside one.
        return pocket_api_settled(ctx,
            entry_new(ctx,p.text,leaf,n,false,asset_size(a),0,"firmware",true),
            false);
    }
    const char *why=NULL;
    uint16_t id=resolve(&p,false,NULL,&why);
    if(!id) return pocket_api_reject(ctx,why?why:POCKET_ERR_NOT_FOUND,OP,
                                     "no such file or directory",false,NULL);
    return pocket_api_settled(ctx,entry_of_object(ctx,p.text,id),false);
}

// ------------------------------------------------------------------- list
//
// Section 4: the page is small on purpose. A cursor belongs to one app, one
// path and one mount generation, expires after 30 seconds of disuse, and is
// invalidated by any change to the store -- which is stricter than the section
// asks (it scopes invalidation to the directory) and is honest about it: this
// build keeps one mutation counter, not one per directory.

typedef struct {
    uint32_t token;         // 0 marks a free slot
    uint8_t  volume;
    uint16_t dir;
    uint16_t at;            // where the next page starts
    uint32_t owner;
    uint32_t mutations;
    // sd: only. A FAT directory has no mutation counter, so a cursor into one
    // is scoped by the media generation instead: a card removed and reinserted
    // mid-listing produces a new generation and this cursor goes STALE rather
    // than silently continuing into a different filesystem at the same offset.
    uint32_t gen;
    int64_t  expires_us;
} fs_cursor_t;

static fs_cursor_t cursors[FS_MAX_CURSORS];
static uint32_t    next_cursor_token=1;

static void cursors_clear(void) { memset(cursors,0,sizeof(cursors)); }

// Parses "<slot>.<token>". Returns the cursor, or NULL when it is unparseable,
// expired, foreign, or was made before a change the app has not seen.
static fs_cursor_t *cursor_find(const char *text, uint8_t volume, uint16_t dir) {
    unsigned slot=0; unsigned long token=0;
    if(sscanf(text,"%u.%lu",&slot,&token)!=2) return NULL;
    if(slot>=FS_MAX_CURSORS||!token) return NULL;
    fs_cursor_t *c=&cursors[slot];
    if(c->token!=(uint32_t)token||c->owner!=owner_hash) return NULL;
    if(c->volume!=volume||c->dir!=dir) return NULL;
    if(esp_timer_get_time()>c->expires_us) { c->token=0; return NULL; }
    return c;
}

static JSValue js_list(JSContext *ctx, JSValueConst self,
                       int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="fs.list";
    fs_path_t p;
    JSValue bad=take_path(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&p);
    if(!JS_IsUndefined(bad)) return bad;
    JSValueConst opt=argc>1?argv[1]:JS_UNDEFINED;
    fs_options_t options;
    bad=take_options(ctx,opt,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    int64_t limit=0;
    if(!take_int(ctx,opt,"limit",OP,1,FS_LIST_MAX,FS_LIST_DEFAULT,&limit,&bad))
        return bad;
    char cursor_text[32];
    if(!take_string(ctx,opt,"cursor",OP,cursor_text,sizeof(cursor_text),&bad))
        return bad;

    uint16_t dir=0;
    if(p.volume==VOL_APP&&p.depth) {
        const char *why=NULL;
        dir=resolve(&p,false,NULL,&why);
        if(!dir) return pocket_api_reject(ctx,why?why:POCKET_ERR_NOT_FOUND,OP,
                                          "no such directory",false,NULL);
        if(store->dir[dir-1].kind!=FS_KIND_DIR)
            return pocket_api_reject(ctx,FS_ERR_NOT_DIRECTORY,OP,
                                     "this path is a file",false,NULL);
    } else if(p.volume==VOL_ASSETS&&p.depth) {
        // assets: is flat in this build: one level, no directories.
        return pocket_api_reject(ctx,asset_find(&p)>=0?FS_ERR_NOT_DIRECTORY
                                                      :POCKET_ERR_NOT_FOUND,
                                 OP,"assets: has no directories",false,NULL);
    }

    fs_cursor_t *c=NULL;
    uint16_t at=0;
    if(cursor_text[0]) {
        c=cursor_find(cursor_text,p.volume,dir);
        if(!c)
            return pocket_api_reject(ctx,FS_ERR_STALE_CURSOR,OP,
                                     "this cursor expired or the directory changed",
                                     false,NULL);
        // assets: can be listed before anything has mounted app:, so the
        // counter is read through the pointer that may still be NULL.
        if(p.volume==VOL_SD) {
            if(!sd_generation_valid(sd_media(),c->gen)) {
                c->token=0;
                return pocket_api_reject(ctx,FS_ERR_STALE_CURSOR,OP,
                                         "the card changed while this listing was open",
                                         false,NULL);
            }
        } else if(c->mutations!=(store?store->mutations:0)) {
            c->token=0;
            return pocket_api_reject(ctx,FS_ERR_STALE_CURSOR,OP,
                                     "the store changed while this listing was open",
                                     false,NULL);
        }
        at=c->at;
    }

    JSValue entries=JS_NewArray(ctx);
    if(JS_IsException(entries)) return entries;
    uint32_t emitted=0;
    char path[FS_MAX_PATH+1];
    bool sd_more=false;
    if(p.volume==VOL_SD) {
        int err=0;
        uint16_t next_at=at;
        int n=sd_list_page(ctx,&p,at,(unsigned)limit,entries,&sd_more,&err,
                           &next_at);
        if(err) {
            JS_FreeValue(ctx,entries);
            sd_media_note_error(err==EIO?ESP_ERR_TIMEOUT:ESP_OK);
            return pocket_api_reject(ctx,sd_errno_code(err),OP,
                                     "this path could not be listed",false,NULL);
        }
        emitted=(uint32_t)n;
        at=next_at;
    } else if(p.volume==VOL_ASSETS) {
        for(;at<ASSET_COUNT&&emitted<limit;at++) {
            size_t n=strlen(ASSETS[at].name);
            path_join(path,&p,ASSETS[at].name,n);
            JS_SetPropertyUint32(ctx,entries,emitted++,
                entry_new(ctx,path,ASSETS[at].name,n,false,
                          asset_size((int)at),0,"firmware",true));
        }
    } else {
        // Backend enumeration order, which here is object-id order. Section 4
        // asks explicitly that a listing not be sorted: sorting would mean
        // holding every name at once, which is the thing a paged list avoids.
        // Backend enumeration order, which here is object-id order, and the
        // page is what bounds the work: one inode read per entry returned, not
        // one per file in the store. The owner is checked from that same read
        // because the volume root is the one directory two apps can share.
        for(;at<FS_MAX_OBJECTS&&emitted<limit;at++) {
            if(!store->dir[at].kind||store->dir[at].parent!=dir) continue;
            const fs_obj_t *o=object((uint16_t)(at+1));
            if(!o||o->owner!=owner_hash) continue;
            path_join(path,&p,o->name,o->name_len);
            JS_SetPropertyUint32(ctx,entries,emitted++,
                                 entry_of_object(ctx,path,(uint16_t)(at+1)));
        }
    }

    // More to come? Keep or make a cursor. Running out of cursor slots is not
    // an error for a listing that has finished. The digest answers this without
    // a read; at worst it says "more" for a foreign root entry and the next
    // page comes back empty with nextCursor null.
    bool more=false;
    if(p.volume==VOL_SD) more=sd_more;
    else if(p.volume==VOL_ASSETS) more=at<ASSET_COUNT;
    else for(uint16_t i=at;i<FS_MAX_OBJECTS&&!more;i++)
        more=store->dir[i].kind&&store->dir[i].parent==dir;

    JSValue result=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,result,"entries",entries);
    if(!more) {
        if(c) c->token=0;
        JS_SetPropertyStr(ctx,result,"nextCursor",JS_NULL);
        return pocket_api_settled(ctx,result,false);
    }
    if(!c) {
        for(int i=0;i<FS_MAX_CURSORS&&!c;i++)
            if(!cursors[i].token||cursors[i].expires_us<esp_timer_get_time())
                c=&cursors[i];
        if(!c) {
            JS_FreeValue(ctx,result);
            return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                     "two listings are already open",false,NULL);
        }
        c->token=next_cursor_token++;
        c->volume=p.volume; c->dir=dir; c->owner=owner_hash;
    }
    c->at=at;
    c->mutations=store?store->mutations:0;
    c->gen=sd_media()->generation;
    c->expires_us=esp_timer_get_time()+FS_CURSOR_US;
    char text[32];
    snprintf(text,sizeof(text),"%u.%lu",(unsigned)(c-cursors),
             (unsigned long)c->token);
    JS_SetPropertyStr(ctx,result,"nextCursor",JS_NewString(ctx,text));
    return pocket_api_settled(ctx,result,false);
}

// ------------------------------------------------------ write-side guards

// The checks every mutating call shares. Returns a rejection, or JS_UNDEFINED.
static JSValue writable(JSContext *ctx, const fs_path_t *p, const char *op) {
    // sd: passes here, and the check that matters for it has already happened:
    // take_path() refused the call unless the person granted a folder AND the
    // card is mounted, in that order.
    if(p->volume==VOL_ASSETS)
        return pocket_api_reject(ctx,FS_ERR_READ_ONLY,op,
                                 "assets: is read-only",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(!p->depth)
        return pocket_api_reject(ctx,POCKET_ERR_PERMISSION_DENIED,op,
                                 "the volume root cannot be changed",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    return JS_UNDEFINED;
}

// ------------------------------------------------------------------ mkdir

static JSValue js_mkdir(JSContext *ctx, JSValueConst self,
                        int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="fs.mkdir";
    fs_path_t p;
    JSValue bad=take_path(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&p);
    if(!JS_IsUndefined(bad)) return bad;
    bad=writable(ctx,&p,OP);
    if(!JS_IsUndefined(bad)) return bad;
    JSValueConst opt=argc>1?argv[1]:JS_UNDEFINED;
    fs_options_t options;
    bad=take_options(ctx,opt,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    bool recursive=false;
    if(JS_IsObject(opt)) {
        JSValue r=JS_GetPropertyStr(ctx,opt,"recursive");
        if(JS_IsException(r)) return JS_EXCEPTION;
        recursive=JS_ToBool(ctx,r);
        JS_FreeValue(ctx,r);
    }
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,"cancelled",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(p.volume==VOL_SD) return sd_mkdir(ctx,&p,OP,recursive);

    uint16_t at=0;
    for(unsigned i=0;i<p.depth;i++) {
        const char *name=p.text+p.off[i];
        size_t n=p.size[i];
        uint16_t child=child_of(at,name,n);
        bool last=i+1==p.depth;
        if(child) {
            if(store->dir[child-1].kind!=FS_KIND_DIR)
                return pocket_api_reject(ctx,
                    last?FS_ERR_ALREADY_EXISTS:FS_ERR_NOT_DIRECTORY,OP,
                    "a file already has that name",false,
                    POCKET_OUTCOME_NOT_APPLIED);
            at=child;
            // Section 4: an existing directory is a success, not a conflict.
            if(last) return pocket_api_settled(ctx,JS_UNDEFINED,false);
            continue;
        }
        if(!last&&!recursive)
            return pocket_api_reject(ctx,POCKET_ERR_NOT_FOUND,OP,
                                     "the parent directory does not exist",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        if(!room_for(1))
            return pocket_api_reject(ctx,FS_ERR_QUOTA,OP,
                                     "no room left for another directory",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        uint16_t id=object_claim();
        if(!id)
            return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                     "the store holds at most 24 objects",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        if(!inode_write(id,FS_KIND_DIR,at,name,n,1,0,NULL,0)) {
            object_unclaim(id);
            // Section 4: a recursive mkdir that fails part way does not roll
            // back, and the outcome says so rather than pretending otherwise.
            return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,
                                     "the directory could not be written",true,
                                     i?POCKET_OUTCOME_UNKNOWN:POCKET_OUTCOME_NOT_APPLIED);
        }
        pending[id-1]=0;
        at=id;
    }
    return pocket_api_settled(ctx,JS_UNDEFINED,false);
}

// ----------------------------------------------------------------- remove

static JSValue js_remove(JSContext *ctx, JSValueConst self,
                         int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="fs.remove";
    fs_path_t p;
    JSValue bad=take_path(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&p);
    if(!JS_IsUndefined(bad)) return bad;
    bad=writable(ctx,&p,OP);
    if(!JS_IsUndefined(bad)) return bad;
    JSValueConst opt=argc>1?argv[1]:JS_UNDEFINED;
    fs_options_t options;
    bad=take_options(ctx,opt,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    char want[24];
    if(!take_string(ctx,opt,"ifRevision",OP,want,sizeof(want),&bad)) return bad;
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,"cancelled",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(p.volume==VOL_SD) return sd_remove(ctx,&p,OP,want);

    const char *why=NULL;
    uint16_t id=resolve(&p,false,NULL,&why);
    if(!id) return pocket_api_reject(ctx,why?why:POCKET_ERR_NOT_FOUND,OP,
                                     "no such file or directory",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
    if(store->dir[id-1].kind==FS_KIND_DIR&&dir_has_children(id))
        return pocket_api_reject(ctx,FS_ERR_NOT_EMPTY,OP,
                                 "this directory is not empty",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(object_busy(id))
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "a handle is open on this path",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(want[0]) {
        char have[24];
        revision_of_object(have,id);
        // Section 4: the check and the removal happen under the same host lock,
        // which on a single JS task means inside this one call.
        if(strcmp(have,want))
            return pocket_api_reject(ctx,POCKET_ERR_CONFLICT,OP,
                                     "the revision has moved on",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
    }
    if(!object_remove(id))
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,
                                 "the tombstone could not be written",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    return pocket_api_settled(ctx,JS_UNDEFINED,false);
}

// ----------------------------------------------------------------- rename

static bool is_below(uint16_t maybe_child, uint16_t maybe_parent) {
    for(uint16_t at=maybe_child;at;at=store->dir[at-1].parent)
        if(at==maybe_parent) return true;
    return false;
}

static JSValue js_rename(JSContext *ctx, JSValueConst self,
                         int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="fs.rename";
    fs_path_t from,to;
    JSValue bad=take_path(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&from);
    if(!JS_IsUndefined(bad)) return bad;
    bad=take_path(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&to);
    if(!JS_IsUndefined(bad)) return bad;
    if(from.volume!=to.volume)
        return pocket_api_reject(ctx,FS_ERR_CROSS_DEVICE,OP,
                                 "rename stays inside one volume",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    bad=writable(ctx,&from,OP);
    if(!JS_IsUndefined(bad)) return bad;
    bad=writable(ctx,&to,OP);
    if(!JS_IsUndefined(bad)) return bad;
    JSValueConst opt=argc>2?argv[2]:JS_UNDEFINED;
    fs_options_t options;
    bad=take_options(ctx,opt,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    char want[24];
    if(!take_string(ctx,opt,"ifRevision",OP,want,sizeof(want),&bad)) return bad;
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,"cancelled",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(from.volume==VOL_SD) return sd_rename(ctx,&from,&to,OP,want);

    const char *why=NULL;
    uint16_t id=resolve(&from,false,NULL,&why);
    if(!id) return pocket_api_reject(ctx,why?why:POCKET_ERR_NOT_FOUND,OP,
                                     "no such file or directory",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
    uint16_t parent=0;
    resolve(&to,true,&parent,&why);
    if(why) return pocket_api_reject(ctx,why,OP,
                                     "the destination directory is not there",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
    size_t n=0;
    const char *leaf=path_leaf(&to,&n);
    if(child_of(parent,leaf,n))
        // Section 4: rename never overwrites.
        return pocket_api_reject(ctx,FS_ERR_ALREADY_EXISTS,OP,
                                 "something already has that name",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(is_below(parent,id))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "a directory cannot move inside itself",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(object_busy(id))
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "a handle is open on this path",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(want[0]) {
        char have[24];
        revision_of_object(have,id);
        if(strcmp(have,want))
            return pocket_api_reject(ctx,POCKET_ERR_CONFLICT,OP,
                                     "the revision has moved on",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
    }
    if(!room_for(1))
        return pocket_api_reject(ctx,FS_ERR_QUOTA,OP,
                                 "no room for the new name",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    // Copied out of the window, because inode_write() moves the window on.
    const fs_obj_t *live=object(id);
    if(!live)
        return pocket_api_reject(ctx,POCKET_ERR_CORRUPT_DATA,OP,
                                 "this path has no readable inode",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    fs_obj_t o=*live;
    // One inode carries the new name and parent and re-lists the SAME sectors,
    // so not a byte of the data is rewritten and no observer sees a half-renamed
    // tree. The old inode is released once the new one is on flash.
    if(!inode_write(id,o.kind,parent,leaf,n,o.gen+1,o.size,o.sector,o.blocks))
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,
                                 "the new name could not be written",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    return pocket_api_settled(ctx,JS_UNDEFINED,false);
}

// ------------------------------------------------------------------- open

// durability, and the one place this implementation knowingly departs from the
// specification.
//
// Section 6 makes crash-safe the DEFAULT for create and replace, and says an
// unsupported guarantee must answer UNSUPPORTED rather than quietly weaken.
// Both cannot hold here: crashSafeReplace is false (no power-cut test has been
// run), so following the default literally would make every ordinary
// writeText() fail with UNSUPPORTED and the volume would be write-only in
// theory and unusable in practice. Taken together the two rules can only mean
// "do not silently downgrade a guarantee the caller asked for", so:
//
//   - the default on a volume whose crashSafeReplace is false is "synced",
//   - an explicit durability:"crash-safe" is refused with UNSUPPORTED.
//
// Nothing is downgraded behind the app: it either did not ask, or it was told
// no. When the power-cut test of section 6 is run and passes, the flag and this
// default move together.
// `must_ask` is the sd: case, and it is the one place the two volumes differ:
// there the default IS refused, exactly as section 6 writes it, because FAT
// cannot give the guarantee the default names. docs/filesystem-api.md section 6
// calls that an intended surprise and asks for the refusal to name the value it
// accepts -- a spec-lookup UNSUPPORTED and a self-correcting one differ by one
// string, and this is that string.
static JSValue take_durability(JSContext *ctx, JSValueConst opt, const char *op,
                               bool must_ask, bool *bad_out, JSValue *bad) {
    char text[16];
    *bad_out=false;
    if(!take_string(ctx,opt,"durability",op,text,sizeof(text),bad)) {
        *bad_out=true;
        return *bad;
    }
    if(!text[0]&&must_ask) {
        *bad_out=true;
        return pocket_api_reject(ctx,POCKET_ERR_UNSUPPORTED,op,
            "sd: cannot promise crash-safe replacement, which is what create "
            "and replace default to; pass durability:\"synced\" to accept what "
            "the card can give",
            false,POCKET_OUTCOME_NOT_APPLIED);
    }
    if(!text[0]||!strcmp(text,"synced")) return JS_UNDEFINED;
    *bad_out=true;
    if(!strcmp(text,"crash-safe"))
        return pocket_api_reject(ctx,POCKET_ERR_UNSUPPORTED,op,
            "crash-safe durability is not proven on this volume; ask for synced",
            false,POCKET_OUTCOME_NOT_APPLIED);
    return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "durability must be synced or crash-safe",false,
                             POCKET_OUTCOME_NOT_APPLIED);
}

static fs_file_t *file_slot(void) {
    for(int i=0;i<FS_MAX_HANDLES;i++) if(!files[i].handle) return &files[i];
    return NULL;
}

static JSValue file_wrap(JSContext *ctx, fs_file_t *f) {
    JSValue o=JS_NewObjectClass(ctx,file_class);
    if(JS_IsException(o)) { file_close(f,true); return o; }
    JS_SetOpaque(o,(void *)(uintptr_t)f->handle);
    return o;
}

static JSValue js_open(JSContext *ctx, JSValueConst self,
                       int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="fs.open";
    fs_path_t p;
    JSValue bad=take_path(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&p);
    if(!JS_IsUndefined(bad)) return bad;
    JSValueConst opt=argc>1?argv[1]:JS_UNDEFINED;
    fs_options_t options;
    bad=take_options(ctx,opt,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    char mode_text[16];
    if(!take_string(ctx,opt,"mode",OP,mode_text,sizeof(mode_text),&bad)) return bad;
    uint8_t mode;
    if(!strcmp(mode_text,"read"))         mode=MODE_READ;
    else if(!strcmp(mode_text,"create"))  mode=MODE_CREATE;
    else if(!strcmp(mode_text,"replace")) mode=MODE_REPLACE;
    else if(!strcmp(mode_text,"append"))  mode=MODE_APPEND;
    else return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                  "mode must be read, create, replace or append",
                                  false,POCKET_OUTCOME_NOT_APPLIED);
    char want[24];
    if(!take_string(ctx,opt,"ifRevision",OP,want,sizeof(want),&bad)) return bad;
    if(mode!=MODE_READ) {
        bool wrong=false;
        JSValue d=take_durability(ctx,opt,OP,
                                  p.volume==VOL_SD&&mode!=MODE_APPEND,
                                  &wrong,&bad);
        if(wrong) return d;
    }
    // Section 5: a create has nothing to check a revision against.
    if(mode==MODE_CREATE&&want[0])
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "create takes no ifRevision",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(mode!=MODE_READ) {
        bad=writable(ctx,&p,OP);
        if(!JS_IsUndefined(bad)) return bad;
    }
    if(!p.depth)
        return pocket_api_reject(ctx,FS_ERR_IS_DIRECTORY,OP,
                                 "the volume root is a directory",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,"cancelled",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    size_t name_len=0;
    const char *leaf=path_leaf(&p,&name_len);
    fs_file_t *f=file_slot();
    if(!f)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "two files are already open",true,
                                 POCKET_OUTCOME_NOT_APPLIED);

    // ---- sd: the card, inside the folder the person granted
    if(p.volume==VOL_SD) return sd_open(ctx,&p,mode,want,OP,f);

    // ---- assets: read only, straight out of the firmware image
    if(p.volume==VOL_ASSETS) {
        int a=asset_find(&p);
        if(a<0) return pocket_api_reject(ctx,POCKET_ERR_NOT_FOUND,OP,
                                         "no such asset",false,NULL);
        if(want[0]&&strcmp(want,"firmware"))
            return pocket_api_reject(ctx,POCKET_ERR_CONFLICT,OP,
                                     "this asset belongs to another build",false,
                                     NULL);
        memset(f,0,sizeof(*f));
        f->handle=next_handle++; f->mode=MODE_READ; f->volume=VOL_ASSETS;
        f->asset=(int8_t)a; f->size=asset_size(a); f->verified=-1;
        f->name_len=(uint8_t)name_len;
        memcpy(f->name,leaf,name_len);
        return pocket_api_settled(ctx,file_wrap(ctx,f),false);
    }

    // ---- app:
    uint16_t parent=0;
    const char *why=NULL;
    resolve(&p,true,&parent,&why);
    if(why) return pocket_api_reject(ctx,why,OP,
                                     "the containing directory is not there",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
    uint16_t id=child_of(parent,leaf,name_len);
    if(id&&store->dir[id-1].kind==FS_KIND_DIR)
        return pocket_api_reject(ctx,FS_ERR_IS_DIRECTORY,OP,
                                 "this path is a directory",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(mode==MODE_CREATE&&id)
        return pocket_api_reject(ctx,FS_ERR_ALREADY_EXISTS,OP,
                                 "this file already exists",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(mode!=MODE_CREATE&&!id)
        return pocket_api_reject(ctx,POCKET_ERR_NOT_FOUND,OP,"no such file",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    if(want[0]&&id) {
        char have[24];
        revision_of_object(have,id);
        // Section 5: for replace and append the check and the taking of the
        // write right are one step, which on one JS task is this call.
        if(strcmp(have,want))
            return pocket_api_reject(ctx,POCKET_ERR_CONFLICT,OP,
                                     "the revision has moved on",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
    }
    if(file_busy(VOL_APP,id,parent,leaf,name_len,mode!=MODE_READ))
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "this file is open elsewhere",true,
                                 POCKET_OUTCOME_NOT_APPLIED);

    memset(f,0,sizeof(*f));
    f->handle=next_handle++; f->mode=mode; f->volume=VOL_APP; f->asset=-1;
    f->parent=parent; f->name_len=(uint8_t)name_len; f->verified=-1;
    memset(f->sector,FS_NO_SECTOR,sizeof(f->sector));
    memcpy(f->name,leaf,name_len);

    if(mode==MODE_READ||mode==MODE_APPEND) {
        // The block list comes off the inode once, here. Nothing can replace
        // the file while this handle is out -- a writer and any other handle on
        // the same object are BUSY -- so the copy stays true for its lifetime.
        const fs_obj_t *o=object(id);
        if(!o) {
            memset(f,0,sizeof(*f));
            return pocket_api_reject(ctx,POCKET_ERR_CORRUPT_DATA,OP,
                                     "this file has no readable inode",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        }
        memcpy(f->sector,o->sector,sizeof(f->sector));
        f->size=o->size;
    }
    if(mode==MODE_READ) {
        f->obj=id;
        return pocket_api_settled(ctx,file_wrap(ctx,f),false);
    }

    f->buf=malloc(FS_BLOCK_PAYLOAD);
    if(!f->buf) {
        memset(f,0,sizeof(*f));
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                 "no memory for the write buffer",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    f->next_index=1;
    if(mode==MODE_CREATE) {
        id=object_claim();
        if(!id) {
            free(f->buf);
            memset(f,0,sizeof(*f));
            return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                     "the store holds at most 24 objects",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        }
        f->obj=id;
        f->gen=1;
    } else {
        f->obj=id;
        // Above the published version, so nothing this writer lays down can be
        // seen until its inode says so.
        const fs_obj_t *o=object(id);
        if(!o) {
            free(f->buf);
            memset(f,0,sizeof(*f));
            return pocket_api_reject(ctx,POCKET_ERR_CORRUPT_DATA,OP,
                                     "this file has no readable inode",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        }
        f->gen=o->gen+1;
        // A replace builds a whole new block list; the old one stays on the
        // inode until commit and is released there.
        if(mode==MODE_REPLACE) memset(f->sector,FS_NO_SECTOR,sizeof(f->sector));
    }
    if(mode==MODE_APPEND) {
        // tell() starts at the existing end, and the partial tail block is
        // pulled back into the staging buffer so the append continues the block
        // rather than starting a ragged one.
        f->pos=f->size;
        if(f->size) {
            uint32_t total=f->size;
            f->next_index=(uint16_t)((total-1)/FS_BLOCK_PAYLOAD+1);
            uint32_t whole=(uint32_t)(f->next_index-1)*FS_BLOCK_PAYLOAD;
            uint32_t tail=total-whole;
            if(tail<FS_BLOCK_PAYLOAD) {
                fs_file_t reader=*f;
                reader.mode=MODE_READ; reader.verified=-1;
                const char *code=NULL;
                if(file_bytes(&reader,whole,tail,f->buf,&code)!=(int)tail) {
                    free(f->buf);
                    memset(f,0,sizeof(*f));
                    return pocket_api_reject(ctx,code?code:POCKET_ERR_CORRUPT_DATA,
                                             OP,"the last block does not verify",
                                             false,POCKET_OUTCOME_NOT_APPLIED);
                }
                f->buf_len=(uint16_t)tail;
            } else f->next_index++;
        }
    }
    return pocket_api_settled(ctx,file_wrap(ctx,f),false);
}

// ------------------------------------------------------------------- copy
//
// Section 4: a fixed native buffer, the destination name kept out of sight
// until the transfer finishes, and nothing loaded whole into RAM. The buffer is
// the same 4072-byte staging block a writer uses, so a copy costs one block of
// host memory whatever the file size.

static bool writer_feed(fs_file_t *w, const uint8_t *data, size_t n) {
    size_t done=0;
    while(done<n) {
        size_t room=FS_BLOCK_PAYLOAD-w->buf_len;
        size_t take=n-done<room?n-done:room;
        memcpy(w->buf+w->buf_len,data+done,take);
        w->buf_len+=(uint16_t)take;
        done+=take;
        if(w->buf_len>=FS_BLOCK_PAYLOAD) {
            if(w->next_index>FS_MAX_BLOCKS||!writer_put_block(w)) return false;
        }
    }
    w->size+=(uint32_t)n;
    return true;
}

static JSValue js_copy(JSContext *ctx, JSValueConst self,
                       int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="fs.copy";
    fs_path_t from,to;
    JSValue bad=take_path(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&from);
    if(!JS_IsUndefined(bad)) return bad;
    bad=take_path(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&to);
    if(!JS_IsUndefined(bad)) return bad;
    fs_options_t options;
    bad=take_options(ctx,argc>2?argv[2]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    bad=writable(ctx,&to,OP);
    if(!JS_IsUndefined(bad)) return bad;
    // Section 4 allows copy across volumes and this build does not serve the
    // card end of it: the transfer would have to be paced across frames to keep
    // a 400 kHz bus off the input path, and that is the job the future job API
    // gets. Refusing by name beats a copy that stalls a frame for a second.
    if(from.volume==VOL_SD||to.volume==VOL_SD)
        return pocket_api_reject(ctx,POCKET_ERR_UNSUPPORTED,OP,
            "copy does not serve sd: yet; open both paths and move the bytes",
            false,POCKET_OUTCOME_NOT_APPLIED);
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,"cancelled",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    // The source, as a reader that never enters the handle table: section 8
    // counts a native copy against the same resources, and it does that here by
    // running inside one JS turn while no other operation can start.
    fs_file_t src={.mode=MODE_READ,.volume=from.volume,.asset=-1,.verified=-1};
    if(from.volume==VOL_ASSETS) {
        int a=asset_find(&from);
        if(a<0) return pocket_api_reject(ctx,POCKET_ERR_NOT_FOUND,OP,
                                         "no such asset",false,NULL);
        src.asset=(int8_t)a; src.size=asset_size(a);
    } else {
        const char *why=NULL;
        uint16_t id=resolve(&from,false,NULL,&why);
        if(!id) return pocket_api_reject(ctx,why?why:POCKET_ERR_NOT_FOUND,OP,
                                         "no such file",false,NULL);
        if(store->dir[id-1].kind==FS_KIND_DIR)
            return pocket_api_reject(ctx,FS_ERR_IS_DIRECTORY,OP,
                                     "copy takes files only",false,NULL);
        const fs_obj_t *o=object(id);
        if(!o) return pocket_api_reject(ctx,POCKET_ERR_CORRUPT_DATA,OP,
                                        "the source has no readable inode",
                                        false,NULL);
        src.obj=id; src.size=o->size;
        memcpy(src.sector,o->sector,sizeof(src.sector));
    }
    if(src.size>FS_MAX_FILE)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "the source is larger than this volume allows",
                                 false,POCKET_OUTCOME_NOT_APPLIED);

    uint16_t parent=0;
    const char *why=NULL;
    resolve(&to,true,&parent,&why);
    if(why) return pocket_api_reject(ctx,why,OP,
                                     "the destination directory is not there",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
    size_t name_len=0;
    const char *leaf=path_leaf(&to,&name_len);
    if(child_of(parent,leaf,name_len))
        return pocket_api_reject(ctx,FS_ERR_ALREADY_EXISTS,OP,
                                 "copy does not overwrite",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(file_busy(VOL_APP,0,parent,leaf,name_len,true))
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "the destination is open elsewhere",true,
                                 POCKET_OUTCOME_NOT_APPLIED);

    fs_file_t dst={.mode=MODE_CREATE,.volume=VOL_APP,.asset=-1,.parent=parent,
                   .verified=-1,.next_index=1,.gen=1};
    memset(dst.sector,FS_NO_SECTOR,sizeof(dst.sector));
    dst.obj=object_claim();
    if(!dst.obj)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "the store holds at most 24 objects",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    dst.buf=malloc(FS_BLOCK_PAYLOAD);
    if(!dst.buf) {
        object_unclaim(dst.obj);
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                 "no memory for the copy buffer",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }

    const char *code=NULL;
    uint8_t piece[256];
    uint32_t at=0;
    bool ok=true;
    while(at<src.size&&ok) {
        int n=file_bytes(&src,at,sizeof(piece),piece,&code);
        if(n<=0) { ok=false; break; }
        at+=(uint32_t)n;
        ok=writer_feed(&dst,piece,(size_t)n);
        if(!ok) code=FS_ERR_QUOTA;
    }
    if(ok&&dst.buf_len) ok=writer_put_block(&dst);
    if(ok) ok=inode_write(dst.obj,FS_KIND_FILE,parent,leaf,name_len,dst.gen,
                          dst.size,dst.sector,writer_blocks(&dst));
    if(!ok) {
        // Section 4: a failed or cancelled copy throws the temporary version
        // away and leaves the source untouched. Nothing was ever published.
        writer_discard(&dst);
        object_unclaim(dst.obj);
        free(dst.buf);
        return pocket_api_reject(ctx,code?code:POCKET_ERR_IO_ERROR,OP,
                                 "the copy could not be completed",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    free(dst.buf);
    writer_published(&dst);
    pending[dst.obj-1]=0;
    return pocket_api_settled(ctx,entry_of_object(ctx,to.text,dst.obj),false);
}

// ------------------------------------------------------- native file reads
//
// Two entry points for surfaces that have no JS value to hand back and no JS
// round trip to get one through. pocket_av.c's audio.player is the caller: it
// asks how long a clip is when it opens, then reads a ring slot at a time while
// it plays.
//
// THE JS TASK'S TO CALL, and now for three reasons rather than one. The flash
// reads happen inside the call; the app: index and handle table are this task's;
// and on sd: the grant, the mount generation and FatFs itself all belong to it.
// Calling either of these from a worker would race the picker.
//
// The resolution is shared rather than copied, and that is the point of it
// being written this way. A ranged read is a second way into the same
// backends, and a second way in is where an invariant gets bypassed -- the
// grant tested BEFORE the media state, a directory refused before a size is
// reported, a writer's file refused to a reader. One function, so there is one
// set of rules and not two that drift.

// Fills `src` with a reader that never enters the handle table. Returns NULL on
// success, or the PocketError code the caller should report. On sd: it leaves
// an open FILE in src->sd which the caller MUST fclose.
static const char *native_reader(const char *path, fs_path_t *p, fs_file_t *src) {
    const char *why=path_parse(path,path?strlen(path):0,p);
    if(why) return POCKET_ERR_INVALID_ARGUMENT;
    if(!p->depth) return FS_ERR_IS_DIRECTORY;

    memset(src,0,sizeof(*src));
    src->mode=MODE_READ; src->volume=p->volume; src->asset=-1; src->verified=-1;

    if(p->volume==VOL_SD) {
        // The same two checks take_path() makes for an app, in the same order
        // and for the same reason: an ungranted caller must not be able to tell
        // a mounted card from an empty slot by which error comes back. See the
        // header of sd_path.h -- this is the second caller that ordering has,
        // and it is why the ordering is a contract rather than a preference.
        const sd_media_t *m=sd_media();
        if(!m->granted) return POCKET_ERR_PERMISSION_DENIED;
        if(m->state!=SD_MEDIA_READY) return POCKET_ERR_DISCONNECTED;
        char fspath[SD_FSPATH_MAX];
        if(sd_path_build(m,p->text+p->off[0],(size_t)(p->len-p->off[0]),
                         fspath,sizeof fspath)!=SD_PATH_OK)
            return POCKET_ERR_INVALID_ARGUMENT;
        struct stat st;
        if(stat(fspath,&st)!=0) {
            int e=errno;
            if(e==EIO) sd_media_note_error(ESP_ERR_TIMEOUT);
            return sd_errno_code(e);
        }
        if(S_ISDIR(st.st_mode)) return FS_ERR_IS_DIRECTORY;
        if((uint64_t)st.st_size>SD_MAX_FILE) return POCKET_ERR_LIMIT_EXCEEDED;
        // Section 5's exclusion applies to a native reader too: section 8 counts
        // the player against the same resources as a JS handle, so a file some
        // app is part way through writing is not one this reads.
        uint32_t key=sd_key_of(fspath);
        if(sd_busy(key,false)) return POCKET_ERR_BUSY;
        src->sd=fopen(fspath,"rb");
        if(!src->sd) {
            int e=errno;
            if(e==EIO) sd_media_note_error(ESP_ERR_TIMEOUT);
            return sd_errno_code(e);
        }
        src->sd_key=key;
        // The generation this read is against. file_bytes() compares it, so a
        // card pulled between the open and the read answers DISCONNECTED rather
        // than whatever the next card has at that offset. There is no handle
        // held across calls, so the grant dying with the media -- sd_path.h's
        // second invariant -- is what stops the NEXT call as well.
        src->sd_gen=sd_media()->generation;
        src->size=(uint32_t)st.st_size;
        return NULL;
    }
    if(p->volume==VOL_APP&&!mount()) return POCKET_ERR_NOT_AVAILABLE;
    if(p->volume==VOL_ASSETS) {
        int a=asset_find(p);
        if(a<0) return POCKET_ERR_NOT_FOUND;
        src->asset=(int8_t)a; src->size=asset_size(a);
        return NULL;
    }
    uint16_t id=resolve(p,false,NULL,&why);
    if(!id) return why?why:POCKET_ERR_NOT_FOUND;
    if(store->dir[id-1].kind==FS_KIND_DIR) return FS_ERR_IS_DIRECTORY;
    const fs_obj_t *o=object(id);
    if(!o) return POCKET_ERR_CORRUPT_DATA;
    src->obj=id; src->size=o->size;
    memcpy(src->sector,o->sector,sizeof(src->sector));
    return NULL;
}

static void native_reader_close(fs_file_t *src) {
    // Only if the card that opened it is still the card in the slot: after a
    // removal the fd belongs to a VFS that has been unregistered. Same rule as
    // file_close(), and the same reason.
    if(src->sd&&sd_generation_valid(sd_media(),src->sd_gen)) fclose(src->sd);
    src->sd=NULL;
}

int32_t pocket_fs_read_all(const char *path, uint8_t *out, uint32_t cap,
                           const char **code) {
    fs_path_t p;
    fs_file_t src;
    *code=native_reader(path,&p,&src);
    if(*code) return -1;
    int32_t answer=-1;
    // A size question is answered before the cap is applied, so a caller can
    // tell "too big for me" from "too big for you" and say so in its own words.
    if(!out) { answer=(int32_t)src.size; goto done; }
    if(src.size>cap) { *code=POCKET_ERR_LIMIT_EXCEEDED; goto done; }
    uint32_t at=0;
    while(at<src.size) {
        int n=file_bytes(&src,at,src.size-at,out+at,code);
        if(n<=0) { if(!*code) *code=POCKET_ERR_CORRUPT_DATA; goto done; }
        at+=(uint32_t)n;
    }
    answer=(int32_t)at;
done:
    native_reader_close(&src);
    return answer;
}

// The same, from an offset and bounded, for a caller that streams rather than
// loads. A player that held its whole source made its buffer the ceiling on a
// clip's length; this one keeps the path and re-reads a slot at a time.
//
// Re-resolving per call is deliberate and is the contract: a source deleted,
// replaced or on a card that has gone ends the stream as a read failure instead
// of playing bytes from something else. What it costs on sd: is a directory
// walk and an open per call, and at 400 kHz that is not free -- see the note in
// pocket_fs.h before building a demonstration around it.
//
// file_bytes() already takes a position and already refuses to span two blocks,
// so this is the shared resolution and a loop.
int32_t pocket_fs_read_at(const char *path, uint32_t offset, uint8_t *out,
                          uint32_t want, const char **code) {
    if(!out) { *code=POCKET_ERR_INVALID_ARGUMENT; return -1; }
    fs_path_t p;
    fs_file_t src;
    *code=native_reader(path,&p,&src);
    if(*code) return -1;
    int32_t answer=-1;
    if(offset>=src.size) { answer=0; goto done; }
    if(want>src.size-offset) want=src.size-offset;
    uint32_t at=0;
    while(at<want) {
        int n=file_bytes(&src,offset+at,want-at,out+at,code);
        if(n<0) { if(!*code) *code=POCKET_ERR_CORRUPT_DATA; goto done; }
        if(!n) break;                     // end of file inside the request
        at+=(uint32_t)n;
    }
    answer=(int32_t)at;
done:
    native_reader_close(&src);
    return answer;
}

static JSValue js_read_text(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="fs.readText";
    fs_path_t p;
    JSValue bad=take_path(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&p);
    if(!JS_IsUndefined(bad)) return bad;
    JSValueConst opt=argc>1?argv[1]:JS_UNDEFINED;
    fs_options_t options;
    bad=take_options(ctx,opt,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    // Section 7 makes maxBytes required, so an app has to say how much of its
    // heap it is willing to spend before the host spends any of it.
    if(!JS_IsObject(opt))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "maxBytes is required",false,NULL);
    int64_t cap=0;
    if(!take_int(ctx,opt,"maxBytes",OP,1,FS_TEXT_MAX,0,&cap,&bad)) return bad;
    if(!cap) return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                      "maxBytes is required",false,NULL);
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,"cancelled",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(p.volume==VOL_SD) {
        if(!p.depth) return pocket_api_reject(ctx,FS_ERR_IS_DIRECTORY,OP,
                                              "this path is a directory",false,NULL);
        return sd_read_text(ctx,&p,OP,(uint32_t)cap);
    }

    fs_file_t src={.mode=MODE_READ,.volume=p.volume,.asset=-1,.verified=-1};
    if(p.volume==VOL_ASSETS) {
        int a=asset_find(&p);
        if(a<0) return pocket_api_reject(ctx,POCKET_ERR_NOT_FOUND,OP,
                                         "no such asset",false,NULL);
        src.asset=(int8_t)a; src.size=asset_size(a);
    } else {
        if(!p.depth) return pocket_api_reject(ctx,FS_ERR_IS_DIRECTORY,OP,
                                              "this path is a directory",false,NULL);
        const char *why=NULL;
        uint16_t id=resolve(&p,false,NULL,&why);
        if(!id) return pocket_api_reject(ctx,why?why:POCKET_ERR_NOT_FOUND,OP,
                                         "no such file",false,NULL);
        if(store->dir[id-1].kind==FS_KIND_DIR)
            return pocket_api_reject(ctx,FS_ERR_IS_DIRECTORY,OP,
                                     "this path is a directory",false,NULL);
        const fs_obj_t *o=object(id);
        if(!o) return pocket_api_reject(ctx,POCKET_ERR_CORRUPT_DATA,OP,
                                        "this file has no readable inode",
                                        false,NULL);
        src.obj=id; src.size=o->size;
        memcpy(src.sector,o->sector,sizeof(src.sector));
    }
    // Section 7: over the limit is a refusal, never a silent truncation.
    if(src.size>(uint32_t)cap)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "the file is longer than maxBytes",false,NULL);

    char *text=malloc(src.size?src.size:1);
    if(!text) return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                       "no memory to read the file",true,NULL);
    const char *code=NULL;
    uint32_t at=0;
    while(at<src.size) {
        int n=file_bytes(&src,at,src.size-at,(uint8_t *)text+at,&code);
        if(n<=0) { free(text); break; }
        at+=(uint32_t)n;
    }
    if(at<src.size)
        return pocket_api_reject(ctx,code?code:POCKET_ERR_CORRUPT_DATA,OP,
                                 "the file could not be read",false,NULL);
    if(!utf8_valid((const uint8_t *)text,src.size)) {
        free(text);
        return pocket_api_reject(ctx,POCKET_ERR_CORRUPT_DATA,OP,
                                 "the file is not well-formed UTF-8",false,NULL);
    }
    // No BOM stripping and no newline conversion: section 7 forbids both.
    JSValue s=JS_NewStringLen(ctx,text,src.size);
    free(text);
    return pocket_api_settled(ctx,s,JS_IsException(s));
}

static JSValue js_write_text(JSContext *ctx, JSValueConst self,
                             int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="fs.writeText";
    fs_path_t p;
    JSValue bad=take_path(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&p);
    if(!JS_IsUndefined(bad)) return bad;
    bad=writable(ctx,&p,OP);
    if(!JS_IsUndefined(bad)) return bad;
    if(argc<2||!JS_IsString(argv[1]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "text must be a string",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValueConst opt=argc>2?argv[2]:JS_UNDEFINED;
    fs_options_t options;
    bad=take_options(ctx,opt,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    char mode_text[16];
    if(!take_string(ctx,opt,"mode",OP,mode_text,sizeof(mode_text),&bad)) return bad;
    bool replace=!strcmp(mode_text,"replace");
    if(mode_text[0]&&!replace&&strcmp(mode_text,"create"))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "mode must be create or replace",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    char want[24];
    if(!take_string(ctx,opt,"ifRevision",OP,want,sizeof(want),&bad)) return bad;
    bool wrong=false;
    JSValue d=take_durability(ctx,opt,OP,p.volume==VOL_SD,&wrong,&bad);
    if(wrong) return d;
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,"cancelled",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    size_t len=0;
    const char *text=JS_ToCStringLen(ctx,&len,argv[1]);
    if(!text) return JS_EXCEPTION;
    if(len>FS_TEXT_MAX||!utf8_valid((const uint8_t *)text,len)) {
        bool big=len>FS_TEXT_MAX;
        JS_FreeCString(ctx,text);
        return pocket_api_reject(ctx,
            big?POCKET_ERR_LIMIT_EXCEEDED:POCKET_ERR_INVALID_ARGUMENT,OP,
            big?"writeText takes at most 8192 UTF-8 bytes"
               :"the text is not well-formed UTF-8",
            false,POCKET_OUTCOME_NOT_APPLIED);
    }
    if(p.volume==VOL_SD) {
        JSValue done=sd_write_text(ctx,&p,OP,text,len,replace,want);
        JS_FreeCString(ctx,text);
        return done;
    }

    JSValue answer=JS_UNDEFINED;
    uint16_t parent=0;
    const char *why=NULL;
    resolve(&p,true,&parent,&why);
    size_t name_len=0;
    const char *leaf=path_leaf(&p,&name_len);
    uint16_t id=why?0:child_of(parent,leaf,name_len);
    fs_file_t w={.mode=MODE_CREATE,.volume=VOL_APP,.asset=-1,.parent=parent,
                 .verified=-1,.next_index=1,.gen=1};
    memset(w.sector,FS_NO_SECTOR,sizeof(w.sector));
    if(why) {
        answer=pocket_api_reject(ctx,why,OP,"the containing directory is not there",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
        goto done;
    }
    if(id&&store->dir[id-1].kind==FS_KIND_DIR) {
        answer=pocket_api_reject(ctx,FS_ERR_IS_DIRECTORY,OP,
                                 "this path is a directory",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
        goto done;
    }
    // Section 7: never an implicit overwrite, and replace needs a target.
    if(!replace&&id) {
        answer=pocket_api_reject(ctx,FS_ERR_ALREADY_EXISTS,OP,
                                 "this file already exists",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
        goto done;
    }
    if(replace&&!id) {
        answer=pocket_api_reject(ctx,POCKET_ERR_NOT_FOUND,OP,"no such file",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
        goto done;
    }
    if(id&&want[0]) {
        char have[24];
        revision_of_object(have,id);
        if(strcmp(have,want)) {
            answer=pocket_api_reject(ctx,POCKET_ERR_CONFLICT,OP,
                                     "the revision has moved on",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
            goto done;
        }
    }
    if(file_busy(VOL_APP,id,parent,leaf,name_len,true)) {
        answer=pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "this file is open elsewhere",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
        goto done;
    }
    if(id) {
        const fs_obj_t *o=object(id);
        if(!o) {
            answer=pocket_api_reject(ctx,POCKET_ERR_CORRUPT_DATA,OP,
                                     "this file has no readable inode",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
            goto done;
        }
        w.obj=id; w.gen=o->gen+1; w.mode=MODE_REPLACE;
    }
    else {
        w.obj=object_claim();
        if(!w.obj) {
            answer=pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                     "the store holds at most 24 objects",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
            goto done;
        }
    }
    w.buf=malloc(FS_BLOCK_PAYLOAD);
    if(!w.buf) {
        if(!id) object_unclaim(w.obj);
        answer=pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                 "no memory for the write buffer",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
        goto done;
    }
    // open -> write -> commit, exactly as section 7 describes, with the whole
    // sequence inside one call so no half-written state is ever reachable.
    if(!writer_feed(&w,(const uint8_t *)text,len)||
       (w.buf_len&&!writer_put_block(&w))||
       !inode_write(w.obj,FS_KIND_FILE,parent,leaf,name_len,w.gen,w.size,
                    w.sector,writer_blocks(&w))) {
        writer_discard(&w);
        if(!id) object_unclaim(w.obj);
        free(w.buf);
        answer=pocket_api_reject(ctx,FS_ERR_QUOTA,OP,
                                 "the store has no room for this file",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
        goto done;
    }
    free(w.buf);
    writer_published(&w);
    pending[w.obj-1]=0;
    answer=pocket_api_settled(ctx,entry_of_object(ctx,p.text,w.obj),false);
done:
    JS_FreeCString(ctx,text);
    return answer;
}

// --------------------------------------------------------------- sd: backend
//
// STATE OF THIS SURFACE, 2026-09-08. The four steps the previous handover
// listed are done, in the order it gave, and this is what they came to.
//
// Reachable from an app, end to end:
//
//   fs.requestFolder("sd") -> the host screen in sd_picker.c; the person's
//                             choice IS the grant, and it is the only thing
//                             that mounts a card in this firmware
//   fs.stat / fs.list      -> sd_stat(), sd_list_page(), paged, cursor scoped
//                             by media generation
//   fs.open                -> sd_open(): read, create, replace, append
//   read / seek / write / flush / commit / close on those handles
//   fs.readText / fs.writeText / fs.mkdir / fs.remove / fs.rename
//   fs.volumes / fs.space  -> volume_info(), the second one touching f_getfree
//   the capability          -> supported=true, available observed by sd_probe()
//
// NOT here, and each for a reason rather than for lack of time:
//
//   fs.copy with an sd: end. Section 4 allows it; a whole-file transfer over a
//   400 kHz bus inside one JS turn would hold the frame for as long as the file
//   takes, and the paced version is the job API section 8 defers. It refuses by
//   name so an app is told rather than stalled.
//
//   A per-frame pump, which fs.onVolumeChange needs and this surface has never
//   had. See the comment above volume_table for what that costs an app today.
//
//   crashSafeReplace. Still false, and section 6 says what would change it: a
//   power-cut test this board has not been through. Everything else about
//   replace is real -- a temporary beside the target, published by a rename --
//   and the two flags being different is the whole point of section 6 keeping
//   them apart.
//

// One thing that IS here and was not planned: the native readers below --
// pocket_fs_read_all() and pocket_fs_read_at(), which pocket_av.c's player uses
// -- now serve sd: as well. A native surface reading the card is not borrowing
// a permission it does not have: docs/filesystem-api.md section 9 puts the
// player's permission check and read on this side deliberately, and the grant
// it uses is the one the person made for the app the player is running inside.
// It goes through the same ordering an app's call does, in native_reader(),
// which exists precisely so there is one set of rules and not two.
//
// WHAT HAS RUN ON HARDWARE, AND WHAT HAS NOT. Read this before quoting any of
// the numbers below, and before assuming a path works because it is written.
//
//   Run on a board:  the driver underneath -- mount, unmount, root enumeration,
//                    a file created, written and removed -- by the agent who
//                    brought sd_media.c up on 2026-09-08, and fs.stat / fs.list
//                    as they stood that day.
//   NOT run:         everything this file gained afterwards. The picker has
//                    never been drawn on a screen. open, the writers, commit,
//                    mkdir/remove/rename, volumes/space, the pump, and the
//                    native readers on sd: are compiled, reviewed and
//                    host-tested where a host can reach them -- tools/test_sd.c
//                    covers the authorisation rules and nothing else can be
//                    tested without a card.
//   Changed under a path that HAD worked: the list cursor now counts entries
//                    consumed rather than entries returned (they differ as soon
//                    as one is skipped), listings filter the temporary suffix,
//                    and entry.accessible is computed instead of always true.
//                    If something on a card misbehaves, start there.
//
// apps/pocketfs/pocketfs.js is the automated half and needs no card; its README
// carries the five steps that need a card and a person, including the one that
// checks the durability refusal names the value it wants.
//
// What it costs. Measured on the board 2026-09-08, before the capability
// flipped: mount 7,032 bytes of heap, two open handles 1,640 more, unmount
// returns all of it, leak 0 across three cycles, on a 64 GB SDHC at 400 kHz
// with 512-byte sectors. NONE of that is paid by an app that never asks for a
// folder -- the mount happens in the picker and nowhere else -- but the mount
// path, the card struct and the VFS registration are now REACHABLE, so
// --gc-sections keeps them and the static cost of this build is not the +48
// bytes that was measured while they were dead. That number wants taking again
// against this build; it is the first figure from this work that means
// anything.
//
// Two things that must not be tidied away, both of them in sd_path.h's header
// at length: the grant is checked BEFORE the media state, and a removal drops
// the grant. tools/test_sd.c fails if either goes, and the reserved-suffix
// check added with the write path is in the same test for the same reason --
// deleting it produces five real escape paths in the output.

// A third backend, and deliberately not threaded through the machinery above.
// app: addresses objects by inode id and assets: by table index; sd: addresses
// them by path, through the POSIX layer esp_vfs_fat publishes. Forcing a FAT
// directory into the object-id model would mean inventing ids for entries this
// firmware does not own and cannot keep stable, so the operations that differ
// are written out separately and the ones that do not -- entry_new, path_join,
// the cursor table -- are shared as they stand.

// Translates a parsed sd: path into the FatFs path under the granted folder, or
// produces the rejection. Returns JS_UNDEFINED when `out` is filled.
static JSValue sd_fspath(JSContext *ctx, const fs_path_t *p, const char *op,
                         char *out, size_t outsz) {
    const char *rel = p->depth ? p->text + p->off[0] : "";
    size_t      len = p->depth ? (size_t)(p->len - p->off[0]) : 0;
    switch(sd_path_build(sd_media(),rel,len,out,outsz)) {
    case SD_PATH_OK:
        return JS_UNDEFINED;
    case SD_PATH_NO_GRANT:
        return pocket_api_reject(ctx,POCKET_ERR_PERMISSION_DENIED,op,
                                 "no folder on sd: has been granted to this app",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    case SD_PATH_DISCONNECTED:
        return pocket_api_reject(ctx,POCKET_ERR_DISCONNECTED,op,
                                 "the card is not mounted",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    case SD_PATH_TOO_LONG:
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                 "path is too long for this volume",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    default:
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                 "path is not a portable path",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
}

// errno at the moment a POSIX call failed, as the nearest section 8 code. ENOENT
// is the only one an app should routinely see; the rest are the card answering
// badly, which is CORRUPT_DATA or IO_ERROR and not the app's fault.
static const char *sd_errno_code(int e) {
    switch(e) {
    case ENOENT:  return POCKET_ERR_NOT_FOUND;
    case EACCES:  return POCKET_ERR_PERMISSION_DENIED;
    case EEXIST:  return FS_ERR_ALREADY_EXISTS;
    case ENOTDIR: return FS_ERR_NOT_DIRECTORY;
    case EISDIR:  return FS_ERR_IS_DIRECTORY;
    case ENOSPC:  return FS_ERR_NO_SPACE;
    case ENFILE:
    case EMFILE:  return POCKET_ERR_LIMIT_EXCEEDED;
    default:      return POCKET_ERR_IO_ERROR;
    }
}

// A FAT timestamp is local time with no zone, and this firmware has no way to
// know which zone the writing PC was in. Section 4 says an unavailable time is
// null rather than a guess, so mtime is reported only as what stat() gives and
// never adjusted.
// Section 4: an opaque token valid within a mount generation. Size and mtime
// are what the medium has; they move together with any write the host can
// observe, and nothing outside a generation compares them.
static void sd_revision(char out[24], const struct stat *st) {
    snprintf(out,24,"%u.%llu.%lld",(unsigned)sd_media()->generation,
             (unsigned long long)st->st_size,(long long)st->st_mtime);
}

static JSValue sd_entry(JSContext *ctx, const char *vpath, const char *name,
                        size_t name_len, const struct stat *st) {
    // The card is the one volume whose names this firmware did not make, so it
    // is the one place accessible can be false: a PC writes names this API
    // refuses, and a path that does not fit the 256-byte limit joins to "".
    bool reachable=name_ok(name,name_len)&&vpath[0];
    // Section 4: revision is an opaque token valid within a mount generation.
    // Size and mtime are what the medium has; they change together with any
    // write the host can observe, and nothing outside a generation compares
    // them.
    char rev[24];
    sd_revision(rev,st);
    return entry_new(ctx,vpath,name,name_len,S_ISDIR(st->st_mode),
                     (int64_t)st->st_size,(int64_t)st->st_mtime*1000,rev,
                     reachable);
}

static JSValue sd_stat(JSContext *ctx, const fs_path_t *p, const char *op) {
    char fspath[SD_FSPATH_MAX];
    JSValue bad=sd_fspath(ctx,p,op,fspath,sizeof fspath);
    if(!JS_IsUndefined(bad)) return bad;

    struct stat st;
    if(stat(fspath,&st)!=0) {
        int e=errno;
        sd_media_note_error(e==EIO?ESP_ERR_TIMEOUT:ESP_OK);
        return pocket_api_reject(ctx,sd_errno_code(e),op,
                                 "no such file or directory",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    const char *name = p->depth ? p->text+p->off[p->depth-1] : "";
    size_t      nlen = p->depth ? p->size[p->depth-1] : 0;
    return pocket_api_settled(ctx,sd_entry(ctx,p->text,name,nlen,&st),false);
}

// One page of a directory. `at` is how many entries to skip, which is the only
// cursor a FAT directory can offer without holding the DIR open across calls --
// and holding it open would pin a handle for 30 seconds of app inactivity.
// Re-opening and skipping is O(n) per page and honest about what it costs; the
// page limit is what bounds it.
static int sd_list_page(JSContext *ctx, const fs_path_t *p, uint16_t at,
                        unsigned limit, JSValue entries, bool *more, int *err,
                        uint16_t *next_at) {
    char fspath[SD_FSPATH_MAX];
    if(sd_path_build(sd_media(),
                     p->depth?p->text+p->off[0]:"",
                     p->depth?(size_t)(p->len-p->off[0]):0,
                     fspath,sizeof fspath)!=SD_PATH_OK) { *err=EINVAL; return 0; }

    DIR *d=opendir(fspath);
    if(!d) { *err=errno; return 0; }

    int emitted=0; uint16_t seen=0;
    struct dirent *de;
    char child[SD_FSPATH_MAX];
    char vpath[FS_MAX_PATH+1];
    *more=false;
    while((de=readdir(d))!=NULL) {
        // The cursor counts entries CONSUMED, not entries returned. They differ
        // whenever one is skipped below, and counting the returned ones would
        // make the next page start short and repeat what this one skipped.
        if(seen<at) { seen++; continue; }
        if((unsigned)emitted>=limit) { *more=true; break; }
        seen++;
        size_t nlen=strlen(de->d_name);
        // Section 2: the host's own temporary files are not listed. They are
        // also not addressable -- sd_path_build refuses the suffix -- so this
        // is the listing half of one rule rather than a rule of its own.
        if(sd_name_reserved(de->d_name,nlen)) continue;
        // A PC writes names this firmware would refuse to create. They are
        // listed rather than hidden -- an app that cannot see a file cannot
        // explain why it cannot open it -- and entry.accessible is what says
        // whether it can be used.
        struct stat st;
        int n=snprintf(child,sizeof child,"%s/%s",fspath,de->d_name);
        if(n<0||(size_t)n>=sizeof child) continue;
        if(stat(child,&st)!=0) continue;
        path_join(vpath,p,de->d_name,nlen);
        JS_SetPropertyUint32(ctx,entries,(uint32_t)emitted++,
                             sd_entry(ctx,vpath,de->d_name,nlen,&st));
    }
    closedir(d);
    *err=0;
    *next_at=seen;
    return emitted;
}


// --------------------------------------------------------------- sd: writes
//
// Section 6, for the volume it costs the most on. FAT has no journal and this
// board has not been through a power-cut test, so crashSafeReplace is false and
// stays false until it has. atomicReplace is true and is earned rather than
// assumed: a create or replace stages into a temporary beside the target and
// publishes it with a rename, so no read and no listing ever meets a
// half-written file.
//
// FatFs' rename refuses an existing destination, so a replace is
// unlink-then-rename rather than one call. The gap between the two is not
// observable THROUGH THIS API -- one JS turn on a single-threaded host, with no
// other operation able to start inside it -- and a power cut in that gap is
// precisely what crashSafeReplace=false is telling the app about. That is the
// whole of the difference between the two flags, written out because the next
// person here will wonder whether the unlink is a bug.
//
// The consequence an app meets first: per section 6 the DEFAULT durability for
// create and replace is crash-safe, this volume cannot give it, and so an
// open() that does not say durability:"synced" is refused. The refusal names
// the value it wants, because a spec-lookup UNSUPPORTED and a self-correcting
// one differ by one string.

static void sd_temp_path(const char *fspath, char *out, size_t outsz) {
    snprintf(out,outsz,"%s%s",fspath,SD_TEMP_SUFFIX);
}

// Section 5 needs to know when two handles mean the same file. A digest rather
// than the path: see fs_file_t.sd_key for why, and note that a collision
// refuses an operation that would have been allowed, never the other way round.
static uint32_t sd_key_of(const char *fspath) {
    return fnv1a(fspath,strlen(fspath));
}

static bool sd_busy(uint32_t key, bool writing) {
    for(int i=0;i<FS_MAX_HANDLES;i++) {
        fs_file_t *f=&files[i];
        if(!f->handle||f->volume!=VOL_SD||f->sd_key!=key) continue;
        if(writing||f->mode!=MODE_READ) return true;
    }
    return false;
}

// errno at the moment a POSIX call failed, turned into a rejection and into the
// removal signal this board has no pin for.
static JSValue sd_fail(JSContext *ctx, const char *op, int e, const char *what,
                       const char *outcome) {
    if(e==EIO) sd_media_note_error(ESP_ERR_TIMEOUT);
    return pocket_api_reject(ctx,sd_errno_code(e),op,what,e==EIO,outcome);
}

// Publishes what a writer staged. `fh` is closed whichever way this goes, and
// the answer is the Promise the call settles with.
static JSValue sd_publish(JSContext *ctx, const char *op, FILE *fh,
                          const char *fspath, const char *vpath, bool replacing) {
    char temp[SD_FSPATH_MAX+sizeof(SD_TEMP_SUFFIX)];
    sd_temp_path(fspath,temp,sizeof temp);
    // Sync before publish, so what the rename makes visible is what the card
    // holds rather than what the C library was still holding.
    int fd=fileno(fh);
    if(fflush(fh)!=0||(fd>=0&&fsync(fd)!=0)) {
        int e=errno;
        fclose(fh);
        unlink(temp);
        return sd_fail(ctx,op,e,"the new version could not be synchronised",
                       POCKET_OUTCOME_NOT_APPLIED);
    }
    if(fclose(fh)!=0) {
        int e=errno;
        unlink(temp);
        return sd_fail(ctx,op,e,"the new version could not be closed",
                       POCKET_OUTCOME_NOT_APPLIED);
    }
    if(replacing&&unlink(fspath)!=0&&errno!=ENOENT) {
        int e=errno;
        unlink(temp);
        return sd_fail(ctx,op,e,"the old version could not be replaced",
                       POCKET_OUTCOME_NOT_APPLIED);
    }
    if(rename(temp,fspath)!=0) {
        // The one path where the outcome is genuinely unknown: on a replace the
        // old version is already gone and the new one did not land. Section 6
        // forbids retrying automatically after an unknown, so this says so and
        // stops.
        int e=errno;
        unlink(temp);
        return sd_fail(ctx,op,e,"the new version could not be published",
                       replacing?POCKET_OUTCOME_UNKNOWN:POCKET_OUTCOME_NOT_APPLIED);
    }
    struct stat st;
    if(stat(fspath,&st)!=0)
        return sd_fail(ctx,op,errno,"the new version could not be read back",
                       POCKET_OUTCOME_APPLIED);
    const char *slash=strrchr(vpath,'/');
    const char *name=slash?slash+1:vpath;
    return pocket_api_settled(ctx,sd_entry(ctx,vpath,name,strlen(name),&st),false);
}

// fs.open on sd:. Fills the slot and answers with the File, or answers with the
// rejection and leaves the slot free.
static JSValue sd_open(JSContext *ctx, const fs_path_t *p, uint8_t mode,
                       const char *want, const char *op, fs_file_t *f) {
    char fspath[SD_FSPATH_MAX];
    JSValue bad=sd_fspath(ctx,p,op,fspath,sizeof fspath);
    if(!JS_IsUndefined(bad)) return bad;

    struct stat st;
    bool there=stat(fspath,&st)==0;
    if(there&&S_ISDIR(st.st_mode))
        return pocket_api_reject(ctx,FS_ERR_IS_DIRECTORY,op,
                                 "this path is a directory",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(mode==MODE_CREATE&&there)
        return pocket_api_reject(ctx,FS_ERR_ALREADY_EXISTS,op,
                                 "this file already exists",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(mode!=MODE_CREATE&&!there)
        return pocket_api_reject(ctx,POCKET_ERR_NOT_FOUND,op,"no such file",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    if(there&&(uint64_t)st.st_size>SD_MAX_FILE)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,op,
                                 "this file is larger than this API can address",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    if(there&&want[0]) {
        char have[24];
        sd_revision(have,&st);
        if(strcmp(have,want))
            return pocket_api_reject(ctx,POCKET_ERR_CONFLICT,op,
                                     "the revision has moved on",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
    }
    uint32_t key=sd_key_of(fspath);
    if(sd_busy(key,mode!=MODE_READ))
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,op,
                                 "this file is open elsewhere",true,
                                 POCKET_OUTCOME_NOT_APPLIED);

    memset(f,0,sizeof(*f));
    f->mode=mode; f->volume=VOL_SD; f->asset=-1; f->verified=-1;
    f->sd_key=key;
    // The generation this handle was made under. Every later use compares it,
    // which is where section 3's "a removal invalidates handles" is enforced.
    f->sd_gen=sd_media()->generation;
    size_t name_len=0;
    const char *leaf=path_leaf(p,&name_len);
    f->name_len=(uint8_t)name_len;
    memcpy(f->name,leaf,name_len);

    char temp[SD_FSPATH_MAX+sizeof(SD_TEMP_SUFFIX)];
    if(mode==MODE_CREATE||mode==MODE_REPLACE) {
        f->sd_w=calloc(1,sizeof(*f->sd_w));
        if(!f->sd_w) {
            memset(f,0,sizeof(*f));
            return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,op,
                                     "no memory for the write",true,
                                     POCKET_OUTCOME_NOT_APPLIED);
        }
        memcpy(f->sd_w->fspath,fspath,strlen(fspath)+1);
        memcpy(f->sd_w->vpath,p->text,(size_t)p->len+1);
        sd_temp_path(fspath,temp,sizeof temp);
        f->sd=fopen(temp,"wb");
    } else if(mode==MODE_APPEND) {
        f->sd=fopen(fspath,"ab");
        f->size=(uint32_t)st.st_size;
        f->pos=f->size;
    } else {
        f->sd=fopen(fspath,"rb");
        f->size=(uint32_t)st.st_size;
    }
    if(!f->sd) {
        int e=errno;
        free(f->sd_w);
        memset(f,0,sizeof(*f));
        return sd_fail(ctx,op,e,"the file could not be opened",
                       POCKET_OUTCOME_NOT_APPLIED);
    }
    f->handle=next_handle++;
    return pocket_api_settled(ctx,file_wrap(ctx,f),false);
}

// One write on an sd: handle. Section 5: the bytes are copied at acceptance --
// fwrite does that -- and a failure closes the writer, because the temporary is
// now of a length nobody knows.
static JSValue sd_file_write(JSContext *ctx, fs_file_t *f, const char *op,
                             const uint8_t *data, size_t len) {
    if((uint64_t)f->size+len>SD_MAX_FILE)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,op,
                                 "a file on this volume holds at most 2147483647 bytes",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    size_t done=fwrite(data,1,len,f->sd);
    if(done!=len) {
        int e=errno;
        f->broken=true;
        f->size+=(uint32_t)done;
        JSValue err=pocket_api_error(ctx,e==ENOSPC?FS_ERR_NO_SPACE:sd_errno_code(e),
                                     op,"the card would not take these bytes",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        JS_SetPropertyStr(ctx,err,"bytesTransferred",JS_NewInt64(ctx,(int64_t)done));
        if(e==EIO) sd_media_note_error(ESP_ERR_TIMEOUT);
        return pocket_api_settled(ctx,err,true);
    }
    f->size+=(uint32_t)len;
    f->pos=f->size;
    return pocket_api_settled(ctx,JS_NewInt64(ctx,(int64_t)len),false);
}

// fs.mkdir, fs.remove and fs.rename on sd:. Each is one POSIX call plus the
// checks section 4 asks for; none of them can reach outside the granted folder,
// because every path they take is built by sd_path_build first.
static JSValue sd_mkdir(JSContext *ctx, const fs_path_t *p, const char *op,
                        bool recursive) {
    char fspath[SD_FSPATH_MAX];
    JSValue bad=sd_fspath(ctx,p,op,fspath,sizeof fspath);
    if(!JS_IsUndefined(bad)) return bad;
    struct stat st;
    if(stat(fspath,&st)==0)
        // Section 4: an existing directory is a success, a file is a conflict.
        return S_ISDIR(st.st_mode)
            ? pocket_api_settled(ctx,JS_UNDEFINED,false)
            : pocket_api_reject(ctx,FS_ERR_ALREADY_EXISTS,op,
                                "a file already has that name",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    if(recursive) {
        // Every missing parent in turn. Section 4 gives no rollback for a
        // recursive mkdir that fails half way, and this does not invent one.
        for(char *at=fspath+1;*at;at++) {
            if(*at!='/') continue;
            *at='\0';
            int r=mkdir(fspath,0777);
            int e=errno;
            *at='/';
            if(r!=0&&e!=EEXIST)
                return sd_fail(ctx,op,e,"a parent directory could not be made",
                               POCKET_OUTCOME_UNKNOWN);
        }
    }
    if(mkdir(fspath,0777)!=0)
        return sd_fail(ctx,op,errno,"the directory could not be made",
                       POCKET_OUTCOME_NOT_APPLIED);
    return pocket_api_settled(ctx,JS_UNDEFINED,false);
}

static JSValue sd_remove(JSContext *ctx, const fs_path_t *p, const char *op,
                         const char *want) {
    char fspath[SD_FSPATH_MAX];
    JSValue bad=sd_fspath(ctx,p,op,fspath,sizeof fspath);
    if(!JS_IsUndefined(bad)) return bad;
    struct stat st;
    if(stat(fspath,&st)!=0)
        return sd_fail(ctx,op,errno,"no such file or directory",
                       POCKET_OUTCOME_NOT_APPLIED);
    if(want[0]) {
        char have[24];
        sd_revision(have,&st);
        if(strcmp(have,want))
            return pocket_api_reject(ctx,POCKET_ERR_CONFLICT,op,
                                     "the revision has moved on",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
    }
    if(sd_busy(sd_key_of(fspath),true))
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,op,
                                 "a handle is open on this path",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    // Section 4: files and EMPTY directories, and a non-empty one is NOT_EMPTY
    // rather than the recursive delete this version does not have.
    int r=S_ISDIR(st.st_mode)?rmdir(fspath):unlink(fspath);
    if(r!=0) {
        int e=errno;
        if(S_ISDIR(st.st_mode)&&(e==ENOTEMPTY||e==EACCES||e==EEXIST))
            return pocket_api_reject(ctx,FS_ERR_NOT_EMPTY,op,
                                     "this directory is not empty",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        return sd_fail(ctx,op,e,"this path could not be removed",
                       POCKET_OUTCOME_NOT_APPLIED);
    }
    return pocket_api_settled(ctx,JS_UNDEFINED,false);
}

static JSValue sd_rename(JSContext *ctx, const fs_path_t *from,
                         const fs_path_t *to, const char *op, const char *want) {
    char a[SD_FSPATH_MAX],b[SD_FSPATH_MAX];
    JSValue bad=sd_fspath(ctx,from,op,a,sizeof a);
    if(!JS_IsUndefined(bad)) return bad;
    bad=sd_fspath(ctx,to,op,b,sizeof b);
    if(!JS_IsUndefined(bad)) return bad;
    struct stat st;
    if(stat(a,&st)!=0)
        return sd_fail(ctx,op,errno,"no such file or directory",
                       POCKET_OUTCOME_NOT_APPLIED);
    if(want[0]) {
        char have[24];
        sd_revision(have,&st);
        if(strcmp(have,want))
            return pocket_api_reject(ctx,POCKET_ERR_CONFLICT,op,
                                     "the revision has moved on",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
    }
    struct stat dst;
    if(stat(b,&dst)==0)
        // Section 4: rename never overwrites.
        return pocket_api_reject(ctx,FS_ERR_ALREADY_EXISTS,op,
                                 "something already has that name",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(sd_busy(sd_key_of(a),true)||sd_busy(sd_key_of(b),true))
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,op,
                                 "a handle is open on this path",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(rename(a,b)!=0)
        return sd_fail(ctx,op,errno,"the new name could not be written",
                       POCKET_OUTCOME_NOT_APPLIED);
    return pocket_api_settled(ctx,JS_UNDEFINED,false);
}

// fs.readText on sd:, section 7's convenience over a read handle. Written out
// separately only because the handle table is not involved.
static JSValue sd_read_text(JSContext *ctx, const fs_path_t *p, const char *op,
                            uint32_t cap) {
    char fspath[SD_FSPATH_MAX];
    JSValue bad=sd_fspath(ctx,p,op,fspath,sizeof fspath);
    if(!JS_IsUndefined(bad)) return bad;
    struct stat st;
    if(stat(fspath,&st)!=0)
        return sd_fail(ctx,op,errno,"no such file",POCKET_OUTCOME_NOT_APPLIED);
    if(S_ISDIR(st.st_mode))
        return pocket_api_reject(ctx,FS_ERR_IS_DIRECTORY,op,
                                 "this path is a directory",false,NULL);
    // Section 7: over the limit is a refusal, never a silent truncation, and it
    // is answered from the size before a byte reaches the guest's heap.
    if((uint64_t)st.st_size>cap)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,op,
                                 "the file is longer than maxBytes",false,NULL);
    size_t size=(size_t)st.st_size;
    char *text=malloc(size?size:1);
    if(!text) return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,op,
                                       "no memory to read the file",true,NULL);
    FILE *fh=fopen(fspath,"rb");
    if(!fh) {
        int e=errno;
        free(text);
        return sd_fail(ctx,op,e,"the file could not be opened",
                       POCKET_OUTCOME_NOT_APPLIED);
    }
    size_t got=size?fread(text,1,size,fh):0;
    bool failed=got!=size;
    if(failed&&ferror(fh)) sd_media_note_error(ESP_ERR_TIMEOUT);
    fclose(fh);
    if(failed) {
        free(text);
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,op,
                                 "the file could not be read",false,NULL);
    }
    if(!utf8_valid((const uint8_t *)text,size)) {
        free(text);
        return pocket_api_reject(ctx,POCKET_ERR_CORRUPT_DATA,op,
                                 "the file is not well-formed UTF-8",false,NULL);
    }
    JSValue str=JS_NewStringLen(ctx,text,size);
    free(text);
    return pocket_api_settled(ctx,str,JS_IsException(str));
}

// fs.writeText on sd:. open -> write -> commit, the whole sequence inside one
// call so no half-written state is ever reachable -- the same shape the app:
// side has, over a different backend.
static JSValue sd_write_text(JSContext *ctx, const fs_path_t *p, const char *op,
                             const char *text, size_t len, bool replace,
                             const char *want) {
    char fspath[SD_FSPATH_MAX];
    JSValue bad=sd_fspath(ctx,p,op,fspath,sizeof fspath);
    if(!JS_IsUndefined(bad)) return bad;
    struct stat st;
    bool there=stat(fspath,&st)==0;
    if(there&&S_ISDIR(st.st_mode))
        return pocket_api_reject(ctx,FS_ERR_IS_DIRECTORY,op,
                                 "this path is a directory",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    // Section 7: never an implicit overwrite, and replace needs a target.
    if(!replace&&there)
        return pocket_api_reject(ctx,FS_ERR_ALREADY_EXISTS,op,
                                 "this file already exists",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(replace&&!there)
        return pocket_api_reject(ctx,POCKET_ERR_NOT_FOUND,op,"no such file",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    if(there&&want[0]) {
        char have[24];
        sd_revision(have,&st);
        if(strcmp(have,want))
            return pocket_api_reject(ctx,POCKET_ERR_CONFLICT,op,
                                     "the revision has moved on",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
    }
    if(sd_busy(sd_key_of(fspath),true))
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,op,
                                 "this file is open elsewhere",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    char temp[SD_FSPATH_MAX+sizeof(SD_TEMP_SUFFIX)];
    sd_temp_path(fspath,temp,sizeof temp);
    FILE *fh=fopen(temp,"wb");
    if(!fh) return sd_fail(ctx,op,errno,"the file could not be opened",
                           POCKET_OUTCOME_NOT_APPLIED);
    if(len&&fwrite(text,1,len,fh)!=len) {
        int e=errno;
        fclose(fh);
        unlink(temp);
        return sd_fail(ctx,op,e,"the card would not take these bytes",
                       POCKET_OUTCOME_NOT_APPLIED);
    }
    return sd_publish(ctx,op,fh,fspath,p->text,there);
}

// ------------------------------------------------------------ capabilities
//
// Three names from section 2, and the third is the one worth reading.
//
// fs.volume.sd is supported=true as of this build, and the bar it had to clear
// was not "a card mounts". It mounted on 2026-09-08 -- 64 GB SDHC, 512-byte
// sectors, FATFS at 400 kHz over the pin map in docs/hardware-constraints.md
// (CS=12, MOSI=14, CLK=40, MISO=39, on the SPI bus the EXT connector shares),
// root enumerated, files written and removed -- and the capability stayed false
// for months afterwards on purpose. `supported` says whether an APP can use
// this surface, not whether the driver works: flipping it while nothing in this
// file reached the driver would have put an app that feature-tested its way in
// through an UNSUPPORTED-shaped failure at every call, which is precisely the
// failure section 2 exists to prevent.
//
// It flips now because the picker, open, read, write, volumes and space all
// answer. `limits` below reports only values this file actually enforces, which
// is why there is no quotaBytes on the card and why maxFileBytes is there.
//
// One hardware fact the surface does not change: there is NO card-detect pin in
// the map. Removal is not an event this firmware can receive, only the shape of
// a command that failed. available is therefore observed -- sd_probe() reports
// whether a granted card is answering, and asks the hardware nothing -- and
// nothing polls, because polling would invent a liveness the hardware cannot
// report.

static const pocket_limit_t app_limits[] = {
    {.name="maxPathBytes",    .kind=POCKET_LIMIT_INT, .number=FS_MAX_PATH},
    {.name="maxNameBytes",    .kind=POCKET_LIMIT_INT, .number=FS_MAX_NAME},
    {.name="maxDepth",        .kind=POCKET_LIMIT_INT, .number=FS_MAX_DEPTH},
    {.name="maxObjects",      .kind=POCKET_LIMIT_INT, .number=FS_MAX_OBJECTS},
    {.name="maxOpenFiles",    .kind=POCKET_LIMIT_INT, .number=FS_MAX_HANDLES},
    {.name="maxCursors",      .kind=POCKET_LIMIT_INT, .number=FS_MAX_CURSORS},
    {.name="chunkBytes",      .kind=POCKET_LIMIT_INT, .number=FS_CHUNK},
    {.name="listLimit",       .kind=POCKET_LIMIT_INT, .number=FS_LIST_MAX},
    {.name="quotaBytes",      .kind=POCKET_LIMIT_INT, .number=FS_QUOTA_BYTES},
    {.name="maxFileBytes",    .kind=POCKET_LIMIT_INT, .number=(int32_t)FS_MAX_FILE},
    {.name="maxTextBytes",    .kind=POCKET_LIMIT_INT, .number=FS_TEXT_MAX},
    {.name="maxTimeoutMs",    .kind=POCKET_LIMIT_INT, .number=FS_MAX_TIMEOUT_MS},
    // Charged in whole sectors, so a one-byte file costs two of them: one for
    // the data and one for the metadata that publishes it. An app dividing
    // quotaBytes by its record size has to know this.
    {.name="allocationBytes", .kind=POCKET_LIMIT_INT, .number=FS_SECTOR},
    {.name="durability",      .kind=POCKET_LIMIT_TEXT,.text="synced"},
    {.kind=POCKET_LIMIT_END},
};

static const pocket_limit_t assets_limits[] = {
    {.name="maxPathBytes", .kind=POCKET_LIMIT_INT, .number=FS_MAX_PATH},
    {.name="maxOpenFiles", .kind=POCKET_LIMIT_INT, .number=FS_MAX_HANDLES},
    {.name="chunkBytes",   .kind=POCKET_LIMIT_INT, .number=FS_CHUNK},
    {.name="maxTextBytes", .kind=POCKET_LIMIT_INT, .number=FS_TEXT_MAX},
    {.name="files",        .kind=POCKET_LIMIT_INT, .number=(int32_t)ASSET_COUNT},
    {.kind=POCKET_LIMIT_END},
};

// available is an observation, so the probe mounts once and reports what it
// found rather than a hopeful constant.
static void app_probe(const pocket_capability_t *capability, bool *available,
                      const char **reason) {
    (void)capability;
    *available=mount();
    *reason=*available?NULL:POCKET_REASON_DISABLED;
}

static const pocket_capability_t app_capability = {
    .name="fs.volume.app", .supported=true, .available=true,
    .limits=app_limits, .probe=app_probe,
};
static const pocket_capability_t assets_capability = {
    .name="fs.volume.assets", .supported=true, .available=true,
    .limits=assets_limits,
};
// Only what this file actually enforces, which is why there is no quotaBytes
// here: nothing charges an app for what it writes to the card. maxFileBytes IS
// enforced -- sd_open() refuses a larger file -- and it is also the largest
// size the uint32 positions in this file can address.
static const pocket_limit_t sd_limits[] = {
    {.name="maxPathBytes",  .kind=POCKET_LIMIT_INT, .number=FS_MAX_PATH},
    {.name="maxNameBytes",  .kind=POCKET_LIMIT_INT, .number=FS_MAX_NAME},
    {.name="maxDepth",      .kind=POCKET_LIMIT_INT, .number=FS_MAX_DEPTH},
    {.name="maxOpenFiles",  .kind=POCKET_LIMIT_INT, .number=SD_MAX_OPEN_FILES},
    {.name="maxCursors",    .kind=POCKET_LIMIT_INT, .number=FS_MAX_CURSORS},
    {.name="chunkBytes",    .kind=POCKET_LIMIT_INT, .number=FS_CHUNK},
    {.name="listLimit",     .kind=POCKET_LIMIT_INT, .number=FS_LIST_MAX},
    {.name="maxFileBytes",  .kind=POCKET_LIMIT_INT, .number=(int32_t)SD_MAX_FILE},
    {.name="maxTextBytes",  .kind=POCKET_LIMIT_INT, .number=FS_TEXT_MAX},
    {.name="maxTimeoutMs",  .kind=POCKET_LIMIT_INT, .number=FS_MAX_TIMEOUT_MS},
    // The only durability create and replace accept here, and the value the
    // refusal of the default names.
    {.name="durability",    .kind=POCKET_LIMIT_TEXT,.text="synced"},
    // The grant comes from a host screen. An app that reads this knows there is
    // something to call and that it cannot name the folder itself.
    {.name="picker",        .kind=POCKET_LIMIT_FLAG,.number=1},
    {.kind=POCKET_LIMIT_END},
};

// Deliberately does NOT mount. available says whether a card the person has
// already granted is answering; before they have chosen, the answer is no
// without the hardware being asked -- which is also what keeps an app from
// learning whether there is a card in the slot at all.
static void sd_probe(const pocket_capability_t *capability, bool *available,
                     const char **reason) {
    (void)capability;
    *available=sd_media_usable(sd_media());
    *reason=*available?NULL:POCKET_REASON_DISABLED;
}

static const pocket_capability_t sd_capability = {
    .name="fs.volume.sd", .supported=true, .available=false,
    .reason=POCKET_REASON_DISABLED, .limits=sd_limits, .probe=sd_probe,
};

// ---------------------------------------------------------------- install

static const JSCFunctionListEntry file_methods[] = {
    JS_CFUNC_DEF("read",   2, js_file_read),
    JS_CFUNC_DEF("write",  2, js_file_write),
    JS_CFUNC_DEF("seek",   2, js_file_seek),
    JS_CFUNC_DEF("tell",   0, js_file_tell),
    JS_CFUNC_DEF("flush",  1, js_file_flush),
    JS_CFUNC_DEF("commit", 1, js_file_commit),
    JS_CFUNC_DEF("close",  0, js_file_close),
};

// No finalizer, on purpose: a File is a slot in a fixed table, and letting the
// collector close one would make an app that dropped its variable lose an open
// writer at an unpredictable moment. close(), commit() and pocket_fs_reset()
// are the three ways a handle ends -- the same rule pocket_ui.c states for its
// nodes.
static const JSClassDef file_class_def = { .class_name="PocketFile" };

static const JSCFunctionListEntry fs_methods[] = {
    JS_CFUNC_DEF("volumes",        0, js_volumes),
    JS_CFUNC_DEF("onVolumeChange", 1, js_on_volume_change),
    JS_CFUNC_DEF("space",          2, js_space),
    JS_CFUNC_DEF("stat",           2, js_stat),
    JS_CFUNC_DEF("list",           2, js_list),
    JS_CFUNC_DEF("mkdir",          2, js_mkdir),
    JS_CFUNC_DEF("remove",         2, js_remove),
    JS_CFUNC_DEF("rename",         3, js_rename),
    JS_CFUNC_DEF("copy",           3, js_copy),
    JS_CFUNC_DEF("open",           2, js_open),
    JS_CFUNC_DEF("readText",       2, js_read_text),
    JS_CFUNC_DEF("writeText",      3, js_write_text),
    JS_CFUNC_DEF("requestFolder", 2, sd_picker_request),
    JS_CFUNC_DEF("pickFile",      2, file_picker_request),
    JS_CFUNC_DEF("nextFile",      2, file_picker_next),
};

static esp_err_t build_fs(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    JSRuntime *rt=JS_GetRuntime(ctx);
    // Both halves live in pocket_api_class_ready(): the id must come from THIS
    // runtime's counter, and the registration must not be repeated when
    // pocket_api_lazy() retries a build that failed.
    //
    // This is the file where it was caught. "pocket.fs could not be built:
    // ESP_FAIL" with file_class=65, on a guest using 97,952 bytes of a
    // 163,840-byte ceiling -- and the guest was told "no memory", which is what
    // the lazy wrapper calls every failure. Two wrong diagnoses came out of
    // that message before the log printed the id.
    if(!pocket_api_class_ready(rt,&class_rt,&file_class)) {
        JS_NewClassID(rt,&file_class);
        if(JS_NewClass(rt,file_class,&file_class_def)<0) {
            ESP_LOGE(TAG,"JS_NewClass failed for file_class=%u",
                     (unsigned)file_class);
            return ESP_FAIL;
        }
    }
    JSValue proto=JS_NewObject(ctx);
    if(JS_IsException(proto)) return ESP_ERR_NO_MEM;
    JS_SetPropertyFunctionList(ctx,proto,file_methods,
                               (int)(sizeof(file_methods)/sizeof(file_methods[0])));
    // One shared prototype, so a File costs the guest one object with an
    // opaque. Per-handle closures would put seven function objects on every
    // open, and guest heap is what runs out on this board.
    JS_SetClassProto(ctx,file_class,proto);

    // Nothing survives a session: the realm going away takes the callbacks with
    // it, so every table starts empty.
    memset(files,0,sizeof(files));
    memset(pending,0,sizeof(pending));
    cursors_clear();
    volume_table.ctx=ctx;
    // The card's state at the start of the run is the baseline, not an event.
    sd_told_now();

    JS_SetPropertyFunctionList(ctx,ns,fs_methods,
                               (int)(sizeof(fs_methods)/sizeof(fs_methods[0])));
    built=true;
    return ESP_OK;
}

esp_err_t pocket_fs_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    // Eager: three capability entries, so a feature test answers without a
    // class, a prototype or nineteen function objects being made first.
    pocket_api_register(&app_capability);
    pocket_api_register(&assets_capability);
    pocket_api_register(&sd_capability);
    if(!owner_hash) pocket_fs_set_owner(NULL);
    return pocket_api_lazy(ctx,"fs",build_fs,NULL);
}

void pocket_fs_reset(void) {
    class_rt=NULL;
    // A folder or file screen still up goes back to the shell, and its
    // promise is settled by the completion picker_finish() posts.
    sd_picker_reset();
    file_picker_reset();
    // Section 8: everything is cancelled when the app ends, and a create or
    // replace that never committed loses its temporary version here. That is
    // flash erases at app_stop() -- up to seven sectors, tens of milliseconds
    // each -- and it is the price of not leaving a half-written file behind.
    //
    // BEFORE the unmount, and the order is not decoration: after it the media
    // generation has moved, and file_close() then declines to touch a FILE*
    // whose VFS is gone. Closing while the card is still mounted is what makes
    // an sd: temporary actually disappear.
    if(built)
        for(int i=0;i<FS_MAX_HANDLES;i++)
            if(files[i].handle) file_close(&files[i],true);
    // The card is one session's. Section 3 is explicit that a re-inserted card
    // has to go back through the picker, and sd_media_unmount() drops the grant
    // as it returns the 7,032 bytes the mount holds. Outside the `built` test
    // on purpose: a session that reached the picker must give the card back
    // even if it never built anything else.
    sd_media_unmount();
    // The unmount above moved the state, and there is nobody left to tell: the
    // guest is going away and its listeners with it. Recording it as announced
    // keeps the next session's baseline honest.
    sd_told_now();
    if(!built) return;   // no handle, no cursor, no index: nothing to give back
    built=false;
    memset(pending,0,sizeof(pending));
    cursors_clear();
    pocket_api_sub_close_all(&volume_table);
    volume_table.ctx=NULL;
    // And the index goes back. Keeping it would be cheaper for an app that uses
    // files twice in a row, but it would charge every LATER app 3080 bytes of
    // internal DRAM for a store it may never open -- and on this board the
    // scarce thing is the room a running app has, not the milliseconds a mount
    // costs. Rebuilding reads 64 sector headers of 24 bytes plus one small
    // payload per live object; nothing here reads a data block.
    free(store);
    store=NULL;
}
