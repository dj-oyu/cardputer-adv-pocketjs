#include "visibility.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

/* Original vertex identity is preserved; validation precedes exact narrowing. */
_Static_assert(VIS_MAX_VERTICES-1u<=UINT16_MAX, "vertex index storage");
_Static_assert(VIS_MAX_TRIANGLES-1u<=UINT16_MAX, "triangle ordinal storage");
typedef struct { uint16_t vertex[3], ordinal; } triangle;
typedef struct { double center[3]; } build_center;
typedef struct { double lo[3], hi[3]; uint32_t left,right,start,count; } node;
struct vis_scene { size_t refs, bytes, triangles, nodes, node_capacity, vertices;
                   vis_generation generation; triangle *tri; node *tree; double *xyz; };
static int equal(vis_generation a,vis_generation b) {
 return a.geometry==b.geometry && a.pose==b.pose && a.light==b.light;
}
static int valid_generation(vis_generation g) { return g.geometry && g.pose && g.light; }
/* Exact capacity for the unchanged median split and leaf threshold. Only the
 * checked public entry accepts external counts. Maximum recursion depth is ten. */
static size_t required_nodes_valid(size_t n){
 if(n<=4)return 1;
 size_t left=required_nodes_valid(n/2),right=required_nodes_valid(n-n/2);
 if(!left || !right || right==SIZE_MAX || left>SIZE_MAX-1-right)return 0;
 return 1+left+right;
}
size_t vis_scene_required_nodes(size_t n){
 if(!n || n>VIS_MAX_TRIANGLES)return 0;
 return required_nodes_valid(n);
}
static size_t required_bytes_checked(size_t vertices,size_t n,size_t nodes){
 if(!vertices || vertices>VIS_MAX_VERTICES || !n || n>VIS_MAX_TRIANGLES || !nodes)return 0;
 size_t bytes=sizeof(vis_scene);
 if(vertices>(SIZE_MAX-bytes)/(3*sizeof(double)))return 0;
 bytes+=vertices*3*sizeof(double);
 if(n>(SIZE_MAX-bytes)/sizeof(triangle))return 0;
 bytes+=n*sizeof(triangle);
 if(nodes>(SIZE_MAX-bytes)/sizeof(node))return 0;
 return bytes+nodes*sizeof(node);
}
size_t vis_scene_required_bytes_v3(size_t vertices,size_t n) {
 return required_bytes_checked(vertices,n,vis_scene_required_nodes(n));
}
size_t vis_scene_build_temporary_bytes_v3(size_t n){
 if(!n || n>VIS_MAX_TRIANGLES || n>SIZE_MAX/sizeof(build_center))return 0;
 return n*sizeof(build_center);
}
static int spans_overlap(const void *a,size_t an,const void *b,size_t bn){
 if(!a || !b || !an || !bn)return 0;
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
 /* Subtraction avoids wrapping the end pointer. Invalid wrapping spans are
  * conservatively overlapping, so adapters reject them. */
 if(an-1>UINTPTR_MAX-x || bn-1>UINTPTR_MAX-y)return 1;
 return x<=y ? y-x<an : x-y<bn;
}
int vis_scene_overlaps(const vis_scene *s,const void *p,size_t n){
 return s && (spans_overlap(s,sizeof(*s),p,n) ||
  spans_overlap(s->tri,s->triangles*sizeof(triangle),p,n) ||
  spans_overlap(s->xyz,s->vertices*3*sizeof(double),p,n) ||
  spans_overlap(s->tree,s->node_capacity*sizeof(node),p,n));
}
size_t vis_scene_bytes(const vis_scene *s){return s?s->bytes:0;}
size_t vis_scene_triangle_count(const vis_scene *s){return s?s->triangles:0;}
size_t vis_scene_node_count(const vis_scene *s){return s?s->nodes:0;}
vis_generation vis_scene_generation(const vis_scene *s){vis_generation z={0,0,0};return s?s->generation:z;}
vis_status vis_scene_retain(vis_scene *s){if(!s)return VIS_INVALID;if(s->refs==SIZE_MAX)return VIS_CAPACITY;s->refs++;return VIS_OK;}
void vis_scene_release(vis_scene *s){if(s && --s->refs==0){free(s->xyz);free(s->tri);free(s->tree);free(s);}}
/* Stable in-place insertion sort avoids hidden allocator/global comparator state.
 * Bounded n<=2048; median split guarantees a small, bounded traversal stack. */
