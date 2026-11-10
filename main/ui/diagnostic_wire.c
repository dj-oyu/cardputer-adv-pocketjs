#include "diagnostic_wire.h"
#include <string.h>
static const char hex[]="0123456789abcdef";
static int digit(uint8_t c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
static bool decode(const uint8_t *p,uint8_t *out,unsigned n){
    for(unsigned i=0;i<n;i++){
        int a=digit(p[2*i]),b=digit(p[2*i+1]);if(a<0||b<0)return false;
        out[i]=(uint8_t)((unsigned)a*16u+(unsigned)b);
    }
    return true;
}
static uint32_t read_be32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void reply(diagnostic_wire *w,const uint8_t id[DC_ID_BYTES],diagnostic_control_snapshot s){
    static const char *const names[]={"PENDING","RUNNING","CANCEL_REQUESTED","COMPLETED","CANCELLED","REFUSED","QUARANTINED"};
    uint8_t out[DIAGNOSTIC_RESPONSE_MAX];size_t n=0;
    out[n++]=0x1d;memcpy(out+n,"ND1 ",4);n+=4;
    for(unsigned i=0;i<DC_ID_BYTES;i++){out[n++]=(uint8_t)hex[id[i]>>4];out[n++]=(uint8_t)hex[id[i]&15];}
    out[n++]=' ';size_t len=strlen(names[s.state]);memcpy(out+n,names[s.state],len);n+=len;
    out[n++]=' ';out[n++]=(uint8_t)hex[s.flags>>4];out[n++]=(uint8_t)hex[s.flags&15];out[n++]=' ';
    uint32_t halves[2]={(uint32_t)(w->control->boot>>32),(uint32_t)w->control->boot};
    for(unsigned h=0;h<2;h++)for(unsigned i=0;i<8;i++)out[n++]=(uint8_t)hex[(halves[h]>>(28-4*i))&15];
    out[n++]=' ';out[n++]=w->control->enabled?'1':'0';out[n++]='\n';
    /* Worst case is CANCEL_REQUESTED: exactly77 bytes, below the fixed80 cap.
     * A refused TX does not roll back a request or implicitly retry work. */
    if(w->emit)(void)w->emit(w->context,out,n);
}
static void complete(diagnostic_wire *w){
    const uint8_t *p=w->bytes;unsigned n=w->length;unsigned op,width;
    if(n<4||p[0]!=0x1d||p[1]!='D'||p[2]!='1'||p[3]!=' ')return;
    if(n>=12&&!memcmp(p+4,"REQUEST ",8)){op=0;width=7;}
    else if(n>=11&&!memcmp(p+4,"STATUS ",7)){op=1;width=6;}
    else if(n>=11&&!memcmp(p+4,"CANCEL ",7)){op=2;width=6;}
    else return;
    unsigned at=4+width+1;
    if(n!=at+32+1+16+1||p[at+32]!=' '||p[n-1]!='\n')return;
    uint8_t id[DC_ID_BYTES],epoch[8];
    if(!decode(p+at,id,DC_ID_BYTES)||!decode(p+at+33,epoch,8))return;
    uint64_t boot=(uint64_t)read_be32(epoch)<<32|read_be32(epoch+4);
    diagnostic_control_snapshot s;
    if(op==0)s=diagnostic_control_request(w->control,boot,id);
    else if(op==1)s=diagnostic_control_status(w->control,boot,id);
    else s=diagnostic_control_cancel(w->control,boot,id);
    reply(w,id,s);
}
void diagnostic_wire_init(diagnostic_wire *w,diagnostic_control *c,diagnostic_wire_emit emit,void *context){
    w->control=c;w->emit=emit;w->context=context;w->length=0;w->dropping=false;
}
void diagnostic_wire_begin(diagnostic_wire *w){w->bytes[0]=0x1d;w->bytes[1]='D';w->length=2;w->dropping=false;}
void diagnostic_wire_abort(diagnostic_wire *w){w->length=0;w->dropping=false;}
bool diagnostic_wire_feed(diagnostic_wire *w,uint8_t byte){
    if(!w||!w->control)return byte=='\n';
    if(!w->dropping){
        if(w->length<DIAGNOSTIC_REQUEST_MAX)w->bytes[w->length++]=byte;
        else w->dropping=true;
    }
    if(byte!='\n')return false;
    if(!w->dropping)complete(w);
    diagnostic_wire_abort(w);return true;
}
