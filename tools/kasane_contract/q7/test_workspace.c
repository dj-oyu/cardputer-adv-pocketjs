#include "ksn_mesh_q7_workspace.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(void){
 unsigned char *raw=malloc(128);assert(raw);
 for(size_t alignment=1;alignment<=32;alignment++)for(size_t offset=0;offset<32;offset++){
  size_t total;assert(ksn_mesh_q7_workspace_allocation(32,alignment,&total));
  void *p=ksn_mesh_q7_workspace_align(raw+offset,total,32,alignment);
  assert(p&&(uintptr_t)p%alignment==0);
  assert((unsigned char *)p>=raw+offset&&(unsigned char *)p+32<=raw+offset+total);
 }
 size_t total=17;assert(!ksn_mesh_q7_workspace_allocation(SIZE_MAX,8,&total)&&total==17);
 assert(!ksn_mesh_q7_workspace_allocation(32,0,&total));
 assert(!ksn_mesh_q7_workspace_align(NULL,40,32,8));
 assert(!ksn_mesh_q7_workspace_align(raw,0,32,8));
 assert(!ksn_mesh_q7_workspace_align((void *)UINTPTR_MAX,40,32,8));
 free(raw);puts("Q7_WORKSPACE_PASS arbitrary base offsets and alignments, overflow and capacity guards");
}
