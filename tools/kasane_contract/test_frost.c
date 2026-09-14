#include "ksn_frost.h"
#include <stdio.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"frost line %d\n",__LINE__);return 1;}}while(0)
static uint16_t strip[240*8];
static uint16_t rgb(unsigned r,unsigned g,unsigned b){return (uint16_t)((r>>3)<<11|(g>>2)<<5|(b>>3));}
int main(void){
    ksn_frost frost;ksn_frost_init(&frost);
    CHECK(sizeof(frost)==2048);
    CHECK(ksn_frost_span(&frost,0,0,1,0,strip)==KSN_STALE);
    CHECK(ksn_frost_blur(&frost,1)==KSN_STALE);
    CHECK(ksn_frost_feed(&frost,8,8,strip)==KSN_STALE);
    CHECK(ksn_frost_feed(&frost,0,7,strip)==KSN_INVALID);
    for(unsigned radius=1;radius<=2;radius++){
        ksn_frost_init(&frost);
        for(unsigned y=0;y<135;y+=8){
            unsigned rows=y==128?7:8;
            for(unsigned row=0;row<rows;row++)for(unsigned x=0;x<240;x++){
                unsigned py=y+row;
                strip[row*240+x]=py<16?rgb(13,23,39):((x%120)/12+py/12)%2?rgb(30,160,220):rgb(235,128,48);
            }
            CHECK(ksn_frost_feed(&frost,y,rows,strip)==KSN_OK);
        }
        CHECK(ksn_frost_blur(&frost,0)==KSN_INVALID);
        CHECK(ksn_frost_blur(&frost,radius)==KSN_OK);
        CHECK(ksn_frost_blur(&frost,radius)==KSN_STALE);
        CHECK(ksn_frost_span(&frost,0,239,2,0,strip)==KSN_INVALID);
        for(unsigned y=0;y<135;y++){
            CHECK(ksn_frost_span(&frost,y,0,240,0x1c304360,strip)==KSN_OK);
            for(unsigned x=0;x<240;x++){putchar(strip[x]&255);putchar(strip[x]>>8);}
        }
    }
    return 0;
}
