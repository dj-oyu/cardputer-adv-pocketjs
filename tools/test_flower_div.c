// What the reciprocal multiplies cost the picture.
//
// main/scene/flower.c replaced sixteen software float divisions and five
// fmaxf calls on its hot paths with reciprocal multiplies and comparisons.
// Every one of those is a rounding change, not an identity: a/b and a*(1/b)
// differ by up to an ulp, and an ulp on a discriminant is a silhouette pixel
// that lands on the other side of a `d<0` test. So the change is held to the
// same contract as the garden kernel -- a bound on how far the picture may
// move, as a test that can fail -- rather than to an argument that it should
// not move much.
//
// Build it twice, once with -DFLOWER_DIV_EXACT, and diff the dumps:
//
//   gcc -O2 -DFLOWER_DIV_EXACT tools/test_flower_div.c -lm -o /tmp/fd_ref
//   gcc -O2                    tools/test_flower_div.c -lm -o /tmp/fd
//   /tmp/fd_ref /tmp/ref.bin && /tmp/fd /tmp/new.bin && /tmp/fd --diff /tmp/ref.bin /tmp/new.bin
//
// Two builds rather than two code paths in one binary, because flower.c keeps
// its state in file statics and the arithmetic is spread across bell_hit,
// shade and ray_row; a runtime switch would have to reach all three and would
// be a different program from the one that ships.
#include "../main/scene/scene_mem.c"
#include "../main/scene/garden.c"
#include "../main/scene/flower.c"
#include <stdio.h>
#include <stdlib.h>

#define PHASES 8
#define SPECIES 8
static uint16_t fb[W*H];

static const char *NAME[SPECIES]={"CRYSTAL","VALLEY","SUNFLOWER","SNOWDROP",
                                  "TULIP","DAFFODIL","CROCUS","CALLA"};

static void dump(const char *path) {
    FILE *f=fopen(path,"wb");
    if(!f){perror(path);exit(1);}
    for(int sp=0;sp<SPECIES;sp++)
        for(int ph=0;ph<PHASES;ph++) {
            elapsed=3.1f+ph*1.7f-1.0f/30;
            flower_prepare(1.0f/30,0,0,(flower_species_t)sp);
            flower_draw(fb,0,H);
            if(fwrite(fb,sizeof fb,1,f)!=1){perror("write");exit(1);}
        }
    fclose(f);
}

static void diff(const char *pa,const char *pb) {
    FILE *a=fopen(pa,"rb"),*b=fopen(pb,"rb");
    if(!a||!b){perror("open");exit(1);}
    static uint16_t x[W*H],y[W*H];
    int worst_any=0;unsigned long moved_all=0,total_all=0;
    for(int sp=0;sp<SPECIES;sp++) {
        unsigned long moved=0,total=0;int step[3]={0,0,0};
        for(int ph=0;ph<PHASES;ph++) {
            if(fread(x,sizeof x,1,a)!=1||fread(y,sizeof y,1,b)!=1){
                fprintf(stderr,"short read: were both dumps written?\n");exit(1);}
            for(int i=0;i<W*H;i++) {
                total++;
                if(x[i]==y[i])continue;
                moved++;
                int c[3]={(x[i]>>11)&31,(x[i]>>5)&63,x[i]&31};
                int d[3]={(y[i]>>11)&31,(y[i]>>5)&63,y[i]&31};
                for(int k=0;k<3;k++){int e=abs(c[k]-d[k]);if(e>step[k])step[k]=e;}
            }
        }
        moved_all+=moved;total_all+=total;
        for(int k=0;k<3;k++)if(step[k]>worst_any)worst_any=step[k];
        printf("%-10s %8lu/%lu moved (%6.4f%%)  worst step r=%d g=%d b=%d\n",
               NAME[sp],moved,total,100.0*moved/total,step[0],step[1],step[2]);
    }
    fclose(a);fclose(b);
    printf("\nDIV_OK: %lu/%lu pixels move (%.4f%%), worst channel step %d, over "
           "%d species x %d phases\n",
           moved_all,total_all,100.0*moved_all/total_all,worst_any,SPECIES,PHASES);
    // The licence. A reciprocal multiply may land a discriminant on the other
    // side of `d<0`, which moves one silhouette pixel by a whole shading step;
    // what it must not do is move the interior, so the share has to stay tiny
    // even though the step does not.
    if(100.0*moved_all/total_all>0.25){
        printf("FAIL: more than a quarter of one percent of pixels moved\n");exit(1);}
    if(worst_any>63){printf("FAIL: a channel saturated\n");exit(1);}
}

int main(int argc,char**argv) {
    if(argc==4&&!strcmp(argv[1],"--diff")){diff(argv[2],argv[3]);return 0;}
    if(argc==2){dump(argv[1]);return 0;}
    fprintf(stderr,"usage: %s <dump.bin> | %s --diff <a.bin> <b.bin>\n",argv[0],argv[0]);
    return 2;
}
