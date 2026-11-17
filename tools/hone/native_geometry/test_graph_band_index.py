"""Index collector against the existing unrestricted randomized reference."""
import pathlib
here=pathlib.Path(__file__).resolve().parent
source=(here/'test_alpha_graph_occlusion.py').read_text().replace('alpha_graph_occlusion_candidate.h','graph_band_index_candidate.h')
helper=r'''
static int indexed(const hone_graph_snapshot*s,unsigned x,unsigned y,float depth,float*scratch,size_t count,hone_graph_ordered*out){
 hone_graph_band_index index={0};hone_graph_bounds bounds[32];uint16_t ids[32*17];
 if(!hone_graph_index_build(&index,s,1,bounds,32,ids,32*17))return -1;
 hone_graph_band_index saved=index;
 assert(!hone_graph_index_build(&index,s,2,bounds,32,ids,0)||index.used==0);
 if(saved.used){assert(!memcmp(&index,&saved,sizeof index));}
 assert(hone_graph_index_fragments(&saved,s,2,x,y,depth,scratch,count,out)==-1);
 return hone_graph_index_fragments(&saved,s,1,x,y,depth,scratch,count,out);
}
'''
source=source.replace('static uint32_t state=17;',helper+'\nstatic uint32_t state=17;').replace('hone_graph_fragments_occlusion(', 'indexed(')
source=source.replace('float scratch[16];unsigned pixels=0,overflows=0;', 's.owner=&s;s.generation=1;float scratch[16];unsigned pixels=0,overflows=0;')

source=source.replace(' /* An invalid fragment behind exact opaque coverage MUST still fail. */',r"""
 /* Integer sample bounds at viewport endpoints and both sides of 8-row edges. */
 const unsigned ys[]={0,7,8,127,128,134};s.count=1;
 for(unsigned j=0;j<6;j++)for(unsigned side=0;side<2;side++){
  float x=side?239:0,y=(float)ys[j];code[0][0].value=1;
  packets[0].triangle.v[0]=(hone_graph_vertex){x-1,y-1,3,{0,0,0}};
  packets[0].triangle.v[1]=(hone_graph_vertex){x+1,y-1,3,{1,0,0}};
  packets[0].triangle.v[2]=(hone_graph_vertex){x-1,y+1,3,{0,1,0}};
  for(unsigned yy=0;yy<135;yy++)for(unsigned xx=0;xx<240;xx++){
   hone_graph_ordered full[32],q[4];int n=reference(&s,xx,yy,-INFINITY,scratch,full),k=indexed(&s,xx,yy,-INFINITY,scratch,16,q);assert(n==k);
   if(n){float a[3],b[3];blend(full,n,a);blend(q,k,b);assert(!memcmp(a,b,sizeof a));}
  }
 }
 /* Nonfinite offscreen geometry must fail index construction, not get culled. */
 packets[0].triangle.v[0].x=NAN;hone_graph_band_index bad={0},saved_index=bad;hone_graph_bounds bounds[32];uint16_t ids[544];
 assert(!hone_graph_index_build(&bad,&s,1,bounds,32,ids,544)&&!memcmp(&bad,&saved_index,sizeof bad));
 /* An invalid fragment behind exact opaque coverage MUST still fail. */
""")

source=source.replace('OCCLUSION PASS','BAND_INDEX PASS')
exec(compile(source,str(here/'test_alpha_graph_occlusion.py'),'exec'))
