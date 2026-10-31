/* Diagnostic host only: real AYNM evaluator and real procedural Gouraud raster.
 * No QuickJS, SDK, device, alternate depth rasterizer or product-code edits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "ayatsuri/ayatsuri_model.h"
#include "ui/kasane/ksn_procedural.h"
#define PIXELS (KSN_PROC_W*KSN_PROC_H)
static ksn_proc_frame normal_frame,coverage_frame,triangle_frame;
static uint16_t pixels[PIXELS],coverage[PIXELS],tags[PIXELS];
static unsigned u16(const uint8_t *p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
static float f32le(const uint8_t *p){uint32_t u=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;float f;memcpy(&f,&u,4);return f;}
static int fail(const char *s){fprintf(stderr,"native render probe: %s\n",s);return 1;}
static int number(const char *s,float *out){char *end;errno=0;double d=strtod(s,&end);if(errno||!s[0]||*end||!isfinite(d)||!isfinite((float)d))return 0;*out=(float)d;return 1;}
/* Same packet serialization and 1024-segment/UINT16-raster accounting as
 * pocket_proc_native_gouraud; no JS entrypoint or submission is involved. */
static int append(ksn_proc_frame *f,const ksn_proc_gouraud_patch *p){
 int l,t,r,b;unsigned cost;
 if(!ksn_proc_gouraud_bounds(p,&l,&t,&r,&b,&cost))return 0;
 if(l>r||t>b)return 1;
 unsigned words=(unsigned)KSN_PROC_GOURAUD_WORDS(p->header.vertices,p->header.triangles,p->header.holes);
 if(words+1u>(unsigned)KSN_PROC_SEGMENTS-f->count||cost>(unsigned)UINT16_MAX-f->raster_steps)return 0;
 f->segments[f->count++]=(ksn_proc_segment){(int16_t)(l+KSN_PROC_EXT_GOURAUD_PATCH),(int16_t)t,(int16_t)r,(int16_t)b,0};
 uint8_t *out=(uint8_t *)(f->segments+f->count);memset(out,0,words*sizeof f->segments[0]);
 memcpy(out,&p->header,sizeof p->header);out+=sizeof p->header;
 size_t n=p->header.vertices*sizeof p->vertices[0];memcpy(out,p->vertices,n);out+=n;
 n=p->header.triangles*3u*sizeof p->indices[0];memcpy(out,p->indices,n);out+=n;
 memcpy(out,p->clips,p->header.holes*sizeof p->clips[0]);
 f->count+=(uint16_t)words;f->raster_steps=(uint16_t)(f->raster_steps+cost);f->ext=1;f->ready=true;return 1;
}
static int render(const ksn_proc_frame *f,uint16_t *out){
 for(int y=0;y<KSN_PROC_H;y+=8){int h=KSN_PROC_H-y;if(h>8)h=8;if(!ksn_proc_render_band(f,out+y*KSN_PROC_W,y,h))return 0;}return 1;
}
static int write_pixels(const char *folder,const char *name,const uint16_t *p){
 char path[4096];if(snprintf(path,sizeof path,"%s/%s",folder,name)>=(int)sizeof path)return 0;
 FILE *f=fopen(path,"wb");if(!f)return 0;
 for(unsigned i=0;i<PIXELS;i++){uint8_t b[2]={(uint8_t)p[i],(uint8_t)(p[i]>>8)};if(fwrite(b,1,2,f)!=2){fclose(f);return 0;}}
 return fclose(f)==0;
}
static void float_list(FILE *f,const float *p,unsigned n){fputc('[',f);for(unsigned j=0;j<n;j++)fprintf(f,"%s%.9g",j?",":"",(double)p[j]);fputc(']',f);}
int main(int argc,char **argv){
 /* input, output directory, camera9, light6, useRootPose, rootPose9 */
 if(argc!=28)return fail("expected input output camera9 light6 useRootPose rootPose9");
 float camera[9],light[6],rootpose[9];for(unsigned k=0;k<9;k++)if(!number(argv[3+k],camera+k))return fail("invalid camera");
 for(unsigned k=0;k<6;k++)if(!number(argv[12+k],light+k))return fail("invalid light");
 if(strcmp(argv[18],"0")&&strcmp(argv[18],"1"))return fail("invalid root pose flag");
 for(unsigned k=0;k<9;k++)if(!number(argv[19+k],rootpose+k))return fail("invalid root pose");
 FILE *file=fopen(argv[1],"rb");if(!file)return fail("cannot open asset");
 if(fseek(file,0,SEEK_END)){fclose(file);return fail("seek failed");}long size=ftell(file);
 if(size<=0||size>AYATSURI_MODEL_MAX_BYTES){fclose(file);return fail("asset size");}rewind(file);
 uint8_t *blob=malloc((size_t)size);if(!blob){fclose(file);return fail("allocation");}
 if(fread(blob,1,(size_t)size,file)!=(size_t)size){fclose(file);free(blob);return fail("asset read");}fclose(file);
 ayatsuri_model model;if(ayatsuri_model_open(blob,(size_t)size,&model)!=AYATSURI_MODEL_OK){free(blob);return fail("reader rejected asset");}
 size_t arena_bytes=ayatsuri_model_instance_bytes(&model);void *allocation=malloc(arena_bytes+7);
 if(!allocation){free(blob);return fail("arena allocation");}void *arena=(void *)(((uintptr_t)allocation+7u)&~(uintptr_t)7u);
 ayatsuri_model_instance instance;int owner=0,rc=1;FILE *meta=NULL;
 if(ayatsuri_model_instance_init(&model,arena,arena_bytes,&owner,1,&instance)!=AYATSURI_MODEL_OK)goto cleanup;
 if(argv[18][0]=='1'){
  uint8_t dirty[AYATSURI_MODEL_MAX_NODES]={0};float pose[9*AYATSURI_MODEL_MAX_NODES]={0};uint32_t color[AYATSURI_MODEL_MAX_NODES]={0};double opacity[AYATSURI_MODEL_MAX_NODES]={0};
  dirty[0]=7;memcpy(pose,rootpose,sizeof rootpose);ayatsuri_model_targets target={dirty,pose,color,opacity,model.node_count};
  if(ayatsuri_model_instance_targets(&instance,&owner,1,&target)!=AYATSURI_MODEL_OK)goto dispose;
 }
 if(ayatsuri_model_instance_evaluate(&instance,&owner,1,camera,light)!=AYATSURI_MODEL_OK)goto dispose;
 char path[4096];if(snprintf(path,sizeof path,"%s/metadata.json",argv[2])>=(int)sizeof path)goto dispose;
 meta=fopen(path,"w");if(!meta)goto dispose;
 fprintf(meta,"{\"schema\":\"aynm-native-raster-probe-v1\",\"width\":240,\"height\":135,\"pixelFormat\":\"RGB565_U16LE\",\"background\":0,\"pointerBits\":%zu,\"coldBytes\":%ld,\"arenaBytes\":%zu,\"nodes\":%u,\"camera\":",sizeof(void*)*8,size,arena_bytes,(unsigned)model.node_count);
 float_list(meta,camera,9);fputs(",\"evaluatedLighting\":",meta);float_list(meta,instance.lighting,9);fputs(",\"light\":",meta);float_list(meta,light,6);fprintf(meta,",\"rootPoseApplied\":%s,\"rootPose\":",argv[18][0]=='1'?"true":"false");float_list(meta,rootpose,9);fputs(",\"packets\":[",meta);
 uint32_t cursor=0;ayatsuri_model_packet packet;unsigned packet_count=0,tag=0;ayatsuri_model_status status;
 normal_frame.ready=coverage_frame.ready=true;
 while((status=ayatsuri_model_instance_next(&instance,&owner,1,&cursor,&packet))==AYATSURI_MODEL_OK){
  if(packet.kind!=4){fail("mesh-only probe rejects non-Gouraud visible packets");goto dispose;}
  ksn_proc_gouraud_patch p;memcpy(&p,packet.storage+84,sizeof p);unsigned nv=p.header.vertices,nt=p.header.triangles;
  if(!append(&normal_frame,&p)){fail("normal frame segment/raster quota exceeded");goto dispose;}
  const uint8_t *scratch=packet.storage+84+sizeof p;float depths[KSN_PROC_GOURAUD_VERTICES];memcpy(depths,scratch+nv*2*sizeof(float),nv*sizeof(float));
  fprintf(meta,"%s{\"packetIndex\":%u,\"nodeId\":%u,\"kind\":4,\"vertices\":[",packet_count?",":"",packet_count,(unsigned)packet.node_id);
  /* Cold indices are model section10 row IDs, not source-mesh global IDs. */
  const uint8_t *coldnode=model.data+model.section[0].offset+16u*packet.node_id;
  const uint8_t *descriptor=model.data+model.section[8].offset+32u*u16(coldnode+4);
  const uint8_t *material=model.data+model.section[4].offset+72u*u16(coldnode+8);
  for(unsigned v=0;v<nv;v++){const ksn_proc_gouraud_vertex *a=p.vertices+v;fprintf(meta,"%s{\"x\":%.9g,\"y\":%.9g,\"depth\":%.9g,\"mask\":%.9g,\"colorA\":%u,\"colorB\":%u}",v?",":"",(double)a->x,(double)a->y,(double)depths[v],(double)a->mask,(unsigned)a->color_a,(unsigned)a->color_b);}
  fputs("],\"coldMaterial\":[",meta);for(unsigned k=0;k<8;k++)fprintf(meta,"%s%.9g",k?",":"",(double)f32le(material+4*k));
  fprintf(meta,"],\"albedoA\":%u,\"albedoB\":%u,\"thickness\":%u,\"coldVertexRowIndices\":[",u16(descriptor+14),u16(descriptor+16),(unsigned)descriptor[18]);
  for(unsigned k=0;k<nv;k++)fprintf(meta,"%s%u",k?",":"",u16(descriptor)+k);
  fputs("],\"triangles\":[",meta);for(unsigned t=0;t<nt;t++)fprintf(meta,"%s[%u,%u,%u]",t?",":"",p.indices[3*t],p.indices[3*t+1],p.indices[3*t+2]);
  fputs("],\"triangleTags\":[",meta);for(unsigned t=0;t<nt;t++)fprintf(meta,"%s%u",t?",":"",tag+t+1);
  fputs("],\"holes\":[",meta);for(unsigned h=0;h<p.header.holes;h++){fprintf(meta,"%s{\"xy\":",h?",":"");float_list(meta,p.clips[h].xy,p.clips[h].count*2u);fputc('}',meta);}fputs("]}",meta);
  ksn_proc_gouraud_patch white=p;for(unsigned v=0;v<nv;v++)white.vertices[v].color_a=white.vertices[v].color_b=65535;
  if(!append(&coverage_frame,&white))goto dispose;
  for(unsigned t=0;t<nt;t++){
   if(++tag>65535){fail("triangle tag range exceeded");goto dispose;}
   ksn_proc_gouraud_patch one=p;one.header.triangles=1;memset(one.indices,0,sizeof one.indices);memcpy(one.indices,p.indices+3*t,3*sizeof(uint16_t));
   for(unsigned v=0;v<nv;v++)one.vertices[v].color_a=one.vertices[v].color_b=(uint16_t)tag;
   memset(&triangle_frame,0,sizeof triangle_frame);triangle_frame.ready=true;
   if(!append(&triangle_frame,&one)||!render(&triangle_frame,tags))goto dispose;
  }
  packet_count++;
 }
 if(status!=AYATSURI_MODEL_DONE||!render(&normal_frame,pixels)||!render(&coverage_frame,coverage))goto dispose;
 unsigned mismatch=0,covered=0;for(unsigned i=0;i<PIXELS;i++){mismatch+=(tags[i]!=0)!=(coverage[i]!=0);covered+=coverage[i]!=0;}
 fprintf(meta,"],\"packetCount\":%u,\"triangleCount\":%u,\"frameSegments\":%u,\"frameRasterCost\":%u,\"coveredPixels\":%u,\"tagCoverageMismatch\":%u,\"tagPass\":\"diagnostic constant-tag triangles replayed in identical order with reusable one-triangle frames; not production quota proof\",\"renderer\":\"real ksn_proc_render_band, 8-row bands\",\"device\":\"NOT TESTED\"}\n",packet_count,tag,(unsigned)normal_frame.count,(unsigned)normal_frame.raster_steps,covered,mismatch);
 if(fclose(meta)){meta=NULL;goto dispose;}meta=NULL;
 if(mismatch){fail("tag coverage mismatch");goto dispose;}
 if(!write_pixels(argv[2],"pixels.u16le",pixels)||!write_pixels(argv[2],"coverage.u16le",coverage)||!write_pixels(argv[2],"visible-triangles.u16le",tags))goto dispose;
 rc=0;
 dispose:if(meta)fclose(meta);if(ayatsuri_model_instance_dispose(&instance,&owner,1)!=AYATSURI_MODEL_OK)rc=1;
 cleanup:free(allocation);free(blob);if(rc)fail("probe did not complete");return rc;
}

