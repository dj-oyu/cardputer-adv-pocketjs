#include <assert.h>
#include "cold_slot_candidate.h"
int main(void){float p[]={1,2,3},n[]={0,0,1,0,1,0};hone_cold_corner map[]={{0,0},{0,1}};ayatsuri_skin_result slot[32];
assert(hone_cold_slot(p,1,n,2,map,2,1,1,1,slot));assert(slot[0].position[0]==slot[1].position[0]);assert(slot[0].normal[2]==1&&slot[1].normal[1]==1);
assert(!hone_cold_slot(p,1,n,2,map,2,1,2,1,slot));map[1].corner=2;assert(!hone_cold_slot(p,1,n,2,map,2,1,1,1,slot));map[1].corner=1;
p[0]=2;assert(hone_cold_slot(p,1,n,2,map,2,2,2,2,slot));assert(slot[0].position[0]==2);
n[5]=NAN;assert(!hone_cold_slot(p,1,n,2,map,2,2,2,2,slot));n[5]=0;assert(hone_cold_slot(p,1,n,2,map,2,2,2,2,slot));return 0;}
