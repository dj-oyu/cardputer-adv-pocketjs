"""Synthetic Hone callback through the real Model/snapshot/raster contract.

Full source morph/DQ and polygon normals precede the real provider.
Analytic planar oracle; no general-rig or Blender parity claim.
No private asset, firmware build, device access or production-source changes.
"""
import argparse
import os
from pathlib import Path
import subprocess


ADAPTER = r'''
#include "geometry.h"
#include "native_morph.h"
#include "native_dq.h"
#include "native_deform.h"
#include "native_gather.h"
#include "native_provider.h"
#include "../source_package/rigid_dq_candidate.c"
#include "../source_package/native_polygon_normals.c"
/* Analytic planar fixture: independent oracle uses the same binary32 rest
 * samples, then explicit z morph, x reflection and rigid translation. */
static void geometry(unsigned frame,unsigned node,ayatsuri_skin_result *out){
 for(unsigned v=0;v<32;v++){
  double a=v*2*3.141592653589793/32;
  float x=(float)(8*cos(a)),y=(float)(8*sin(a));
  float z=(float)(frame%8)/8,dx=(float)(frame%5)/4,dz=(float)(node%3)/2;
  if(frame==UINT_MAX)x+=-2*x;
  out[v]=(ayatsuri_skin_result){{x+dx,y,z+dz},{0,0,frame==UINT_MAX?-1:1}};
 }
}
static bool hone_provide_geometry(unsigned frame,unsigned node,unsigned count,
                                  bool invalid,ayatsuri_skin_result *slot){
 ayatsuri_skin_result reference[32],saved[32];
 geometry(frame,node,reference);memcpy(saved,slot,sizeof saved);
 float rest[99],prepared[270],keys[810],positions[99],normals[270],masks[99];
 uint32_t source[90],corners[90],faces[90],offsets[31],part[32],rep[33],relative[3]={0,0,0};
 uint8_t smooth[30];hone_input_range ranges[90];hone_input_weight weights[90];
 for(unsigned v=0;v<32;v++){
  double a=v*2*3.141592653589793/32;
  rest[3*v]=(float)(8*cos(a));rest[3*v+1]=(float)(8*sin(a));rest[3*v+2]=0;rep[v]=UINT32_MAX;
 }
 rest[96]=9;rest[97]=8;rest[98]=7;rep[32]=UINT32_MAX;
 for(unsigned f=0;f<30;f++){
  offsets[f]=3*f;smooth[f]=1;
  for(unsigned j=0;j<3;j++){
   unsigned c=3*f+j,v=j?f+j:0;source[c]=v;corners[c]=c;faces[c]=f;
   if(rep[v]==UINT32_MAX)rep[v]=c;
   ranges[c]=(hone_input_range){c,1};weights[c]=(hone_input_weight){0,1};
   for(unsigned k=0;k<3;k++){
    prepared[3*c+k]=rest[3*v+k];
    for(unsigned key=0;key<3;key++)keys[270*key+3*c+k]=rest[3*v+k];
   }
   keys[270+3*c+2]=1;keys[540+3*c]=-rest[3*v];
  }
 }
 offsets[30]=90;for(unsigned v=0;v<32;v++)part[v]=rep[v];
 for(unsigned k=0;k<99;k++)masks[k]=1;
 hone_input_mesh mesh={.vertices=90,.morphs=3,.groups=1,.relative_morph=1,
  .position=prepared,.source_vertex=source,.source_corner=corners,.corner_polygon=faces,
  .source_vertex_count=33,.source_position=rest,.source_representative=rep,
  .polygon_count=30,.polygon_corner_count=90,.source_corner_count=90,
  .polygon_offsets=offsets,.polygon_corners=corners,.polygon_vertices=source,.polygon_smooth=smooth,
  .morph_positions=keys,.morph_relative=relative,.weight_ranges=ranges,.weights=weights};
 float values[3]={0,(float)(frame%8)/8,frame==UINT_MAX?1:0};
 uint32_t map[1]={0};double joints[7]={1,0,0,0,(double)(frame%5)/4,0,(double)(node%3)/2};
 double identity[12]={1,0,0,0,0,1,0,0,0,0,1,0},work[99];
 /* Three independent frame-failure sites. Deform scratch may be partial;
  * no pre-gather stage is permitted to write the producer slot. */
 if(invalid && node%3==0)masks[31*3+1]=NAN;
 bool ok=hone_deform_source(&mesh,values,masks,3,1,map,1,joints,1,identity,identity,positions);
 if(ok){
  assert(positions[96]==9&&positions[97]==8&&positions[98]==7);
  if(invalid && node%3==1)offsets[30]=89;
  ok=polygon_normals(positions,33,offsets,source,smooth,30,90,work,normals);
 }
 if(ok){
  /* Component tolerance 1e-6, unit tolerance 1e-6; orientation is signed.
   * Planar analytic fixture is not a general Blender normal parity claim. */
  for(unsigned c=0;c<90;c++){
   assert(fabsf(normals[3*c])<=1e-6f&&fabsf(normals[3*c+1])<=1e-6f);
   assert(fabsf(normals[3*c+2]-(frame==UINT_MAX?-1:1))<=1e-6f);
  }
  hone_provider_mesh input={&mesh,positions,normals};hone_provider_part nodes[COUNT+1]={0};
  for(unsigned id=1;id<=COUNT;id++)nodes[id]=(hone_provider_part){0,32,part};
  hone_provider_context ctx={&input,1,nodes,COUNT+1};
  if(invalid && node%3==2)normals[3*part[31]+2]=0;
  ok=hone_native_provider(&ctx,node,count,slot);
 }
 if(invalid){assert(!ok&&!memcmp(saved,slot,sizeof saved));return false;}
 assert(ok);
 for(unsigned v=0;v<32;v++)for(unsigned k=0;k<3;k++){
  assert(fabsf(slot[v].position[k]-reference[v].position[k])<=1e-6f);
  assert(fabsf(slot[v].normal[k]-reference[v].normal[k])<=1e-6f);
 }
 return true;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--opt', choices=['0', '2'], default='2')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--normals', choices=['exact', 'wedge'], default='exact',
                        help='independent normal candidate; does not change runtime admission')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[2]
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (root/'tools/ayatsuri/provider/test_provider.c').read_text()
    start = source.index('static void geometry(')
    end = source.index('\nstruct Provider', start)
    source = source[:start] + source[end:]
    anchor = '\nstruct Provider {unsigned frame,calls;'
    old = ''' geometry(p->frame,node,slot);
 if(-(int)node==p->fail_node)slot[31].normal[2]=0;
 return true;'''
    if source.count(anchor) != 1 or source.count(old) != 1:
        raise RuntimeError('provider fixture changed: review adapter anchors')
    adapter = ADAPTER
    if args.normals == 'wedge':
        adapter = adapter.replace('native_polygon_normals.c', 'native_wedge_lut_candidate.c')
    source = source.replace(anchor, '\n'+adapter+anchor).replace(
        old, ' return hone_provide_geometry(p->frame,node,count,-(int)node==p->fail_node,slot);')
    # These inherited negative tests deliberately pass incompatible addresses
    # for pre-entry rejection, never dereference them as a skin-result array.
    source = source.replace('static void entry_rejections(',
        '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wstrict-aliasing"\nstatic void entry_rejections(', 1)
    source = source.replace('static void unsupported_metadata(',
        '#pragma GCC diagnostic pop\nstatic void unsupported_metadata(', 1)
    # Interpose only allocations in compiled test/production translation units.
    # Linker wrapping on MinGW also captures CRT filename conversion buffers,
    # whose runtime-internal frees do not pass through the wrapper.
    redirect = out/'allocation_redirect.h'
    redirect.write_text("""#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
void *__wrap_malloc(size_t);void *__wrap_calloc(size_t,size_t);void __wrap_free(void*);
#define malloc __wrap_malloc
#define calloc __wrap_calloc
#define free __wrap_free
""")
    allocator = 'void *__real_malloc(size_t);void *__real_calloc(size_t,size_t);void __real_free(void*);'
    if source.count(allocator) != 1:
        raise RuntimeError('provider allocation hooks changed')
    source = source.replace(allocator, """#undef malloc
#undef calloc
#undef free
void *__real_malloc(size_t n){return malloc(n);}
void *__real_calloc(size_t n,size_t s){return calloc(n,s);}
void __real_free(void *p){free(p);}
""")
    source = source.replace('static const float cam[9]',
        '#define malloc __wrap_malloc\n#define calloc __wrap_calloc\n#define free __wrap_free\nstatic const float cam[9]', 1)
    if os.name == 'nt':
        # Winpthreads does not provide the POSIX custom-stack painter. Run the
        # same transaction checks synchronously; report no stack high-water.
        start = source.index('int main(int argc,char**argv)')
        source = source[:start] + '''int main(int argc,char**argv){
 assert(argc==2);run(argv[1]);puts("stack_highwater=not_measured_windows");return 0;}
