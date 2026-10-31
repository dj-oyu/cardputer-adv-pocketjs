/* Actual QuickJS boundary for the bounded indexed colour patch. */
#define main procedural_baseline_fixture_main
#include "test_pocket_proc_qjs.c"
#undef main
#include <math.h>
#include <malloc.h>
static size_t ga_calls,ga_live;
static void *ga_malloc(void *o,size_t n){(void)o;++ga_calls;void *p=malloc(n);if(p)++ga_live;return p;}
static void *ga_calloc(void *o,size_t n,size_t z){(void)o;++ga_calls;void *p=calloc(n,z);if(p)++ga_live;return p;}
static void ga_free(void *o,void *p){(void)o;if(p)--ga_live;free(p);}
static void *ga_realloc(void *o,void *p,size_t n){(void)o;++ga_calls;bool had=p!=NULL;void *q=realloc(p,n);if(!had&&q)++ga_live;if(had&&!n)--ga_live;return q;}
static size_t ga_usable(const void *p){
#ifdef _WIN32
 return _msize((void *)p);
#else
 return malloc_usable_size((void *)p);
#endif
}
static const JSMallocFunctions GA_MF={ga_calloc,ga_malloc,ga_free,ga_realloc,ga_usable};
static void mark_unchanged(JSContext *ctx,const pocket_proc_mark *a){pocket_proc_mark b;JSValue v=pocket_proc_native_begin(ctx,"gouraud-test",&b);REQUIRE(!JS_IsException(v));JS_FreeValue(ctx,v);REQUIRE(a->count==b.count&&a->raster==b.raster&&a->ext==b.ext&&a->surface==b.surface);}
static void band_equal(void){
 REQUIRE(pocket_proc_backdrop(NULL,0,135,expected)==KSN_OK);
 for(unsigned height=1;height<=17;height+=4){
  memset(actual,0,sizeof actual);for(unsigned y=0;y<135;y+=height){unsigned n=height;if(n>135-y)n=135-y;REQUIRE(pocket_proc_backdrop(NULL,y,n,actual+y*240)==KSN_OK);}REQUIRE(!memcmp(expected,actual,sizeof expected));
 }
}
static void odd_free(JSRuntime *rt,void *opaque,void *data){(void)rt;(void)data;free(opaque);}
static unsigned ga_encode(ksn_proc_segment *data,const ksn_proc_gouraud_patch *p){
 unsigned words=(unsigned)KSN_PROC_GOURAUD_WORDS(p->header.vertices,p->header.triangles,p->header.holes);uint8_t *out=(uint8_t *)data;memset(out,0,words*sizeof *data);memcpy(out,&p->header,8);out+=8;
 size_t bytes=p->header.vertices*sizeof p->vertices[0];memcpy(out,p->vertices,bytes);out+=bytes;bytes=p->header.triangles*6u;memcpy(out,p->indices,bytes);out+=bytes;memcpy(out,p->clips,p->header.holes*sizeof p->clips[0]);return words;
}
static void packet_contracts(void){
 ksn_proc_gouraud_patch p={0},q;int l,t,r,b;unsigned cost,used;
 p.header.vertices=4;p.header.triangles=2;
 const float xy[8]={10,10,30,10,30,30,10,30};const uint16_t ix[6]={0,1,3,1,2,3};
 for(unsigned i=0;i<4;i++)p.vertices[i]=(ksn_proc_gouraud_vertex){xy[i*2],xy[i*2+1],i<2?-1:1,63488,31};memcpy(p.indices,ix,sizeof ix);
 REQUIRE(ksn_proc_gouraud_bounds(&p,&l,&t,&r,&b,&cost)&&l==10&&t==10&&r==30&&b==30&&cost==882);
 ksn_proc_segment raw[KSN_PROC_GOURAUD_WORDS(32,64,2)];const unsigned words=ga_encode(raw,&p);
 for(unsigned n=0;n<words;n++)REQUIRE(!ksn_proc_gouraud_read(raw,n,&q,&used));
 uint8_t skew[sizeof raw+10];for(unsigned offset=0;offset<10;offset++){
  memcpy(skew+offset,raw,words*sizeof raw[0]);REQUIRE(ksn_proc_gouraud_read((const ksn_proc_segment *)(skew+offset),words,&q,&used)&&used==words&&!memcmp(&q,&p,sizeof p));
 }
 /* Explicit tail padding and header fields cannot smuggle hidden signature bytes. */
 const size_t bytes=KSN_PROC_GOURAUD_BYTES(4,2,0);REQUIRE(bytes<words*sizeof raw[0]);((uint8_t *)raw)[bytes]=1;REQUIRE(!ksn_proc_gouraud_read(raw,words,&q,&used));ga_encode(raw,&p);
 ((uint8_t *)raw)[3]=1;REQUIRE(!ksn_proc_gouraud_read(raw,words,&q,&used));ga_encode(raw,&p);
 q=p;q.vertices[3].mask=NAN;REQUIRE(!ksn_proc_gouraud_bounds(&q,&l,&t,&r,&b,&cost));q=p;q.vertices[3].x=4097;REQUIRE(!ksn_proc_gouraud_bounds(&q,&l,&t,&r,&b,&cost));
 q=p;q.indices[5]=4;REQUIRE(!ksn_proc_gouraud_bounds(&q,&l,&t,&r,&b,&cost));q=p;q.indices[1]=3;q.indices[2]=1;REQUIRE(!ksn_proc_gouraud_bounds(&q,&l,&t,&r,&b,&cost));
 q=p;q.vertices[4].color_a=1;REQUIRE(!ksn_proc_gouraud_bounds(&q,&l,&t,&r,&b,&cost));q=p;q.indices[6]=1;REQUIRE(!ksn_proc_gouraud_bounds(&q,&l,&t,&r,&b,&cost));q=p;q.clips[0].xy[0]=1;REQUIRE(!ksn_proc_gouraud_bounds(&q,&l,&t,&r,&b,&cost));
 /* All-offscreen remains valid and has zero raster cost. */
 q=p;for(unsigned i=0;i<4;i++)q.vertices[i].x-=100;REQUIRE(ksn_proc_gouraud_bounds(&q,&l,&t,&r,&b,&cost)&&l>r&&cost==0);
 /* A valid but expensive packet reports full cost; the append enforces 65535. */
 q=(ksn_proc_gouraud_patch){0};q.header.vertices=3;q.header.triangles=64;q.vertices[0]=(ksn_proc_gouraud_vertex){0,0,0,0,0};q.vertices[1]=(ksn_proc_gouraud_vertex){239,0,0,0,0};q.vertices[2]=(ksn_proc_gouraud_vertex){0,134,0,0,0};for(unsigned i=0;i<64;i++){q.indices[3*i]=0;q.indices[3*i+1]=1;q.indices[3*i+2]=2;}REQUIRE(ksn_proc_gouraud_bounds(&q,&l,&t,&r,&b,&cost)&&cost==64u*240u*135u);
 /* Mutated packets must remain bounded under a sanitizer, including a frame walk. */
 uint32_t seed=0x4a31c7u;static ksn_proc_frame f;
 for(unsigned attempt=0;attempt<512;attempt++){
  ga_encode(raw,&p);seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;((uint8_t *)raw)[seed%(words*sizeof raw[0])]^=(uint8_t)(1u<<((seed>>16)&7));
  if(!ksn_proc_gouraud_read(raw,words,&q,&used)||!ksn_proc_gouraud_bounds(&q,&l,&t,&r,&b,&cost)||l>r||t>b||cost>65535)continue;
  memset(&f,0,sizeof f);f.ready=true;f.ext=1;f.count=(uint16_t)(words+1);f.raster_steps=(uint16_t)cost;f.segments[0]=(ksn_proc_segment){(int16_t)(l+KSN_PROC_EXT_GOURAUD_PATCH),(int16_t)t,(int16_t)r,(int16_t)b,0};memcpy(f.segments+1,raw,words*sizeof raw[0]);unsigned i=0,walk_cost;bool pattern;ksn_proc_segment g;REQUIRE(ksn_proc_frame_next(&f,&i,&g,&pattern,&walk_cost)&&i==f.count&&walk_cost==cost&&pattern);memset(actual,0,sizeof actual);REQUIRE(ksn_proc_render_band(&f,actual,0,135));
 }
 puts("GOURAUD_PACKET_PASS truncation alignment padding range cost mutation512");
}

