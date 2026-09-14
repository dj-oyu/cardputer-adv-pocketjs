#include "ksn_frost.h"
#include "ksn_stress_scene.h"
#include <stdio.h>
static uint16_t strip[240*8];
int main(void){
    ksn_frost frost;const unsigned times[]={0,1234,8765};
    for(unsigned i=0;i<3;i++){
        unsigned t=times[i],px,py,a;stress_panel(t,&px,&py,&a);
        ksn_frost_init(&frost);
        for(unsigned y=0;y<135;y+=8){
            unsigned rows=y==128?7:8;
            for(unsigned row=0;row<rows;row++)for(unsigned x=0;x<240;x++)strip[row*240+x]=stress_source(x,y+row,t);
            if(ksn_frost_feed(&frost,y,rows,strip)!=KSN_OK)return 1;
        }
        if(ksn_frost_blur(&frost,1+i%2)!=KSN_OK)return 1;
        for(unsigned y=0;y<135;y++){
            if(ksn_frost_span(&frost,y,0,240,0x1c304300|a,strip)!=KSN_OK)return 1;
            for(unsigned x=0;x<240;x++){putchar(strip[x]&255);putchar(strip[x]>>8);}
        }
    }
    return 0;
}
