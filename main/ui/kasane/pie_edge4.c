#include "pie_edge4.h"
#include <math.h>
#include <limits.h>
#include <string.h>
static bool admitted(const int32_t a[3]){
 int64_t aa=a[0]<0?-(int64_t)a[0]:a[0],bb=a[1]<0?-(int64_t)a[1]:a[1],cc=a[2]<0?-(int64_t)a[2]:a[2];
 return aa*246+bb*134+cc+PIE_EDGE4_LIMIT<INT32_MAX;
}
bool pie_edge4_prepare(const double abc[3][3],pie_edge4_coeff*out){
 if(!abc||!out)return false;
 pie_edge4_coeff temp;
 for(unsigned k=0;k<3;k++){
  for(unsigned j=0;j<3;j++)if(!isfinite(abc[k][j]))return false;
  bool ok=false;
  for(int e=24;e>=-8;e--){
   double a=round(ldexp(abc[k][0],e)),b=round(ldexp(abc[k][1],e)),c=round(ldexp(abc[k][2],e));
   if(!isfinite(a)||!isfinite(b)||!isfinite(c)||fabs(a)*246+fabs(b)*134+fabs(c)+PIE_EDGE4_LIMIT>=INT32_MAX)continue;
   temp.abc[k][0]=(int32_t)a;temp.abc[k][1]=(int32_t)b;temp.abc[k][2]=(int32_t)c;ok=true;break;
  }
  if(!ok)return false;
 }
 memcpy(out,&temp,sizeof temp);return true;
}
bool pie_edge4_init(const pie_edge4_coeff*q,unsigned x,unsigned y,pie_edge4_lanes*out){
 if(!q||!out||x>239||y>134||((uintptr_t)out&15))return false;
 pie_edge4_lanes temp;
 for(unsigned k=0;k<3;k++){
  if(!admitted(q->abc[k]))return false;
  for(unsigned i=0;i<4;i++)temp.v[k][i]=(int32_t)((int64_t)q->abc[k][0]*(x+i)+(int64_t)q->abc[k][1]*y+q->abc[k][2]);
 }
 memcpy(out,&temp,sizeof temp);return true;
}
_Static_assert(sizeof(pie_edge4_coeff)==36,"coefficient storage");
_Static_assert(sizeof(pie_edge4_lanes)==48 && _Alignof(pie_edge4_lanes)==16,"lanes");
_Static_assert(sizeof(pie_edge4_mask)==16 && _Alignof(pie_edge4_mask)==16,"masks");
