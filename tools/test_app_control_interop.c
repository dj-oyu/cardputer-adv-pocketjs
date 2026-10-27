#include "app_control.h"
#include <string.h>
static unsigned starts,taps,stops,emitted,generation;
static unsigned char answer[132];
static unsigned count(void) { return 3; }
static bool catalog(unsigned i,app_control_descriptor_t *d) {
    static const char *ids[]={"local.hello","local.stress","local.vmprobe"};
    if(i>=3)return false;
    *d=(app_control_descriptor_t){ids[i],i==0?"HELLO WORLD":i==1?"STRESS TEST":"VM PROBE",i==2?0:7};return true;
}
static unsigned char start(const char *id,bool resume) {(void)id;if(!resume)generation++;starts++;return AC_OK;}
static unsigned char tap(unsigned char a) {(void)a;taps++;return AC_OK;}
static unsigned char suspend_(void) {return AC_OK;}
static bool stop(void) {stops++;return true;}
static bool emit(const unsigned char *p,size_t n) {if(n!=132)return false;memcpy(answer,p,n);emitted++;return true;}
static void fingerprint(unsigned char *hash,char *version) {memset(hash,0x5a,32);strcpy(version,"interop-v1");}
static unsigned instance(void) { return generation; }
void reset(void) {
    starts=taps=stops=emitted=generation=0;
    app_control_hooks_t h={.catalog_count=count,.catalog=catalog,.start=start,.tap=tap,
        .suspend=suspend_,.stop=stop,.emit=emit,.fingerprint=fingerprint,.instance=instance};
    app_control_init(0x1122334455667788ULL,&h);
}
unsigned transact(const unsigned char *p,unsigned n,unsigned long long now,unsigned char *out) {
    emitted=0;app_control_receive(p,n);app_control_pump(now);memcpy(out,answer,132);return emitted;
}
unsigned started(void) {return starts;}
unsigned tapped(void) {return taps;}
unsigned stopped(void) {return stops;}
