#include "srcstore.h"
#include "esp_partition.h"
#include "esp_crc.h"
#include "esp_log.h"
#include <string.h>

// Record: magic, length, CRC32 of the text, then the text. Flash erases in
// 4 KB blocks and writes need 4-byte alignment, so the whole record is built
// in RAM and written as one aligned block.
// "SRC2". Version 1 had no seq and a 12-byte header, so its records sit four
// bytes out of place under this layout; the CRC would catch that, but only
// after newest() had already taken four bytes of source text for a sequence
// number. A new magic rejects them outright.
#define SRC_MAGIC 0x32435253u

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint32_t crc;
    uint32_t seq;     // higher wins; the two faces alternate
} src_hdr_t;

#define BLOCK (((sizeof(src_hdr_t)+SRC_MAX+4095)/4096)*4096)
// Two faces. A save erases one and writes it while the other still holds the
// previous version, so losing power mid-write costs the last save rather than
// the whole source. The cost is 12 KB of a 2.4 MB partition.
#define FACES 2
// A slot is its two faces, laid end to end. Slot 0 starts at offset 0, so the
// records written before slots existed are still where this looks for them.
#define SLOT_SPAN ((size_t)BLOCK*FACES)

static const esp_partition_t *storage(void) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,0x42,"storage");
}

// Byte offset of one face, or SIZE_MAX when it does not fit the partition.
static size_t face_at(const esp_partition_t *p, unsigned slot, unsigned face) {
    if(slot>=SRC_SLOT_COUNT) return SIZE_MAX;
    size_t off=(size_t)slot*SLOT_SPAN+(size_t)face*BLOCK;
    return (off+BLOCK<=p->size) ? off : SIZE_MAX;
}

// Reads one face into `out`. Returns false unless the record verifies.
static bool read_face(const esp_partition_t *p, unsigned slot, unsigned face,
                      char *out, src_hdr_t *hdr) {
    size_t base=face_at(p,slot,face);
    if(base==SIZE_MAX) return false;
    if(esp_partition_read(p,base,hdr,sizeof(*hdr))!=ESP_OK) return false;
    if(hdr->magic!=SRC_MAGIC || hdr->len>SRC_MAX) return false;
    if(esp_partition_read(p,base+sizeof(*hdr),out,hdr->len)!=ESP_OK) return false;
    out[hdr->len]=0;
    if(esp_crc32_le(0,(const uint8_t*)out,hdr->len)!=hdr->crc) {
        ESP_LOGW("src","slot %u face %u failed its CRC",slot,face);
        return false;
    }
    return true;
}

// Which face of a slot holds the newest good record, or -1 when neither does.
static int newest(const esp_partition_t *p, unsigned slot, uint32_t *out_seq) {
    int best=-1; uint32_t best_seq=0;
    for(unsigned f=0;f<FACES;f++) {
        size_t base=face_at(p,slot,f);
        if(base==SIZE_MAX) continue;
        src_hdr_t hdr;
        if(esp_partition_read(p,base,&hdr,sizeof(hdr))!=ESP_OK) continue;
        if(hdr.magic!=SRC_MAGIC || hdr.len>SRC_MAX) continue;
        if(best<0 || (int32_t)(hdr.seq-best_seq)>0) { best=(int)f; best_seq=hdr.seq; }
    }
    if(out_seq) *out_seq=best_seq;
    return best;
}

// Defined with the draft region at the end of this file; srcstore_clear uses
// them to drop a draft that belongs to the slot it is forgetting.
static bool draft_owner_of(unsigned *slot);

uint32_t srcstore_revision(unsigned slot) {
    const esp_partition_t *p=storage();
    if(!p) return 0;
    uint32_t seq=0;
    // A slot whose only face fails its CRC still has a sequence number, and
    // reporting it is right: the caller asks "has this moved since I read it",
    // and a record that went bad has moved.
    return newest(p,slot,&seq)<0 ? 0 : seq;
}

size_t srcstore_load(unsigned slot, char *out) {
    return srcstore_load_checked(slot,out,NULL);
}

size_t srcstore_load_checked(unsigned slot, char *out, bool *verified) {
    if(verified) *verified=false;
    out[0]=0;
    const esp_partition_t *p=storage();
    if(!p) return 0;
    // Try the newest face, then the other one: a half-written newest face
    // fails its CRC and the previous version is still there.
    uint32_t seq=0;
    int first=newest(p,slot,&seq);
    if(first<0) return 0;
    for(unsigned attempt=0;attempt<FACES;attempt++) {
        unsigned face=(unsigned)((first+attempt)%FACES);
        src_hdr_t hdr;
        if(read_face(p,slot,face,out,&hdr)) {
            ESP_LOGI("src","slot %u: loaded %u bytes from face %u seq %u",
                     slot,(unsigned)hdr.len,face,(unsigned)hdr.seq);
            if(verified) *verified=true;
            return hdr.len;
        }
    }
    out[0]=0;
    return 0;
}

