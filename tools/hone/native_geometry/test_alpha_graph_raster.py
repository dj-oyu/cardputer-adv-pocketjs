import os,pathlib,subprocess,tempfile,unittest


class TestAlphaGraphRaster(unittest.TestCase):
    def test_preflight_parity_overflow_and_alias(self):
        here=pathlib.Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp);(p/'test.c').write_text('''#include "alpha_graph_raster_candidate.h"
#include "../source_package/alpha_raster_candidate.h"
#include <assert.h>
int main(void){
 const hone_alpha_instruction a[]={{HONE_ALPHA_CONST,0,0,.5f}},r[]={{HONE_ALPHA_CONST,0,0,.2f}},g[]={{HONE_ALPHA_CONST,0,0,.4f}},b[]={{HONE_ALPHA_CONST,0,0,.6f}};
 const hone_alpha_program alpha={a,1,0},colors[3]={{r,1,0},{g,1,0},{b,1,0}};
 hone_graph_packet p[5];for(unsigned i=0;i<5;i++)p[i]=(hone_graph_packet){{{{0,0,1,{0,0,0}},{4,0,1,{1,0,0}},{0,4,1,{0,1,0}}},&alpha,colors},i};
 int owner=1;hone_graph_snapshot s={p,1,5,NULL,0,&owner,1,1};
 hone_alpha_packet old={.v={{0,0,1,0},{4,0,1,0},{0,4,1,0}},.rgb={.2f,.4f,.6f},.alpha=.5f,.order=0};
 hone_alpha_snapshot reference={.packets=&old,.count=1,.capacity=1,.owner=&owner,.generation=1,.ready=1};
 uint16_t graph[16],normal[16],band[16],saved[16];float depth[16],scratch[1024];
 for(unsigned i=0;i<16;i++){graph[i]=normal[i]=band[i]=0x39e7;depth[i]=0;}
 assert(hone_alpha_render(&reference,&owner,1,normal,depth,4,4,1,4));
 assert(hone_graph_render(&s,&owner,1,graph,depth,4,4,1,4,scratch,1024));
 assert(hone_graph_render(&s,&owner,1,band,depth,4,4,4,4,scratch,1024));
 assert(!memcmp(graph,normal,sizeof graph)&&!memcmp(band,normal,sizeof band));
 const hone_alpha_instruction xcode[]={{HONE_ALPHA_X,0,0,0}};const hone_alpha_program xalpha={xcode,1,0};
 p[0].triangle.alpha=&xalpha;
 for(unsigned i=0;i<16;i++)graph[i]=band[i]=0x39e7;
 assert(hone_graph_render(&s,&owner,1,graph,depth,4,4,1,4,scratch,1024));
 assert(hone_graph_render(&s,&owner,1,band,depth,4,4,4,4,scratch,1024));
 assert(!memcmp(graph,band,sizeof graph));assert(graph[0]!=graph[1]);
 memcpy(saved,graph,sizeof saved);s.count=5;
 assert(!hone_graph_render(&s,&owner,1,graph,depth,4,4,1,4,scratch,1024));assert(!memcmp(saved,graph,sizeof graph));
 s.count=1;const hone_alpha_instruction dyn[]={{HONE_ALPHA_DRIVER,0,0,0}};const hone_alpha_program dynamic={dyn,1,0};
 p[0].triangle.alpha=&dynamic;double value=NAN;s.drivers=&value;s.driver_count=1;
 assert(!hone_graph_render(&s,&owner,1,graph,depth,4,4,1,4,scratch,1024));assert(!memcmp(saved,graph,sizeof graph));
 value=.5;union {float f[16];uint16_t p[32];} alias;memset(&alias,0,sizeof alias);
 assert(!hone_graph_render(&s,&owner,1,alias.p,depth,4,4,1,4,alias.f,16));
 return 0;}
''',encoding='utf8')
            cmd=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(here),str(p/'test.c'),'-lm','-o',str(p/'test')]
            if os.getenv('HONE_SANITIZE')=='1':cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(cmd,check=True);subprocess.run([str(p/'test')],check=True)


if __name__=='__main__':unittest.main()
