#include "glass_rain.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

#define RAIN_W 240
#define RAIN_H 135
#define RAIN_N 6
typedef struct {
    float x,y,start,speed,age,life,phase;
    int radius;
} RainDrop;
static RainDrop drops[RAIN_N];
static uint16_t source_row[RAIN_W];
static uint32_t rng;
static float next_shower,shower_left,next_drop;
static unsigned remaining;

static uint32_t rain_random(void) {
    rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;
}
static float rain_range(float lo,float hi) {return lo+(hi-lo)*(rain_random()&65535)/65535.0f;}
static int rain_clamp(int v,int lo,int hi) {return v<lo?lo:v>hi?hi:v;}
static uint16_t rain_mix(uint16_t a,uint16_t b,unsigned alpha) {
    unsigned r=(((a>>11)&31)*(256-alpha)+((b>>11)&31)*alpha)>>8;
    unsigned g=(((a>>5)&63)*(256-alpha)+((b>>5)&63)*alpha)>>8;
    unsigned blue=((a&31)*(256-alpha)+(b&31)*alpha)>>8;
    return (uint16_t)((r<<11)|(g<<5)|blue);
}
void glass_rain_prepare(float dt,uint32_t seed) {
    if(!rng) {rng=seed?seed:0x62726f6f;next_shower=rain_range(8,16);}
    if(!isfinite(dt)||dt<0)dt=0;
    dt=fminf(dt,.1f); // App return resumes animation rather than simulating a backlog.
    for(int i=0;i<RAIN_N;i++)if(drops[i].life>0) {
        RainDrop *d=&drops[i];d->age+=dt;d->life-=dt;
        if(d->age>.65f) {
            // Surface tension pauses a newly formed bead, then it slips down.
            d->speed=fminf(d->speed+dt*4,27);
            d->y+=d->speed*dt*(.75f+.25f*sinf(d->age*2+d->phase));
        }
        if(d->life<=0)d->life=0;
    }
    if(shower_left>0) {
        shower_left-=dt;next_drop-=dt;
        if(next_drop<=0&&remaining) {
            for(int i=0;i<RAIN_N;i++)if(drops[i].life<=0) {
                RainDrop *d=&drops[i];
                *d=(RainDrop){.x=rain_range(12,228),.y=rain_range(6,44),
                    .speed=rain_range(7,12),.life=rain_range(6,8),
                    .phase=rain_range(0,6.2831853f),.radius=2+(rain_random()&1)};
                d->start=d->y;remaining--;break;
            }
            next_drop=rain_range(.35f,.8f);
        }
    } else {
        next_shower-=dt;
        if(next_shower<=0) {
            shower_left=4;remaining=2+rain_random()%4;next_drop=0;
            next_shower=rain_range(18,36);
        }
    }
}
void glass_rain_draw(uint16_t *pixels,int y,int height) {
    if(!pixels||y<0||height<0||y>RAIN_H||height>RAIN_H-y)return;
    // No extra background samples or rays: refraction only reads the already
    // shaded row. A row copy makes the result independent of strip boundaries
    // and avoids feeding a displaced pixel back into the next displacement.
    for(int j=0;j<height;j++) {
        int py=y+j;bool copied=false;
        uint16_t *row=pixels+j*RAIN_W;
        for(int i=0;i<RAIN_N;i++) {
            const RainDrop *d=&drops[i];if(d->life<=0)continue;
            int cy=(int)d->y,ry=d->radius+3;
            int tail=(int)fmaxf(d->start,d->y-35);
            if(py<tail-ry||py>cy+ry)continue;
            if(!copied) {memcpy(source_row,row,sizeof source_row);copied=true;}
            int cx=(int)(d->x+1.2f*sinf(py*.045f+d->phase));
            unsigned fade=(unsigned)(256*fminf(1,fminf(d->age*3,d->life)));
            int dy=py-cy;
            for(int dx=-d->radius-1;dx<=d->radius+1;dx++) {
                int x=cx+dx;if(x<0||x>=RAIN_W)continue;
                int q=dx*dx*256/(d->radius*d->radius)+dy*dy*256/(ry*ry);
                if(q<256) {
                    int sample=rain_clamp(x-dx/2,0,RAIN_W-1);
                    uint16_t wet=source_row[sample];
                    // Opposing light/dark rims sell the lens without tracing it.
                    if(q>125&&dx<=0)wet=rain_mix(wet,0x9e7b,70);
                    else wet=rain_mix(wet,0x0842,22);
                    if(dy<-ry/3&&dx==-1)wet=rain_mix(wet,0xdfff,90);
                    row[x]=rain_mix(row[x],wet,fade*3/4);
                } else if(py<cy&&py>=tail&&dx>=-1&&dx<=1) {
                    unsigned strength=fade*(unsigned)(py-tail+1)/(unsigned)(cy-tail+1);
                    uint16_t wet=source_row[rain_clamp(x+1,0,RAIN_W-1)];
                    wet=rain_mix(wet,dx<0?0x7c92:0x0842,dx<0?22:30);
                    row[x]=rain_mix(row[x],wet,strength/2);
                }
            }
        }
    }
}