static void sort_axis(triangle *t,const build_center *work,size_t n,int axis){
 for(size_t i=1;i<n;i++){triangle x=t[i];size_t j=i;
  while(j && (work[t[j-1].ordinal].center[axis]>work[x.ordinal].center[axis] ||
   (work[t[j-1].ordinal].center[axis]==work[x.ordinal].center[axis] && t[j-1].ordinal>x.ordinal))){t[j]=t[j-1];j--;}
  t[j]=x;
 }
}
/* Identical per-triangle nesting and node scan order preserve signed-zero bits.
 * Only construction repeats these owned-xyz gathers; traversal is unchanged. */
static void build_bounds(const vis_scene *s,const triangle *t,double lo[3],double hi[3]){
 double p[3][3];
 for(int j=0;j<3;j++)memcpy(p[j],s->xyz+3*(size_t)t->vertex[j],3*sizeof(double));
 for(int k=0;k<3;k++){
  lo[k]=fmin(p[0][k],fmin(p[1][k],p[2][k]));
  hi[k]=fmax(p[0][k],fmax(p[1][k],p[2][k]));
 }
}
static uint32_t build_node(vis_scene *s,const build_center *work,size_t start,size_t count){
 if(s->nodes>=s->node_capacity)return UINT32_MAX;
 uint32_t id=(uint32_t)s->nodes++;node *b=&s->tree[id];
 b->start=(uint32_t)start;b->count=(uint32_t)count;b->left=b->right=UINT32_MAX;
 build_bounds(s,&s->tri[start],b->lo,b->hi);
 for(size_t i=start+1;i<start+count;i++){
  double lo[3],hi[3];build_bounds(s,&s->tri[i],lo,hi);
  for(int k=0;k<3;k++){
   b->lo[k]=fmin(b->lo[k],lo[k]);b->hi[k]=fmax(b->hi[k],hi[k]);}
 }
 if(count>4){int axis=0;for(int k=1;k<3;k++)if(b->hi[k]-b->lo[k]>b->hi[axis]-b->lo[axis])axis=k;
  sort_axis(s->tri+start,work,count,axis);size_t half=count/2;
  b->left=build_node(s,work,start,half);if(b->left==UINT32_MAX)return UINT32_MAX;
  b->right=build_node(s,work,start+half,count-half);if(b->right==UINT32_MAX)return UINT32_MAX;
  b->count=0;
 }return id;
}
vis_status vis_scene_build(vis_scene **slot,const double *xyz,size_t vertices,
 const uint32_t *indices,size_t n,vis_generation g,vis_limits lim){
 if(!slot || !xyz || !indices || !vertices || vertices>VIS_MAX_VERTICES || !n || !valid_generation(g))return VIS_INVALID;
 size_t nodes=vis_scene_required_nodes(n),bytes=required_bytes_checked(vertices,n,nodes);
 if(!bytes || n>lim.max_triangles || nodes>lim.max_nodes || nodes>VIS_MAX_NODES || bytes>lim.max_bytes)return VIS_CAPACITY;
 if(spans_overlap(slot,sizeof(*slot),xyz,vertices*3*sizeof(*xyz)) ||
    spans_overlap(slot,sizeof(*slot),indices,n*3*sizeof(*indices)))return VIS_INVALID;
 if(*slot && vis_scene_overlaps(*slot,slot,sizeof(*slot)))return VIS_INVALID;
 if(*slot){vis_generation old=(*slot)->generation;
  if(g.geometry<old.geometry || g.pose<old.pose || g.light<old.light || equal(g,old))return VIS_GENERATION;}
 /* Restrict enormous coordinates to ensure finite arithmetic for ray/triangle
  * operations; this is far beyond useful Blender-world modeling magnitudes. */
 for(size_t i=0;i<vertices*3;i++)if(!isfinite(xyz[i]) || fabs(xyz[i])>1e50)return VIS_INVALID;
 for(size_t i=0;i<n*3;i++)if(indices[i]>=vertices)return VIS_INVALID;
 vis_scene *s=calloc(1,sizeof(*s));if(!s)return VIS_ALLOC;
 s->refs=1;s->bytes=bytes;s->triangles=n;s->node_capacity=nodes;s->vertices=vertices;s->generation=g;
 s->xyz=malloc(vertices*3*sizeof(double));
 s->tri=malloc(n*sizeof(triangle));s->tree=malloc(s->node_capacity*sizeof(node));
 build_center *work=malloc(vis_scene_build_temporary_bytes_v3(n));
 if(!s->xyz || !s->tri || !s->tree || !work){free(work);vis_scene_release(s);return VIS_ALLOC;}
 memcpy(s->xyz,xyz,vertices*3*sizeof(double));
 for(size_t i=0;i<n;i++){triangle *t=&s->tri[i];t->ordinal=(uint16_t)i;
  double p[3][3];
  for(int j=0;j<3;j++){t->vertex[j]=(uint16_t)indices[3*i+j];memcpy(p[j],s->xyz+3*(size_t)t->vertex[j],3*sizeof(double));}
  for(int k=0;k<3;k++){double lo=fmin(p[0][k],fmin(p[1][k],p[2][k]));double hi=fmax(p[0][k],fmax(p[1][k],p[2][k]));work[i].center[k]=(lo+hi)*0.5;}
  double a[3],b[3],c[3];for(int k=0;k<3;k++){a[k]=p[1][k]-p[0][k];b[k]=p[2][k]-p[0][k];}
  c[0]=a[1]*b[2]-a[2]*b[1];c[1]=a[2]*b[0]-a[0]*b[2];c[2]=a[0]*b[1]-a[1]*b[0];
  if(c[0]==0 && c[1]==0 && c[2]==0){free(work);vis_scene_release(s);return VIS_INVALID;}
 }
 uint32_t root=build_node(s,work,0,n);
 if(root!=0 || s->nodes!=s->node_capacity){free(work);vis_scene_release(s);return VIS_CAPACITY;}
 assert(root==0 && s->nodes==s->node_capacity);
 free(work); /* No build-only pointer survives the atomic publication. */
 vis_scene *old=*slot;*slot=s;vis_scene_release(old);return VIS_OK;
}
vis_status vis_binding_set(vis_binding *b,vis_scene *s,vis_generation expected){
 if(!b || !s)return VIS_INVALID;
 /* Reject new-scene overlap before reading any binding fields. A binding
  * must otherwise be initialized and contain a valid retained scene or NULL. */
 if(vis_scene_overlaps(s,b,sizeof(*b)))return VIS_INVALID;
 if(b->scene && vis_scene_overlaps(b->scene,b,sizeof(*b)))return VIS_INVALID;
 if(!equal(s->generation,expected))return VIS_GENERATION;
 vis_status r=vis_scene_retain(s);if(r!=VIS_OK)return r;vis_scene *old=b->scene;b->scene=s;b->generation=expected;vis_scene_release(old);return VIS_OK;
}
void vis_binding_clear(vis_binding *b){
 if(b){
  if(b->scene && vis_scene_overlaps(b->scene,b,sizeof(*b)))return;
  vis_scene_release(b->scene);memset(b,0,sizeof(*b));
 }
}
static int consume(uint64_t *used,uint64_t cap){if(*used>=cap)return 0;(*used)++;return 1;}
static int box_hit(const node *b,const double o[3],const double d[3],double lo,double hi){
 for(int k=0;k<3;k++){if(d[k]==0){if(o[k]<b->lo[k] || o[k]>b->hi[k])return 0;}else{
  double a=(b->lo[k]-o[k])/d[k],z=(b->hi[k]-o[k])/d[k];if(a>z){double tmp=a;a=z;z=tmp;}lo=fmax(lo,a);hi=fmin(hi,z);if(lo>hi)return 0;}}
 return 1;
}
/* Watertight projected edge tests, inclusive edges, two-sided. */
static void triangle_points(const vis_scene *s,const triangle *t,double p[3][3]){
 for(int j=0;j<3;j++)memcpy(p[j],s->xyz+3*(size_t)t->vertex[j],3*sizeof(double));
}
static int tri_hit(const vis_scene *s,const triangle *t,const double o[3],const double d[3],double lo,double hi){
 double p[3][3];triangle_points(s,t,p);
 int kz=0;for(int k=1;k<3;k++)if(fabs(d[k])>fabs(d[kz]))kz=k;
 int kx=(kz+1)%3,ky=(kx+1)%3;if(d[kz]<0){int tmp=kx;kx=ky;ky=tmp;}
 double sx=d[kx]/d[kz],sy=d[ky]/d[kz],sz=1/d[kz],x[3],y[3],z[3];
 for(int j=0;j<3;j++){double pz=p[j][kz]-o[kz];x[j]=p[j][kx]-o[kx]-sx*pz;y[j]=p[j][ky]-o[ky]-sy*pz;z[j]=pz*sz;}
 double u=x[2]*y[1]-y[2]*x[1],v=x[0]*y[2]-y[0]*x[2],w=x[1]*y[0]-y[1]*x[0];
 if((u<0 || v<0 || w<0) && (u>0 || v>0 || w>0))return 0;
 double det=u+v+w;if(det==0)return 0;
 double distance=(u*z[0]+v*z[1]+w*z[2])/det;return distance>lo && distance<hi;
}
vis_status vis_segment_occluded(const vis_binding *b,vis_generation expected,const double from[3],const double to[3],vis_budget *budget,int *out){
 if(!b || !b->scene || !from || !to || !budget || !out)return VIS_INVALID;
 if(vis_scene_overlaps(b->scene,budget,sizeof(*budget)) ||
    vis_scene_overlaps(b->scene,out,sizeof(*out)) ||
    spans_overlap(budget,sizeof(*budget),out,sizeof(*out)) ||
    spans_overlap(budget,sizeof(*budget),b,sizeof(*b)) ||
    spans_overlap(out,sizeof(*out),b,sizeof(*b)) ||
    spans_overlap(budget,sizeof(*budget),from,3*sizeof(double)) ||
    spans_overlap(budget,sizeof(*budget),to,3*sizeof(double)))return VIS_INVALID;
 if(!equal(b->generation,expected) || !equal(b->scene->generation,expected))return VIS_GENERATION;
 double d[3];for(int k=0;k<3;k++){if(!isfinite(from[k]) || !isfinite(to[k]) || fabs(from[k])>1e50 || fabs(to[k])>1e50)return VIS_INVALID;d[k]=to[k]-from[k];}
 double length=hypot(hypot(d[0],d[1]),d[2]);if(length<=2*VIS_SEGMENT_EPSILON)return VIS_INVALID;
 for(int k=0;k<3;k++)d[k]/=length;
 if(!consume(&budget->used.rays,budget->limit.rays))return VIS_BUDGET;
 uint32_t stack[32];size_t count=1;stack[0]=0;
 while(count){if(!consume(&budget->used.node_visits,budget->limit.node_visits))return VIS_BUDGET;
  const node *n=&b->scene->tree[stack[--count]];
  if(!box_hit(n,from,d,VIS_SEGMENT_EPSILON,length-VIS_SEGMENT_EPSILON))continue;
  if(n->count){for(uint32_t i=0;i<n->count;i++){
   if(!consume(&budget->used.triangle_tests,budget->limit.triangle_tests))return VIS_BUDGET;
   if(tri_hit(b->scene,&b->scene->tri[n->start+i],from,d,VIS_SEGMENT_EPSILON,length-VIS_SEGMENT_EPSILON)){*out=1;return VIS_OK;}
  }}else{if(count+2>sizeof(stack)/sizeof(stack[0]))return VIS_CAPACITY;stack[count++]=n->right;stack[count++]=n->left;}
 }*out=0;return VIS_OK;
}