bool srcstore_clear(unsigned slot) {
    const esp_partition_t *p=storage();
    if(!p) return false;
    // Forgetting a slot forgets its unsaved draft as well: a chapter reset or a
    // work taken back would otherwise leave a draft that the next open would
    // hand to whoever is given that slot next.
    unsigned parked=0;
    if(draft_owner_of(&parked) && parked==slot) srcstore_draft_clear();
    for(unsigned f=0;f<FACES;f++) {
        size_t base=face_at(p,slot,f);
        if(base==SIZE_MAX) return false;
        if(esp_partition_erase_range(p,base,BLOCK)!=ESP_OK) return false;
    }
    ESP_LOGI("src","slot %u cleared",slot);
    return true;
}

bool srcstore_save(unsigned slot, const char *text, size_t len) {
    if(len>SRC_MAX) return false;
    const esp_partition_t *p=storage();
    if(!p) return false;
    uint32_t seq=0;
    int current=newest(p,slot,&seq);
    unsigned face=(current<0)?0:(unsigned)((current+1)%FACES);
    size_t base=face_at(p,slot,face);
    if(base==SIZE_MAX) return false;

    // Flash writes want 4-byte offsets and lengths, so the body goes out in one
    // aligned run and the last few bytes ride in a padded word. Staging the
    // whole 12 KB block in RAM instead would cost more than the record is worth.
    src_hdr_t hdr={.magic=SRC_MAGIC,.len=(uint32_t)len,
                   .crc=esp_crc32_le(0,(const uint8_t*)text,len),
                   .seq=seq+1};
    size_t body=len & ~(size_t)3, tail=len-body;

    // The body lands before the header, so a record whose magic is present is
    // one whose bytes are too.
    esp_err_t err=esp_partition_erase_range(p,base,BLOCK);
    if(err==ESP_OK && body)
        err=esp_partition_write(p,base+sizeof(hdr),text,body);
    if(err==ESP_OK && tail) {
        uint8_t word[4]={0xff,0xff,0xff,0xff};
        memcpy(word,text+body,tail);
        err=esp_partition_write(p,base+sizeof(hdr)+body,word,sizeof(word));
    }
    if(err==ESP_OK) err=esp_partition_write(p,base,&hdr,sizeof(hdr));
    if(err!=ESP_OK) { ESP_LOGW("src","save failed: %s",esp_err_to_name(err)); return false; }
    ESP_LOGI("src","slot %u: saved %u bytes to face %u seq %u",
             slot,(unsigned)len,face,(unsigned)hdr.seq);
    return true;
}

// ---- the parked draft ------------------------------------------------------
//
// Same record shape as a slot's, with the owning slot appended and a magic of
// its own so that a draft can never be mistaken for a source record (or the
// other way round, if the two regions were ever to meet). The region sits at
// SRCSTORE_DRAFT_BASE, past pocket.fs -- see the note in srcstore.h.
#define DRAFT_MAGIC 0x31445253u   // "SRD1"

typedef struct {
    src_hdr_t base;
    uint32_t slot;
} draft_hdr_t;

static size_t draft_face_at(const esp_partition_t *p, unsigned face) {
    size_t off=(size_t)SRCSTORE_DRAFT_BASE+(size_t)face*BLOCK;
    return (off+BLOCK<=p->size) ? off : SIZE_MAX;
}

static bool draft_read(const esp_partition_t *p, unsigned face, char *out,
                       draft_hdr_t *hdr) {
    size_t base=draft_face_at(p,face);
    if(base==SIZE_MAX) return false;
    if(esp_partition_read(p,base,hdr,sizeof(*hdr))!=ESP_OK) return false;
    if(hdr->base.magic!=DRAFT_MAGIC || hdr->base.len>SRC_MAX) return false;
    if(hdr->slot>=SRC_SLOT_COUNT) return false;
    if(esp_partition_read(p,base+sizeof(*hdr),out,hdr->base.len)!=ESP_OK) return false;
    out[hdr->base.len]=0;
    if(esp_crc32_le(0,(const uint8_t*)out,hdr->base.len)!=hdr->base.crc) {
        ESP_LOGW("src","draft face %u failed its CRC",face);
        return false;
    }
    return true;
}