'''
    (out/'test_provider.c').write_text(source)
    # Descriptor declaration comes from the actual exporter, with public-only
    # synthetic input. The adapter above supplies its separate indexed views.
    from native_geometry import emit
    from test_consumer_plan import fixture
    (out/'geometry.h').write_text(emit(fixture()))
    subprocess.run(['node', str(root/'tools/ayatsuri/provider/fixture.cjs'),
                    str(out/'synthetic.aynm')], check=True)
    subprocess.run(['node', str(root/'tools/ayatsuri/skin/render_fixture.cjs'),
                    str(out)], check=True)
    sources = ['main/ayatsuri/ayatsuri_model.c', 'main/ayatsuri/ayatsuri_visibility.c',
               'main/ayatsuri/compact_opaque.c', 'main/ui/kasane/ksn_shiki_frame.c',
               'main/ui/kasane/ksn_mesh_depth.c']
    binary = out/('provider.exe' if os.name == 'nt' else 'provider')
    command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-O'+args.opt,
               '-g', '-Wall', '-Wextra', '-Werror', '-fstrict-aliasing',
               '-Wstrict-aliasing=2', '-ffp-contract=off', '-fno-fast-math',
               '-DAYATSURI_MODEL_EXPERIMENTAL_PROVIDER', '-I'+str(root/'main'),
               '-I'+str(root/'tools/hostshim'), '-I'+str(here), '-include', str(redirect),
               *[str(root/s) for s in sources], str(out/'test_provider.c'),
               '-lm', '-pthread',
               '-o', str(binary)]
    if args.sanitize:
        command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    (out/'command.txt').write_text('\n'.join(command)+'\n')
    subprocess.run(command, check=True)
    env = dict(os.environ)
    if args.sanitize:
        env['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
    subprocess.run([str(binary), (out/'synthetic.aynm').as_posix()], check=True, env=env)


if __name__ == '__main__':
    main()
