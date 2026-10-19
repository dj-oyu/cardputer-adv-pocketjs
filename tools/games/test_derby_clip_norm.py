"""Host-only exact ou() decision and repeated-sqrt work regression.

Extracts the production predicate; preserves its previous expression as oracle.
Counts calls in an oval frame with 18 view chords and five all-visible series.
This is an operation-count bound/fixture, not a measured device hotspot.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
src = (root / 'main/pocket/pocket_derby.c').read_text()
body = re.search(r'static bool ou\(.*?\n}', src, re.S).group()
assert 'k=m*view.clip_norm,' in body
assert 'view.clip_norm=sqrt(pc[4]*pc[4]+25600);' in src
body=body.split('#else')[0].replace('#ifdef POCKET_DERBY_CLIP_NORM\n','')+'\n'+body.split('#endif\n',1)[1]
old = re.sub(r'    /\* Camera getters[\s\S]*?    if\(f!=view.clip_f\)\{[^\n]*\}\n', '', body)
old = old.replace('static bool ou(', 'static bool old_ou(').replace(
    'k=m*view.clip_norm,', 'k=m*counted_sqrt(f*f+25600),')
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
static struct {double ve[128];} data, *course=&data;
static struct {double pc[6],clip_norm,clip_f;} view;
static unsigned calls;
static double counted_sqrt(double x){++calls;return sqrt(x);}
''' + old + '\n' + body + r'''
static uint64_t state=0x7195;
static double rnd(void){state=state*6364136223846793005ULL+1;return ((double)(state>>11)/9007199254740992.0-.5)*4000;}
int main(void){
    const double edges[]={0.,-0.,1.,-1.,160.,1e154,INFINITY,-INFINITY,NAN};
    unsigned cases=0;
    for(unsigned t=0;t<100000;t++){
        for(unsigned i=0;i<128;i++)data.ve[i]=rnd();
        view.pc[4]=t<sizeof edges/sizeof *edges?edges[t]:rnd();
        view.clip_f=view.pc[4];view.clip_norm=sqrt(view.pc[4]*view.pc[4]+25600);
        double w=rnd(),m=rnd(),zf=rnd();
        for(unsigned j=0;j<18;j++){
            assert(old_ou(j,w,m,zf)==ou(j,w,m,zf));++cases;
        }
    }
    calls=0;view.pc[4]=160;
    for(unsigned phase=0;phase<6;phase++)for(unsigned j=0;j<18;j++)old_ou(j,25.5,30,1000);
    assert(calls==108);
    calls=0;view.clip_f=view.pc[4];view.clip_norm=counted_sqrt(view.pc[4]*view.pc[4]+25600);
    for(unsigned phase=0;phase<6;phase++)for(unsigned j=0;j<18;j++)ou(j,25.5,30,1000);
    assert(calls==1);
    /* Reentrant getter/hook changed pc[4] before a nested ser(). */
    view.pc[4]=20;
    for(unsigned j=0;j<18;j++)assert(old_ou(j,25.5,30,1000)==ou(j,25.5,30,1000));
    printf("ou decisions: %u equal; synthetic oval frame sqrt calls 108 -> 1; no device timing claim\n",cases);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp)/'clip.c'
    c.write_text(program)
    binary = Path(tmp)/'clip'
    subprocess.run(['cc','-std=c11','-O2','-ffp-contract=off','-fsanitize=undefined','-I'+str(root/'main/pocket'),str(c),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