// Which face holds the newest draft, reading the headers only.
static int draft_newest(const esp_partition_t *p, uint32_t *out_seq) {
    int best=-1; uint32_t best_seq=0;
    for(unsigned f=0;f<FACES;f++) {
        size_t base=draft_face_at(p,f);
        if(base==SIZE_MAX) continue;
        draft_hdr_t hdr;
        if(esp_partition_read(p,base,&hdr,sizeof(hdr))!=ESP_OK) continue;
        if(hdr.base.magic!=DRAFT_MAGIC || hdr.base.len>SRC_MAX) continue;
        if(best<0 || (int32_t)(hdr.base.seq-best_seq)>0) {
            best=(int)f; best_seq=hdr.base.seq;
        }
    }
    if(out_seq) *out_seq=best_seq;
    return best;
}

size_t srcstore_draft_load(unsigned slot, char *out) {
    out[0]=0;
    const esp_partition_t *p=storage();
    if(!p) return 0;
    int first=draft_newest(p,NULL);
    if(first<0) return 0;
    for(unsigned attempt=0;attempt<FACES;attempt++) {
        unsigned face=(unsigned)((first+attempt)%FACES);
        draft_hdr_t hdr;
        if(!draft_read(p,face,out,&hdr)) continue;
        // A draft belongs to one slot. Another slot asking gets nothing, and
        // the draft stays parked for the slot that owns it.
        if(hdr.slot!=slot) { out[0]=0; return 0; }
        ESP_LOGI("src","draft: %u bytes for slot %u from face %u seq %u",
                 (unsigned)hdr.base.len,slot,face,(unsigned)hdr.base.seq);
        return hdr.base.len;
    }
    out[0]=0;
    return 0;
}

bool srcstore_draft_save(unsigned slot, const char *text, size_t len) {
    if(len>SRC_MAX || slot>=SRC_SLOT_COUNT) return false;
    const esp_partition_t *p=storage();
    if(!p) return false;
    uint32_t seq=0;
    int current=draft_newest(p,&seq);
    unsigned face=(current<0)?0:(unsigned)((current+1)%FACES);
    size_t base=draft_face_at(p,face);
    if(base==SIZE_MAX) return false;

    // Written exactly like a slot's record: body first in aligned runs, header
    // last, so a present magic means the bytes under it are present too.
    draft_hdr_t hdr={.base={.magic=DRAFT_MAGIC,.len=(uint32_t)len,
                            .crc=esp_crc32_le(0,(const uint8_t*)text,len),
                            .seq=seq+1},
                     .slot=slot};
    size_t body=len & ~(size_t)3, tail=len-body;
    esp_err_t err=esp_partition_erase_range(p,base,BLOCK);
    if(err==ESP_OK && body)
        err=esp_partition_write(p,base+sizeof(hdr),text,body);
    if(err==ESP_OK && tail) {
        uint8_t word[4]={0xff,0xff,0xff,0xff};
        memcpy(word,text+body,tail);
        err=esp_partition_write(p,base+sizeof(hdr)+body,word,sizeof(word));
    }
    if(err==ESP_OK) err=esp_partition_write(p,base,&hdr,sizeof(hdr));
    if(err!=ESP_OK) {
        ESP_LOGW("src","draft save failed: %s",esp_err_to_name(err));
        return false;
    }
    ESP_LOGI("src","draft: parked %u bytes of slot %u in face %u seq %u",
             (unsigned)len,slot,face,(unsigned)hdr.base.seq);
    return true;
}

bool srcstore_draft_clear(void) {
    const esp_partition_t *p=storage();
    if(!p) return false;
    // Erasing both faces is what makes "no draft" the answer; erasing only the
    // newest would hand back the one before it.
    for(unsigned f=0;f<FACES;f++) {
        size_t base=draft_face_at(p,f);
        if(base==SIZE_MAX) return false;
        if(esp_partition_erase_range(p,base,BLOCK)!=ESP_OK) return false;
    }
    return true;
}

static bool draft_owner_of(unsigned *slot) {
    const esp_partition_t *p=storage();
    if(!p) return false;
    int face=draft_newest(p,NULL);
    if(face<0) return false;
    size_t base=draft_face_at(p,(unsigned)face);
    if(base==SIZE_MAX) return false;
    draft_hdr_t hdr;
    if(esp_partition_read(p,base,&hdr,sizeof(hdr))!=ESP_OK) return false;
    if(hdr.base.magic!=DRAFT_MAGIC || hdr.slot>=SRC_SLOT_COUNT) return false;
    if(slot) *slot=hdr.slot;
    return true;
}

bool srcstore_draft_owner(unsigned *slot) { return draft_owner_of(slot); }