int main(void){
 packet_contracts();
 JSRuntime *rt=JS_NewRuntime2(&GA_MF,NULL);REQUIRE(rt);JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
 if(sizeof(void *)==4){JS_SetMemoryLimit(rt,163840);JS_SetGCThreshold(rt,81920);}
 JSValue global=JS_GetGlobalObject(ctx),pocket=JS_NewObject(ctx),kasane=JS_NewObject(ctx),hone=JS_NewObject(ctx);
 REQUIRE(pocket_proc_install(ctx,kasane)==ESP_OK&&pocket_shape_fill(ctx,hone,NULL)==ESP_OK);
 REQUIRE(JS_SetPropertyStr(ctx,pocket,"kasane",kasane)>=0&&JS_SetPropertyStr(ctx,pocket,"hone",hone)>=0);
 REQUIRE(JS_SetPropertyStr(ctx,global,"pocket",pocket)>=0);JS_FreeValue(ctx,global);
 eval_ok(ctx,"globalThis.P=pocket.kasane.procedural;globalThis.H=pocket.hone;"
  "globalThis.xy=new Float32Array([10,10,30,10,30,30,10,30]);globalThis.ix=new Uint16Array([0,1,3,1,2,3]);"
  "globalThis.ca=new Uint16Array([63488,63488,63488,63488]);globalThis.cb=new Uint16Array([31,31,31,31]);"
  "globalThis.mask=new Float32Array([-1,-1,1,1]);globalThis.hxy=new Float32Array(0);globalThis.hc=new Uint8Array(0);"
  "globalThis.init=()=>{xy.set([10,10,30,10,30,30,10,30]);ix.set([0,1,3,1,2,3]);ca.fill(63488);cb.fill(31);mask.set([-1,-1,1,1]);hxy=new Float32Array(0);hc=new Uint8Array(0);};"
  "globalThis.draw=()=>H.gouraudPatch(xy,ix,ca,cb,mask,hxy,hc);P.beginFrame(2016);draw();P.commit()");
 band_equal();REQUIRE(expected[15*240+15]==63488&&expected[25*240+15]==31);unsigned filled=0;for(unsigned i=0;i<240*135;i++)filled+=expected[i]!=2016;REQUIRE(filled==400);
 /* Submitted buffers are not borrowed across frames. */
 eval_ok(ctx,"xy.fill(NaN);ca.fill(0);mask.fill(NaN)");REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(expected,actual,sizeof expected));pocket_proc_present_result(KSN_OK);
 /* All data fields participate in the band signature and IO repair. */
 eval_ok(ctx,"init();ca.fill(65535);P.beginFrame(2016);draw();P.commit()");uint32_t damage;int16_t x0[17],x1[17];REQUIRE(pocket_proc_band_damage(0,&damage,x0,x1)&&damage);REQUIRE(pocket_proc_backdrop(NULL,0,135,previous)==KSN_OK&&memcmp(expected,previous,sizeof expected));pocket_proc_present_result(KSN_IO);
 REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(expected,actual,sizeof expected));eval_error(ctx,"P.beginFrame(2016)");pocket_proc_present_result(KSN_OK);
 /* Validation and budget reject without changing the existing candidate. */
 eval_ok(ctx,"init();P.beginFrame(2016);draw()");pocket_proc_mark mark;JSValue value=pocket_proc_native_begin(ctx,"gouraud-test",&mark);REQUIRE(!JS_IsException(value));JS_FreeValue(ctx,value);
 const char *bad[]={"xy[7]=NaN;draw()","xy[7]=5000;draw()","mask[3]=Infinity;draw()","ix[5]=4;draw()","ix.set([0,3,1,1,2,3]);draw()","ix.set([0,0,3,1,2,3]);draw()","H.gouraudPatch(xy,ix,ca,cb,new Float32Array(3),hxy,hc)","hxy=new Float32Array([12,12,20,12,16,14,20,20]);hc=new Uint8Array([4]);draw()","hc=new Uint8Array([2]);draw()","H.gouraudPatch(xy,ix,ca,cb,mask,hxy,new Uint16Array(0))","H.gouraudPatch(xy,ix,ca,cb,mask,new Float32Array(0),new Uint8Array([3]))"};
 for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){eval_ok(ctx,"init()");eval_error(ctx,bad[i]);mark_unchanged(ctx,&mark);}
 eval_ok(ctx,"init();if(typeof SharedArrayBuffer==='function'){let caught=false;try{H.gouraudPatch(new Float32Array(new SharedArrayBuffer(32)),ix,ca,cb,mask,hxy,hc)}catch(e){caught=true}if(!caught)throw Error('shared accepted');}");mark_unchanged(ctx,&mark);
 eval_ok(ctx,"if(typeof ArrayBuffer.prototype.transfer==='function'){let d=new Float32Array(8);d.buffer.transfer();let caught=false;try{H.gouraudPatch(d,ix,ca,cb,mask,hxy,hc)}catch(e){caught=true}if(!caught)throw Error('detached accepted');}");mark_unchanged(ctx,&mark);
 /* Exhaust the raster budget. A further valid append is NOT_APPLIED. */
 unsigned appended=0;for(;;){JSValue g=JS_GetGlobalObject(ctx),fn=JS_GetPropertyStr(ctx,g,"draw");value=JS_Call(ctx,fn,g,0,NULL);JS_FreeValue(ctx,fn);JS_FreeValue(ctx,g);if(JS_IsException(value)){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);JS_FreeValue(ctx,value);break;}JS_FreeValue(ctx,value);++appended;REQUIRE(appended<1000);}
 REQUIRE(appended>0);value=pocket_proc_native_begin(ctx,"gouraud-test",&mark);REQUIRE(!JS_IsException(value));JS_FreeValue(ctx,value);eval_error(ctx,"draw()");mark_unchanged(ctx,&mark);eval_ok(ctx,"P.commit()");band_equal();pocket_proc_present_result(KSN_OK);
 /* Hole changes are evaluated from a numeric snapshot; no remembered clip. */
 eval_ok(ctx,"init();hxy=new Float32Array([14,14,26,14,26,26,14,26]);hc=new Uint8Array([4]);P.beginFrame(2016);draw();P.commit()");band_equal();REQUIRE(expected[20*240+20]==2016&&expected[12*240+12]==63488);eval_ok(ctx,"hxy.fill(NaN)");REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(expected,actual,sizeof expected));pocket_proc_present_result(KSN_OK);
 /* Unaligned foreign Float32 backing is read using memcpy. */
 uint8_t *odd=malloc(33);REQUIRE(odd);const float initial[8]={10,10,30,10,30,30,10,30};memcpy(odd+1,initial,32);global=JS_GetGlobalObject(ctx);REQUIRE(JS_SetPropertyStr(ctx,global,"oddBuffer",JS_NewArrayBuffer(ctx,odd+1,32,odd_free,odd,false))>=0);JS_FreeValue(ctx,global);
 eval_ok(ctx,"init();xy=new Float32Array(oddBuffer);P.beginFrame(2016);draw();P.commit()");band_equal();pocket_proc_present_result(KSN_OK);
 /* The foreign buffer above was a readonly native input. Restore normal
  * backing before exercising JS typed-array writes in subsequent fixtures.
  * Two differently coloured triangles expose double-owned edges. */
 eval_ok(ctx,"xy=new Float32Array(8);init();globalThis.red=new Uint16Array([63488,63488,63488,63488]);globalThis.blue=new Uint16Array([31,31,31,31]);globalThis.ia=new Uint16Array([0,1,3]);globalThis.ib=new Uint16Array([1,2,3]);mask.fill(-1);"
  "P.beginFrame(2016);H.gouraudPatch(xy,ia,red,red,mask,hxy,hc);H.gouraudPatch(xy,ib,blue,blue,mask,hxy,hc);P.commit()");band_equal();memcpy(previous,expected,sizeof previous);pocket_proc_present_result(KSN_OK);
 eval_ok(ctx,"P.beginFrame(2016);H.gouraudPatch(xy,ib,blue,blue,mask,hxy,hc);H.gouraudPatch(xy,ia,red,red,mask,hxy,hc);P.commit()");REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(previous,actual,sizeof actual));pocket_proc_present_result(KSN_OK);
 /* Direct calls isolate update allocation from eval/parser/capture costs. */
 eval_ok(ctx,"init()");global=JS_GetGlobalObject(ctx);JSValue gP=JS_GetPropertyStr(ctx,global,"P"),gH=JS_GetPropertyStr(ctx,global,"H"),begin=JS_GetPropertyStr(ctx,gP,"beginFrame"),commit=JS_GetPropertyStr(ctx,gP,"commit"),fn=JS_GetPropertyStr(ctx,gH,"gouraudPatch");const char *names[]={"xy","ix","ca","cb","mask","hxy","hc"};JSValue args[7];for(unsigned i=0;i<7;i++)args[i]=JS_GetPropertyStr(ctx,global,names[i]);JSValue color=JS_NewInt32(ctx,2016);
 size_t calls=0;for(unsigned tick=0;tick<1004;tick++){
  if(tick==4)calls=ga_calls;
  value=JS_Call(ctx,begin,gP,1,&color);REQUIRE(!JS_IsException(value));JS_FreeValue(ctx,value);
  value=JS_Call(ctx,fn,gH,7,args);REQUIRE(!JS_IsException(value));JS_FreeValue(ctx,value);
  value=JS_Call(ctx,commit,gP,0,NULL);REQUIRE(!JS_IsException(value));JS_FreeValue(ctx,value);pocket_proc_present_result(KSN_OK);
 }
 REQUIRE(ga_calls==calls);for(unsigned i=0;i<7;i++)JS_FreeValue(ctx,args[i]);JS_FreeValue(ctx,color);JS_FreeValue(ctx,fn);JS_FreeValue(ctx,commit);JS_FreeValue(ctx,begin);JS_FreeValue(ctx,gP);JS_FreeValue(ctx,gH);JS_FreeValue(ctx,global);
 pocket_proc_reset();pocket_proc_host_classes_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);REQUIRE(!ga_live);
 printf("GOURAUD_QJS_PASS pointer_bits=%zu patch_max_bytes=%zu appended=%u warm_allocations=0 live_blocks=%zu\n",sizeof(void*)*8,sizeof(ksn_proc_gouraud_patch),appended,ga_live);return 0;
}