/* Separate nearest primitive keeps the existing any-hit API behavior intact. */
static int tri_nearest(const vis_scene *s,const triangle *t,const double o[3],const double d[3],
                       double far,double *distance,double bary[3]){
 double p[3][3];triangle_points(s,t,p);
 int kz=0;for(int k=1;k<3;k++)if(fabs(d[k])>fabs(d[kz]))kz=k;
 int kx=(kz+1)%3,ky=(kx+1)%3;if(d[kz]<0){int tmp=kx;kx=ky;ky=tmp;}
 double sx=d[kx]/d[kz],sy=d[ky]/d[kz],sz=1/d[kz],x[3],y[3],z[3];
 for(int j=0;j<3;j++){double pz=p[j][kz]-o[kz];x[j]=p[j][kx]-o[kx]-sx*pz;y[j]=p[j][ky]-o[ky]-sy*pz;z[j]=pz*sz;}
 double u=x[2]*y[1]-y[2]*x[1],v=x[0]*y[2]-y[0]*x[2],w=x[1]*y[0]-y[1]*x[0];
 if((u<0 || v<0 || w<0) && (u>0 || v>0 || w>0))return 0;
 double det=u+v+w;if(det==0)return 0;
 double distance_value=(u*z[0]+v*z[1]+w*z[2])/det;
 if(!isfinite(distance_value) || distance_value<=VIS_SEGMENT_EPSILON || distance_value>far)return 0;
 *distance=distance_value;bary[0]=u/det;bary[1]=v/det;bary[2]=w/det;return 1;
}
vis_status vis_ray_nearest(const vis_binding *b,vis_generation expected,
 const double origin[3],const double direction[3],vis_budget *budget,vis_hit *out){
 if(!b || !b->scene || !origin || !direction || !budget || !out)return VIS_INVALID;
 if(vis_scene_overlaps(b->scene,budget,sizeof(*budget)) ||
    vis_scene_overlaps(b->scene,out,sizeof(*out)) ||
    spans_overlap(budget,sizeof(*budget),out,sizeof(*out)) ||
    spans_overlap(budget,sizeof(*budget),b,sizeof(*b)) ||
    spans_overlap(out,sizeof(*out),b,sizeof(*b)) ||
    spans_overlap(budget,sizeof(*budget),origin,3*sizeof(double)) ||
    spans_overlap(budget,sizeof(*budget),direction,3*sizeof(double)))return VIS_INVALID;
 if(!equal(b->generation,expected) || !equal(b->scene->generation,expected))return VIS_GENERATION;
 for(int k=0;k<3;k++)if(!isfinite(origin[k]) || fabs(origin[k])>1e50 || !isfinite(direction[k]))return VIS_INVALID;
 double length=hypot(hypot(direction[0],direction[1]),direction[2]);
 if(!isfinite(length) || fabs(length-1)>1e-12)return VIS_INVALID;
 /* Normalize accepted roundoff so reported parameter is a world-space distance. */
 double d[3];for(int k=0;k<3;k++)d[k]=direction[k]/length;
 const node *root=&b->scene->tree[0];double extent[3];
 for(int k=0;k<3;k++)extent[k]=fmax(fabs(root->lo[k]-origin[k]),fabs(root->hi[k]-origin[k]));
 /* Every root-contained point is no farther than this farthest-corner sphere.
  * Generous floating rounding margin, not an arbitrary world distance cutoff. */
 double far=nextafter(hypot(hypot(extent[0],extent[1]),extent[2])*(1+1e-12),INFINITY);
 if(!isfinite(far))return VIS_INVALID;
 if(!consume(&budget->used.rays,budget->limit.rays))return VIS_BUDGET;
 vis_hit result;memset(&result,0,sizeof(result));
 uint32_t stack[32];size_t count=1;stack[0]=0;
 while(count){
  if(!consume(&budget->used.node_visits,budget->limit.node_visits))return VIS_BUDGET;
  const node *n=&b->scene->tree[stack[--count]];
  if(!box_hit(n,origin,d,VIS_SEGMENT_EPSILON,far))continue;
  if(n->count){for(uint32_t i=0;i<n->count;i++){
   if(!consume(&budget->used.triangle_tests,budget->limit.triangle_tests))return VIS_BUDGET;
   const triangle *t=&b->scene->tri[n->start+i];double distance,bary[3];
   if(!tri_nearest(b->scene,t,origin,d,far,&distance,bary))continue;
   if(result.found && (distance>result.distance || (distance==result.distance && t->ordinal>=result.triangle_ordinal)))continue;
   double p[3][3];triangle_points(b->scene,t,p);
   double a[3],v[3],normal[3];for(int k=0;k<3;k++){a[k]=p[1][k]-p[0][k];v[k]=p[2][k]-p[0][k];}
   normal[0]=a[1]*v[2]-a[2]*v[1];normal[1]=a[2]*v[0]-a[0]*v[2];normal[2]=a[0]*v[1]-a[1]*v[0];
   double nl=hypot(hypot(normal[0],normal[1]),normal[2]);
   if(!isfinite(nl) || nl==0)return VIS_INVALID;
   result.found=1;result.triangle_ordinal=t->ordinal;result.distance=distance;
   for(int k=0;k<3;k++){result.bary[k]=bary[k];result.position[k]=origin[k]+distance*d[k];result.geometric_normal[k]=normal[k]/nl;}
   /* Outward margin keeps equal-distance candidates from slab roundoff pruning. */
   far=nextafter(distance*(1+1e-12),INFINITY);
  }}else{if(count+2>sizeof(stack)/sizeof(stack[0]))return VIS_CAPACITY;stack[count++]=n->right;stack[count++]=n->left;}
 }
 *out=result;return VIS_OK;
}
