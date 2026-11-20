"""Synthetic Hone callback through the real Model/snapshot/raster contract.

Reuses the existing provider test's independent all-at-once geometry oracle.
No private asset, firmware build, device access or production-source changes.
"""
import argparse
import os
from pathlib import Path
import subprocess


ADAPTER = r'''
#include "geometry.h"
#include "native_gather.h"
#include "native_provider.h"
/* Test-only source layout: source vertex order is reversed, every prepared
 * corner is separate, and original loop IDs have a nonzero sparse prefix. */
static bool hone_provide_geometry(unsigned frame,unsigned node,unsigned count,
                                  bool invalid,ayatsuri_skin_result *slot){
 ayatsuri_skin_result reference[32],saved[32];
 geometry(frame,node,reference);memcpy(saved,slot,sizeof saved);
 float positions[96],normals[288];
 uint32_t source[96],corners[96],faces[96],offsets[33],loops[96],part[32];
 for(unsigned f=0;f<32;f++){
  offsets[f]=f*3;part[f]=3*(31-f);
  for(unsigned k=0;k<3;k++)positions[3*(31-f)+k]=reference[f].position[k];
  for(unsigned j=0;j<3;j++){
   unsigned c=3*f+j;source[c]=(f+j)%32;corners[c]=100+c;faces[c]=f;loops[c]=100+c;
   normals[3*c]=0;normals[3*c+1]=0;normals[3*c+2]=1;
  }
 }
 offsets[32]=96;
 hone_input_mesh mesh={.vertices=96,.source_vertex=source,.source_corner=corners,
  .corner_polygon=faces,.source_vertex_count=32,.polygon_count=32,
  .polygon_corner_count=96,.source_corner_count=196,
  .polygon_offsets=offsets,.polygon_corners=loops};
 hone_provider_mesh input={&mesh,positions,normals};
 hone_provider_part nodes[COUNT+1]={0};
 for(unsigned id=1;id<=COUNT;id++)nodes[id]=(hone_provider_part){0,32,part};
 /* Node zero is a non-mesh hole: callback receives actual model IDs 1..23. */
 hone_provider_context ctx={&input,1,nodes,COUNT+1};
 if(invalid)normals[3*part[31]+2]=0;
 bool ok=hone_native_provider(&ctx,node,count,slot);
 if(invalid){assert(!ok&&!memcmp(saved,slot,sizeof saved));return false;}
 assert(ok&&!memcmp(reference,slot,sizeof reference));
 return true;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--opt', choices=['0', '2'], default='2')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[2]
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (root/'tools/ayatsuri/provider/test_provider.c').read_text()
    anchor = '\nstruct Provider {unsigned frame,calls;'
    old = ''' geometry(p->frame,node,slot);
 if(-(int)node==p->fail_node)slot[31].normal[2]=0;
 return true;'''
    if source.count(anchor) != 1 or source.count(old) != 1:
        raise RuntimeError('provider fixture changed: review adapter anchors')
    source = source.replace(anchor, '\n'+ADAPTER+anchor).replace(
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
