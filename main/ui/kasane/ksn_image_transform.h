#ifndef KSN_IMAGE_TRANSFORM_H
#define KSN_IMAGE_TRANSFORM_H
#include "ksn_types.h"
/* Q14 quarter-wave, generated as round(sin(i*pi/512)*16384). Flash only.
 * Rotation is clockwise in 1/1024 turns; no trigonometry in the hot path. */
static const int16_t ksn_sine_quarter[257]={0,101,201,302,402,503,603,704,804,904,1005,1105,1205,1306,1406,1506,1606,1706,1806,1906,2006,2105,2205,2305,2404,2503,2603,2702,2801,2900,2999,3098,3196,3295,3393,3492,3590,3688,3786,3883,3981,4078,4176,4273,4370,4467,4563,4660,4756,4852,4948,5044,5139,5235,5330,5425,5520,5614,5708,5803,5897,5990,6084,6177,6270,6363,6455,6547,6639,6731,6823,6914,7005,7096,7186,7276,7366,7456,7545,7635,7723,7812,7900,7988,8076,8163,8250,8337,8423,8509,8595,8680,8765,8850,8935,9019,9102,9186,9269,9352,9434,9516,9598,9679,9760,9841,9921,10001,10080,10159,10238,10316,10394,10471,10549,10625,10702,10778,10853,10928,11003,11077,11151,11224,11297,11370,11442,11514,11585,11656,11727,11797,11866,11935,12004,12072,12140,12207,12274,12340,12406,12472,12537,12601,12665,12729,12792,12854,12916,12978,13039,13100,13160,13219,13279,13337,13395,13453,13510,13567,13623,13678,13733,13788,13842,13896,13949,14001,14053,14104,14155,14206,14256,14305,14354,14402,14449,14497,14543,14589,14635,14680,14724,14768,14811,14854,14896,14937,14978,15019,15059,15098,15137,15175,15213,15250,15286,15322,15357,15392,15426,15460,15493,15525,15557,15588,15619,15649,15679,15707,15736,15763,15791,15817,15843,15868,15893,15917,15941,15964,15986,16008,16029,16049,16069,16088,16107,16125,16143,16160,16176,16192,16207,16221,16235,16248,16261,16273,16284,16295,16305,16315,16324,16332,16340,16347,16353,16359,16364,16369,16373,16376,16379,16381,16383,16384,16384};
static inline int ksn_image_sin(unsigned angle){
    angle&=1023;unsigned quadrant=angle>>8,index=angle&255;
    int value=ksn_sine_quarter[(quadrant&1)?256-index:index];
    return quadrant>=2?-value:value;
}
static inline int ksn_image_bound(int64_t value,bool upper){
    int64_t q=value/32768,r=value%32768;
    if(upper&&r>0)q++;
    if(!upper&&r<0)q--;
    return q<-32768?-32768:q>32767?32767:(int)q;
}
static inline ksn_rect ksn_image_footprint(ksn_rect b,unsigned angle){
    if(!angle||b.x0==b.x1||b.y0==b.y1)return b;
    int c=ksn_image_sin(angle+256),s=ksn_image_sin(angle);
    if(c<0)c=-c;
    if(s<0)s=-s;
    int64_t w=(int)b.x1-b.x0,h=(int)b.y1-b.y0;
    int64_t cx=((int)b.x0+b.x1)*16384ll,cy=((int)b.y0+b.y1)*16384ll;
    int64_t ex=w*c+h*s,ey=w*s+h*c;
    return (ksn_rect){ksn_image_bound(cx-ex,false),ksn_image_bound(cy-ey,false),
                      ksn_image_bound(cx+ex,true),ksn_image_bound(cy+ey,true)};
}
/* Exact rational inverse of the quantized clockwise basis. Pixel centers are
 * tested against the unrotated rectangle before source indices are emitted. */
static inline bool ksn_image_sample(const ksn_draw *d,int x,int y,unsigned *sx,unsigned *sy){
    int c=ksn_image_sin(d->data.image.rotation+256),s=ksn_image_sin(d->data.image.rotation);
    int64_t w=(int)d->bounds.x1-d->bounds.x0,h=(int)d->bounds.y1-d->bounds.y0;
    if(!w||!h)return false;
    int64_t qx=2ll*x+1-d->bounds.x0-d->bounds.x1,qy=2ll*y+1-d->bounds.y0-d->bounds.y1;
    int64_t u=w*16384+qx*c+qy*s,v=h*16384-qx*s+qy*c;
    if(u<0||v<0||u>=w*32768||v>=h*32768)return false;
    *sx=d->data.image.source_x+(unsigned)(u*d->data.image.source_width/(w*32768));
    *sy=d->data.image.source_y+(unsigned)(v*d->data.image.source_height/(h*32768));
    return true;
}
#endif
