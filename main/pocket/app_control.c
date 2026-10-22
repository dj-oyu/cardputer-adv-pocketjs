#include "app_control.h"
#include "pocket_wire.h"
#include <stdatomic.h>
#include <string.h>

typedef struct { uint32_t session, id; uint8_t op, data[AC_REQUEST_SIZE]; } request_t;
typedef struct {
    request_t request;
    uint32_t run;
    uint8_t result, state;
} receipt_t;
static struct {
    app_control_hooks_t hooks;
    uint64_t boot, deadline, now, rate_start;
    uint32_t session, fence, run, run_counter, next_seq, ttl, offer, instance;
    uint8_t state, used, user_count, rate_count;
    bool closed;
    char app[32];
    receipt_t ledger[AC_LEDGER];
} control;
static request_t inbox[AC_INBOX], overflow;
static atomic_uint head, tail;
static atomic_bool overflow_ready;

static uint32_t rd(const uint8_t *p) { return pocket_wire_read32(p); }
static void wr(uint8_t *p,uint32_t v) { pocket_wire_write32(p,v); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd(p)|((uint64_t)rd(p+4)<<32); }
static void wr64(uint8_t *p,uint64_t v) { wr(p,(uint32_t)v);wr(p+4,(uint32_t)(v>>32)); }
static void text(uint8_t *out, size_t n, const char *in) {
    if(in) { size_t len=strlen(in);if(len>=n)len=n-1;memcpy(out,in,len); }
}
static void reply(const request_t *r, uint8_t result, uint8_t state, uint32_t run,
                  const app_control_descriptor_t *descriptor, unsigned index) {
    uint8_t body[12+AC_RESPONSE_SIZE]={1,AC_REPLY,AC_RESPONSE_SIZE,0};
    wr(body+4,r->op==AC_CATALOG?control.offer:r->session);wr(body+8,r->id);
    uint8_t *p=body+12;
    wr64(p,control.boot);wr(p+8,run);wr(p+12,rd(r->data+12));wr(p+16,control.fence);
    p[20]=result;p[21]=state;p[22]=r->op;p[23]=r->data[56];
    wr(p+24,control.next_seq);
    wr(p+28,control.session && !control.closed && state!=AC_NO_SESSION && control.deadline>control.now ? (uint32_t)(control.deadline-control.now):0);
    p[32]=(uint8_t)index;p[33]=(uint8_t)(index>>8);
    unsigned count=control.hooks.catalog_count();p[34]=(uint8_t)count;p[35]=(uint8_t)(count>>8);
    if(r->op==AC_CATALOG && index==0xffff && result==AC_OK) {
        control.hooks.fingerprint(p+40,(char *)p+72);
    } else if(descriptor) {
        wr(p+36,descriptor->flags);text(p+40,32,descriptor->id);text(p+72,48,descriptor->title);
    } else text(p+40,32,control.app);
    (void)control.hooks.emit(body,sizeof(body));
}
static receipt_t *find(uint32_t id) {
    for(unsigned i=0;i<control.used;i++) if(control.ledger[i].request.id==id)return &control.ledger[i];
    return NULL;
}
static bool same(const request_t *a,const request_t *b) {
    return a->session==b->session && a->id==b->id && a->op==b->op &&
           !memcmp(a->data,b->data,AC_REQUEST_SIZE);
}
static bool clean_string(const uint8_t *p) {
    bool end=false;
    for(unsigned i=0;i<32;i++) {
        if(!p[i])end=true;
        else if(end || p[i]<0x21 || p[i]>0x7e)return false;
    }
    return end;
}
static bool zero(const uint8_t *p,size_t n) {
    for(size_t i=0;i<n;i++)if(p[i])return false;
    return true;
}
void app_control_init(uint64_t boot,const app_control_hooks_t *hooks) {
    memset(&control,0,sizeof(control));control.boot=boot?boot:1;control.hooks=*hooks;
    control.offer=(uint32_t)control.boot;if(!control.offer)control.offer=1;
    atomic_store(&head,0);atomic_store(&tail,0);atomic_store(&overflow_ready,false);
}
void app_control_receive(const uint8_t *body,size_t length) {
    if(length!=12+AC_REQUEST_SIZE || body[0]!=1 || body[2]!=AC_REQUEST_SIZE || body[3])return;
    request_t r={.session=rd(body+4),.id=rd(body+8),.op=body[1]};
    memcpy(r.data,body+12,AC_REQUEST_SIZE);
    unsigned h=atomic_load(&head);
    if(h-atomic_load(&tail)>=AC_INBOX) {
        /* Reserve one refusal independently of the admitted queue. A hostile
         * flood beyond this mailbox has no ACK: STATUS returns unknown, never
         * a false consumed receipt. Neither ledger nor admitted input is lost. */
        if(!atomic_load(&overflow_ready)) { overflow=r;atomic_store(&overflow_ready,true); }
        return;
    }
    inbox[h%AC_INBOX]=r;atomic_store(&head,h+1);
}
static bool descriptor_for(const char *id,app_control_descriptor_t *out) {
    for(unsigned i=0;i<control.hooks.catalog_count();i++)
        if(control.hooks.catalog(i,out) && !strcmp(out->id,id))return true;
    return false;
}
static void handle(const request_t *r) {
    uint8_t result=AC_OK;
    uint32_t seq=rd(r->data+12),run=rd(r->data+8),fence=rd(r->data+16);
    uint8_t arg=r->data[56];
    if(!clean_string(r->data+24) || !zero(r->data+57,3) || r->op<AC_CATALOG || r->op>AC_CLOSE) {
        reply(r,AC_INVALID,control.state,control.run,NULL,0);return;
    }
    if(r->op>=AC_START && (rd(r->data+20) ||
       (r->op!=AC_START && !zero(r->data+24,32)) ||
       (r->op!=AC_START && r->op!=AC_TAP && arg))) {
        reply(r,AC_INVALID,control.state,control.run,NULL,0);return;
    }
    if(r->op==AC_CATALOG) {
        app_control_descriptor_t d;
        bool valid=!r->session&&!r->id&&zero(r->data,12)&&zero(r->data+16,44);
        bool got=valid && (seq==0xffff || control.hooks.catalog(seq,&d));
        reply(r,got?AC_OK:AC_INVALID,control.state,control.run,got?&d:NULL,seq);return;
    }
    if(rd64(r->data)!=control.boot) { reply(r,AC_STALE_BOOT,control.state,control.run,NULL,0);return; }
    if(r->op==AC_OPEN) {
        uint32_t ttl=rd(r->data+20);
        if(!r->session || !fence || r->id || run || seq || !ttl || ttl>90000 || arg || !zero(r->data+24,32))result=AC_INVALID;
        else if(control.session && !control.closed) {
            if(control.session!=r->session || control.fence!=fence || control.ttl!=ttl) result=AC_BUSY;
        } else if(!control.offer || r->session!=control.offer)result=AC_STALE_SESSION;
        else {
            control.session=r->session;control.fence=fence;control.ttl=ttl;
            control.deadline=control.now+ttl;control.next_seq=1;control.state=AC_READY;
            control.used=control.user_count=control.rate_count=0;control.rate_start=control.now;
            control.run=0;memset(control.app,0,sizeof(control.app));
            control.closed=false;
        }
        reply(r,result,control.state,control.run,NULL,0);return;
    }
    if(!control.session || r->session!=control.session)result=AC_STALE_SESSION;
    else if(fence!=control.fence)result=AC_STALE_LEASE;
    if(result) { reply(r,result,control.state,control.run,NULL,0);return; }
    if(r->op==AC_STATUS) {
        if(r->id) {
            receipt_t *entry=find(r->id);
            if(!entry || rd(entry->request.data+12)!=seq)reply(r,AC_OUTCOME_UNKNOWN,control.state,control.run,NULL,0);
            else reply(&entry->request,entry->result,entry->state,entry->run,NULL,0);
        } else if(run!=control.run)reply(r,AC_STALE_RUN,control.state,control.run,NULL,0);
        else reply(r,AC_OK,control.state,control.run,NULL,0);
        return;
    }
    receipt_t *entry=find(r->id);
    if(entry) {
        if(same(r,&entry->request))reply(r,entry->result,entry->state,entry->run,NULL,0);
        else reply(r,AC_CONFLICT,control.state,control.run,NULL,0);
        return;
    }
    if(control.closed) { reply(r,AC_STALE_SESSION,control.state,control.run,NULL,0);return; }
    if(!r->id || seq!=control.next_seq || !seq)result=AC_OUT_OF_ORDER;
    else if(control.used>=AC_LEDGER)result=AC_LIMIT;
    else if((r->op!=AC_START || arg!=0) && run!=control.run)result=AC_STALE_RUN;
    else if(r->op==AC_START && arg==0 && run)result=AC_STALE_RUN;
    if(result) { reply(r,result,control.state,control.run,NULL,0);return; }
    entry=&control.ledger[control.used++];entry->request=*r;
    control.next_seq++;
    bool cleanup=r->op==AC_STOP || r->op==AC_CLOSE;
    if(!cleanup && control.now>=control.deadline)result=AC_EXPIRED;
    else if(!cleanup && control.user_count>=60)result=AC_LIMIT;
    if(!cleanup && !result) {
        if(control.now-control.rate_start>=1000) { control.rate_start=control.now;control.rate_count=0; }
        if(control.rate_count>=10)result=AC_LIMIT;
        else { control.rate_count++;control.user_count++; }
    }
    if(!result)switch(r->op) {
    case AC_START: {
        app_control_descriptor_t d;
        if(arg>1)result=AC_INVALID;
        else if(!descriptor_for((const char *)r->data+24,&d))result=AC_UNKNOWN_APP;
        else if(!(d.flags&1))result=AC_UNSUPPORTED;
        else if(arg==1 && (control.state!=AC_SUSPENDED || strcmp(control.app,d.id)))result=AC_STALE_RUN;
        else if(control.state==AC_RUNNING)result=AC_BUSY;
        else if(!arg && control.run_counter==UINT32_MAX)result=AC_LIMIT;
        else {
            result=control.hooks.start(d.id,arg==1);
            if(result==AC_OK) {
                if(!arg) { if(++control.run_counter==0)++control.run_counter;control.run=control.run_counter; }
                memset(control.app,0,32);text((uint8_t *)control.app,32,d.id);control.state=AC_RUNNING;
                control.instance=control.hooks.instance();
            }
        }
        break;
    }
    case AC_TAP:
        if(control.state!=AC_RUNNING)result=AC_BLOCKED;
        else if(arg<1 || arg>6)result=AC_UNSUPPORTED;
        else {
            entry->result=AC_ACCEPTED;entry->state=control.state;entry->run=control.run;
            reply(r,AC_ACCEPTED,control.state,control.run,NULL,0);
            result=control.hooks.tap(arg);
            if(result==AC_OK)result=AC_CONSUMED;
        }
        break;
    case AC_SUSPEND:
        if(control.state!=AC_RUNNING)result=AC_BLOCKED;
        else { result=control.hooks.suspend();if(!result)control.state=AC_SUSPENDED; }
        break;
    case AC_STOP:
        if(!control.hooks.stop()) { result=AC_TEARDOWN_FAILED;control.state=AC_NEEDS_REVIEW; }
        else control.state=AC_STOPPED;
        break;
    case AC_CLOSE:
        if(control.state!=AC_STOPPED)result=AC_BUSY;
        else control.state=AC_NO_SESSION;
        break;
    default:result=AC_INVALID;break;
    }
    entry->result=result;entry->state=control.state;entry->run=control.run;
    reply(r,result,control.state,control.run,NULL,0);
    if(r->op==AC_CLOSE && result==AC_OK) { control.closed=true;control.offer++; }
}
static void expire(void) {
    if(control.session && !control.closed && control.now>=control.deadline) {
        if(control.state==AC_STOPPED || control.hooks.stop()) { control.state=AC_STOPPED;control.closed=true;control.offer++; }
        else control.state=AC_NEEDS_REVIEW;
    }
}
void app_control_pump(uint64_t now_ms) {
    control.now=now_ms;expire();
    /* A finite queue is not a finite owner turn if the USB producer refills
     * it while responses block in the shared writer. Only drain the snapshot
     * admitted before this turn, then return to physical keys and UI frames. */
    unsigned limit=atomic_load(&head);
    for(unsigned work=0;work<AC_INBOX && atomic_load(&tail)!=limit;work++) {
        if(control.hooks.clock_ms)control.now=control.hooks.clock_ms();
        expire();
        unsigned t=atomic_load(&tail);request_t r=inbox[t%AC_INBOX];atomic_store(&tail,t+1);
        handle(&r);
        if(control.hooks.clock_ms)control.now=control.hooks.clock_ms();
        expire();
    }
    if(atomic_load(&overflow_ready)) {
        request_t r=overflow;atomic_store(&overflow_ready,false);
        receipt_t *old=find(r.id);
        if(old && r.session==control.session) {
            if(same(&r,&old->request))reply(&r,old->result,old->state,old->run,NULL,0);
            else reply(&r,AC_CONFLICT,control.state,control.run,NULL,0);
        } else {
            /* A reserved refusal is a stable receipt too. Keeping its ID and
             * high water prevents a retry from silently becoming an action. */
            if(r.op>=AC_START && r.op!=AC_STATUS && r.op<=AC_CLOSE && r.id &&
               rd64(r.data)==control.boot && r.session==control.session &&
               rd(r.data+16)==control.fence && rd(r.data+12)==control.next_seq &&
               control.used<AC_LEDGER && !control.closed) {
                receipt_t *e=&control.ledger[control.used++];e->request=r;
                e->result=AC_QUEUE_FULL;e->state=control.state;e->run=control.run;
                control.next_seq++;
            }
            reply(&r,AC_QUEUE_FULL,control.state,control.run,NULL,0);
        }
    }
}
void app_control_observe(bool running,const char *dormant_id,const char *current_id,uint32_t instance) {
    if(!control.session || control.closed || !control.run || (control.state!=AC_RUNNING && control.state!=AC_SUSPENDED))return;
    if(instance!=control.instance)control.state=AC_STOPPED;
    else if(running && current_id && !strcmp(current_id,control.app))control.state=AC_RUNNING;
    else if(dormant_id && !strcmp(dormant_id,control.app))control.state=AC_SUSPENDED;
    else control.state=AC_STOPPED;
}
size_t app_control_static_bytes(void) { return sizeof(control)+sizeof(inbox)+sizeof(overflow)+sizeof(head)+sizeof(tail)+sizeof(overflow_ready); }
