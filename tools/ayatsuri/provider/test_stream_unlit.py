"""Dedicated positions-only stream regression plus all strict API fixtures."""
import pathlib
here=pathlib.Path(__file__).resolve().parent
source=(here/'test_stream_candidate_api.py').read_text()
helpers=r'''
static int position_bad,sink_bad;static bool expect_unlit;static uint64_t observed;
static void hash_bytes(const void*p,size_t n){const unsigned char*b=p;while(n--)observed=(observed^*b++)*UINT64_C(1099511628211);}
static bool positions_only(void*ctx,unsigned node,unsigned count,ayatsuri_stream_position*out){
 ayatsuri_skin_result full[32];if(!provide(ctx,node,count,full))return false;
 for(unsigned j=0;j<count;j++)memcpy(out[j].position,full[j].position,sizeof out[j].position);
 if(position_bad==1)out[count-1].position[0]=NAN;
 if(position_bad==2)out[count-1].position[0]=INFINITY;
 if(position_bad==3)out[count-1].position[0]=4097;
 return true;
}
static bool observe_unlit(void*ctx,const ayatsuri_stream_attributed_mesh*v){
 (void)ctx;if(sink_bad)return false;
 hash_bytes(&v->node_id,sizeof v->node_id);hash_bytes(&v->enabled,sizeof v->enabled);
 hash_bytes(&v->triangle_count,sizeof v->triangle_count);
 for(unsigned j=0;j<v->vertex_count;j++){
  hash_bytes(v->local[j].position,12);hash_bytes(&v->projected[j].x,sizeof(float));
  hash_bytes(&v->projected[j].y,sizeof(float));hash_bytes(v->depth+j,sizeof(float));
  if(expect_unlit){for(unsigned k=0;k<3;k++)assert(isnan(v->local[j].normal[k]));assert(!v->projected[j].color_a&&!v->projected[j].color_b);}
 }
 if(v->triangle_count){hash_bytes(v->indices,6*v->triangle_count);hash_bytes(v->source_triangle,2*v->triangle_count);}
 return true;
}
static uint16_t load16(const uint8_t*p){return (uint16_t)(p[0]|p[1]<<8);}
'''
source=source.replace('int main(int argc,char**argv){',helpers+'\nint main(int argc,char**argv){')
anchor='size_t used=ready.packets.size;memcpy(saved,ready.packets.data,used);ksn_mesh_snapshot ready_before=ready;'
assert source.count(anchor)==1
source=source.replace(anchor,anchor+r'''
  ayatsuri_stream_route routes[23];size_t nr=0;
  for(unsigned j=0;j<m.node_count;j++){const uint8_t*n=m.data+m.section[0].offset+16*j;if(n[2]==3){assert(nr<23);routes[nr++]=(ayatsuri_stream_route){j,load16(n+4),load16(n+8),AYATSURI_STREAM_MATERIAL};}}
  ayatsuri_stream_routes manifest={&m,routes,nr};
  observed=0;expect_unlit=false;
  assert(ayatsuri_stream_candidate_frame_attributed(&stream,&owner,1,camera,light,provide,&p,slot,&pending,&manifest,observe_unlit,NULL)==AYATSURI_MODEL_OK);
  uint64_t reference=observed;observed=0;expect_unlit=true;
  assert(ayatsuri_stream_candidate_frame_attributed_unlit(&stream,&owner,1,camera,light,positions_only,&p,slot,&pending,&manifest,observe_unlit,NULL)==AYATSURI_MODEL_OK);
  assert(observed==reference&&!pending.packets.size&&!pending.packets.packets);
  ksn_mesh_snapshot old_pending=pending;unsigned old_calls=p.calls;
  routes[0].lane=AYATSURI_STREAM_OPAQUE;
  assert(ayatsuri_stream_candidate_frame_attributed_unlit(&stream,&owner,1,camera,light,positions_only,&p,slot,&pending,&manifest,observe_unlit,NULL)==AYATSURI_MODEL_INVALID);
  routes[0].lane=AYATSURI_STREAM_MATERIAL;
  assert(ayatsuri_stream_candidate_frame_attributed_unlit(&stream,&owner,1,camera,light,positions_only,&p,slot,&pending,&manifest,NULL,NULL)==AYATSURI_MODEL_INVALID);
  assert(p.calls==old_calls&&!memcmp(&old_pending,&pending,sizeof pending));
  for(position_bad=1;position_bad<=3;position_bad++)assert(ayatsuri_stream_candidate_frame_attributed_unlit(&stream,&owner,1,camera,light,positions_only,&p,slot,&pending,&manifest,observe_unlit,NULL)!=AYATSURI_MODEL_OK);
  position_bad=0;sink_bad=1;
  assert(ayatsuri_stream_candidate_frame_attributed_unlit(&stream,&owner,1,camera,light,positions_only,&p,slot,&pending,&manifest,observe_unlit,NULL)!=AYATSURI_MODEL_OK);sink_bad=0;
  assert(!memcmp(&ready,&ready_before,sizeof ready)&&!memcmp(saved,ready.packets.data,used));
''')
source=source.replace('PASS streaming frames=24','PASS unlit/strict streaming frames=24')
exec(compile(source,str(here/'test_stream_candidate_api.py'),'exec'))
