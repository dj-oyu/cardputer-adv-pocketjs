#include "diagnostic_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static diagnostic_control c;
static diagnostic_session session=DIAGNOSTIC_SESSION_INIT;
static diagnostic_wire wire;
static char answer[81];static unsigned replies;static bool refuse_tx;
static bool emit(void *ctx,const uint8_t *p,size_t n){
 (void)ctx;assert(n<=80&&p[0]==0x1d&&p[n-1]=='\n');memcpy(answer,p,n);answer[n]=0;replies++;return !refuse_tx;
}
static void frame(const char *s){
 assert(s[0]==0x1d&&s[1]=='D');diagnostic_wire_begin(&wire);
 for(size_t i=2;i<strlen(s);i++)assert(diagnostic_wire_feed(&wire,(uint8_t)s[i])==(i==strlen(s)-1));
}
static void reset(bool enabled){
 session=(diagnostic_session)DIAGNOSTIC_SESSION_INIT;
 assert(diagnostic_control_init(&c,UINT64_C(0x123456789abcdef0),enabled,enabled?&session:NULL));
 diagnostic_wire_init(&wire,&c,emit,NULL);replies=0;refuse_tx=false;
}
int main(void){
 reset(true);
 frame("\x1d" "D1 STATUS 00000000000000000000000000000000 0000000000000000\n");
 assert(!strcmp(answer,"\x1d" "ND1 00000000000000000000000000000000 REFUSED 00 123456789abcdef0 1\n"));
 frame("\x1d" "D1 REQUEST 11111111111111111111111111111111 123456789abcdef0\n");
 assert(!strcmp(answer,"\x1d" "ND1 11111111111111111111111111111111 PENDING 20 123456789abcdef0 1\n"));
 assert(atomic_load(&c.used)==1);frame("\x1d" "D1 REQUEST 11111111111111111111111111111111 123456789abcdef0\n");assert(atomic_load(&c.used)==1);
 frame("\x1d" "D1 CANCEL 11111111111111111111111111111111 123456789abcdef0\n");
 assert(strstr(answer,"CANCEL_REQUESTED 20"));assert(strlen(answer)==77);
 unsigned before=atomic_load(&c.used);
 frame("\x1d" "D1 REQUEST 22222222222222222222222222222222 123456789abcdef1\n");
 assert(strstr(answer,"REFUSED 00")&&atomic_load(&c.used)==before);
 frame("\x1d" "D1 CANCEL 22222222222222222222222222222222 123456789abcdef0\n");assert(strstr(answer,"REFUSED 00"));
 frame("\x1d" "D1 REQUEST 00000000000000000000000000000000 123456789abcdef0\n");assert(strstr(answer,"REFUSED 00"));
 const char *bad[]={"\x1d" "D2 STATUS 00000000000000000000000000000000 0000000000000000\n",
 "\x1d" "D1 STATUS 1111111111111111111111111111111A 123456789abcdef0\n",
 "\x1d" "D1 STATUS 11111111111111111111111111111111 123456789ABCDEF0\n",
 "\x1d" "D1 STATUS  11111111111111111111111111111111 123456789abcdef0\n",
 "\x1d" "D1 STATUS 11111111111111111111111111111111 123456789abcdef0\r\n",
 "\x1d" "D1 REQUEST 11111111111111111111111111111111 123456789abcdef0 extra\n"};
 for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){before=replies;frame(bad[i]);assert(replies==before);}
 diagnostic_wire_begin(&wire);before=replies;
 for(unsigned i=0;i<1000;i++)assert(!diagnostic_wire_feed(&wire,'q'));
 assert(diagnostic_wire_feed(&wire,'\n')&&replies==before);
 diagnostic_wire_begin(&wire);assert(!diagnostic_wire_feed(&wire,'1'));diagnostic_wire_abort(&wire);
 frame("\x1d" "D1 STATUS 11111111111111111111111111111111 123456789abcdef0\n");assert(strstr(answer,"CANCEL_REQUESTED 20"));
 reset(false);
 frame("\x1d" "D1 REQUEST 11111111111111111111111111111111 123456789abcdef0\n");assert(strstr(answer,"REFUSED 20 123456789abcdef0 0"));assert(!atomic_load(&c.used));
 frame("\x1d" "D1 STATUS 00000000000000000000000000000000 0000000000000000\n");assert(strstr(answer,"REFUSED 00 123456789abcdef0 0"));
 reset(true);refuse_tx=true;
 frame("\x1d" "D1 REQUEST 11111111111111111111111111111111 123456789abcdef0\n");assert(atomic_load(&c.used)==1);refuse_tx=false;
 frame("\x1d" "D1 STATUS 11111111111111111111111111111111 123456789abcdef0\n");assert(strstr(answer,"PENDING 20")&&atomic_load(&c.used)==1);
 puts("diagnostic wire PASS canonical bounded frames, disabled, nonce, unknown, lost-TX receipt");
}
