"""Retain all streaming fixture assertions, adding the typed observer and failures."""
import pathlib

here=pathlib.Path(__file__).resolve().parent
source=(here/'test_stream_candidate_api.py').read_text()
source=source.replace('#include "ayatsuri/ayatsuri_model_stream_candidate.h"',
    '#include "ayatsuri/ayatsuri_model_stream_candidate.h"\n#include "'+str(here/'stream_attributed_observer.h').replace('\\','/')+'"')
source=source.replace('(float)(2*sin(p->frame*.1+node))', '(float)(2*sin(p->frame*.1+node)+.15*v)')
source=source.replace('ayatsuri_stream_candidate_frame(', 'attributed_probe_frame(')
anchor='struct Producer p={frame,0,0};'
assert source.count(anchor)==1
source=source.replace(anchor,'attributed_oracle=&oracle;'+anchor)
anchor='size_t used=ready.packets.size;memcpy(saved,ready.packets.data,used);ksn_mesh_snapshot ready_before=ready;'
assert source.count(anchor)==1
source=source.replace(anchor,anchor+r'''
  attributed_fail=12;assert(attributed_probe_frame(&stream,&owner,1,camera,light,provide,&p,slot,&pending)!=AYATSURI_MODEL_OK);
  assert(!memcmp(&ready,&ready_before,sizeof ready)&&!memcmp(saved,ready.packets.data,used));attributed_fail=-1;
  ayatsuri_stream_route routes[256];size_t nr=attributed_routes(&m,routes);ayatsuri_stream_routes manifest={&m,routes,nr};
  attributed_probe observer={.model=&m,.provider=provide,.context=&p};unsigned calls=p.calls;
  routes[0].material_id++;ksn_mesh_snapshot old_pending=pending;
  assert(ayatsuri_stream_candidate_frame_attributed(&stream,&owner,1,camera,light,attributed_provider,&observer,slot,&pending,&manifest,attributed_observe,&observer)==AYATSURI_MODEL_INVALID);
  assert(p.calls==calls&&!memcmp(&pending,&old_pending,sizeof pending));routes[0].material_id--;
  routes[0].lane=2;assert(ayatsuri_stream_candidate_frame_attributed(&stream,&owner,1,camera,light,attributed_provider,&observer,slot,&pending,&manifest,attributed_observe,&observer)==AYATSURI_MODEL_INVALID);routes[0].lane=AYATSURI_STREAM_OPAQUE;
  routes[1].node_id=routes[0].node_id;assert(ayatsuri_stream_candidate_frame_attributed(&stream,&owner,1,camera,light,attributed_provider,&observer,slot,&pending,&manifest,attributed_observe,&observer)==AYATSURI_MODEL_INVALID);attributed_routes(&m,routes);
  assert(p.calls==calls&&!memcmp(&pending,&old_pending,sizeof pending));
  manifest.count--;assert(ayatsuri_stream_candidate_frame_attributed(&stream,&owner,1,camera,light,attributed_provider,&observer,slot,&pending,&manifest,attributed_observe,&observer)==AYATSURI_MODEL_INVALID);manifest.count++;
  manifest.model=&ordinary;assert(ayatsuri_stream_candidate_frame_attributed(&stream,&owner,1,camera,light,attributed_provider,&observer,slot,&pending,&manifest,attributed_observe,&observer)==AYATSURI_MODEL_INVALID);manifest.model=&m;
  for(size_t r=0;r<nr;r++)routes[r].lane=AYATSURI_STREAM_MATERIAL;
  assert(ayatsuri_stream_candidate_frame_attributed(&stream,&owner,1,camera,light,attributed_provider,&observer,slot,&pending,&manifest,attributed_observe,&observer)==AYATSURI_MODEL_OK);
  assert(!pending.packets.size&&!pending.packets.packets);
''')
source=source.replace('printf("PASS streaming frames=24',
    'printf("ATTRIBUTED calls=%u hidden=%u faces=%u route_rejection/material_lane/rollback PASS\\n",attributed_calls,attributed_hidden,attributed_faces);\n printf("PASS streaming frames=24')
exec(compile(source,str(here/'test_stream_candidate_api.py'),'exec'))

# Repeated authored triples retain separate IDs, including stable centroid ties.
fixture=(here/'fixture.cjs').read_text().replace("require('../model/pack_model.cjs')",'require(process.argv[3])')
fixture=fixture.replace('nodes.push(', 'idx.push(0,1,2);nodes.push(').replace("order:'authored'", "order:'centroid-depth'")
(out/'ordered.cjs').write_text(fixture)
subprocess.run(['node',str(out/'ordered.cjs'),str(out/'ordered.aynm'),str(root/'tools/ayatsuri/model/pack_model.cjs')],check=True)
subprocess.run([str(binary),str(out/'ordered.aynm')],check=True)
