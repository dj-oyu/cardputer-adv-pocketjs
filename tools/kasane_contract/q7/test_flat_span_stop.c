#include <assert.h>
#include <stdio.h>
#include "../../../main/ui/kasane/ksn_mesh_q7_certified.c"
int main(void){
 ksn_mesh_q7_certificate c={0};c.p[0][0]=0x1p-17;c.p[0][2]=400;c.error[0]=0x1p-30;c.valid=true;
 float z[240];uint8_t owner[240];uint16_t pixels[240];
 for(unsigned reject=0;reject<2;reject++){
  for(unsigned i=0;i<240;i++){z[i]=reject?500:-INFINITY;owner[i]=255;pixels[i]=0x1234;}
  q7_row_stats st={0};q7_flat_span span={&c,z,owner,pixels,0,0xf800,7};
  int x=flat_span_run(&span,1,8,&st);assert(x==2&&st.depth_fast==1&&st.span_pixels==1);
  assert(st.depth_reject==reject&&st.mask_evals==1-reject&&st.mask_fast==1-reject);
  assert(owner[1]==(reject?255:7)&&pixels[1]==(reject?0x1234:0xf800));
  assert(owner[2]==255&&pixels[2]==0x1234&&z[2]==(reject?500:-INFINITY));
  q7_row_stats before=st;assert(flat_span_run(&span,2,8,&st)==2&&!memcmp(&before,&st,sizeof st));
  assert(flat_span_run(&span,3,8,&st)==6&&st.depth_fast==4&&st.span_pixels==4);
  assert(owner[6]==255&&pixels[6]==0x1234);
 }
 puts("FLAT_SPAN_STOP_PASS first uncertain pixel untouched; partial and immediate failure; reject/winner counters exact");
}
