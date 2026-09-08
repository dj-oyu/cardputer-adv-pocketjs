// Host-only: what each zoom rung costs, per species, when it is THE shot.
//
// One cut per flower means the rung is chosen once and paid for the whole 40 s
// a species is up, so the question stopped being "what does the ladder cost on
// average" and became "what does this rung cost on this plant". This measures
// the two quantities that answer it:
//
//   COVERED  pixels the plant occupies, counted against the same garden frame
//            the renderer drew over. This is the causal quantity -- every
//            covered pixel pays shade(), and on bell species bell() as well --
//            and it is exact and deterministic, so it is the number to compare
//            rungs by.
//   HOST US  microseconds per flower_draw on this machine. A PROXY ONLY. It is
//            x86 with a cache the S3 does not have and no PIE kernels; use it
//            for the SHAPE of the curve across rungs, never as a frame time.
//            Device fps has to come off the board.
//
//   bash tools/flower_shot_cost.sh
#include "../main/scene/scene_mem.c"
#include "../main/scene/garden.c"
#ifndef FSC_FLOWER_SRC
#define FSC_FLOWER_SRC "flower.c"
#endif
#include FSC_FLOWER_SRC
#include "../main/scene/flower_species.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "flower_catalog.h"

static uint16_t full[W*H],background[W*H];

static void background_frame(void) {
    GardenFrame f={0};
    if(seed_map)f=*(const GardenFrame*)(seed_map+32*32);
    else garden_prepare(&f,elapsed);
    for(int y=0;y<H;y++)garden_row(background+y*W,y,&f);
}

#define REPS 24

// The scan visits every part's screen bounding box, so the sum of those box
// areas IS the traversal work -- the same quantity a visit counter reports,
// reconstructed from public state instead of instrumenting the renderer. It is
// the cost that FALLS when a tighter framing pushes parts out of the window,
// which is why it can move in the opposite direction to covered area.
static void boxes(unsigned long *visits,unsigned long *pale,unsigned *outside) {
    *visits=*pale=0;*outside=0;
    for(unsigned i=0;i<count;i++) {
        const Petal *p=&petals[i];
        long w=(long)p->xmax-p->xmin+1,h=(long)p->ymax-p->ymin+1;
        if(w<0)w=0;
        if(h<0)h=0;
        unsigned long a=(unsigned long)(w*h);
        *visits+=a;
        if(p->shape)*pale+=a;
        // Clamped against the window, so a part whose box was trimmed had
        // something outside it. This is the "cropped or framed" question in
        // its countable half; the other half is a look question.
        if(180+cam_s*(p->c.x-p->ex-cam_x)<X0||180+cam_s*(p->c.x+p->ex-cam_x)>W-1||
           65-cam_s*(p->c.y+p->ey-cam_y)<12||65-cam_s*(p->c.y-p->ey-cam_y)>H-1)
            (*outside)++;
    }
}

int main(void) {
    setvbuf(stdout,NULL,_IOLBF,0);
    for(int pass=0;pass<2;pass++) {
        printf("%s\n",pass?
            "COVERED -- pixels the plant paints; the shading work":
            "VISITS -- sum of part box areas; the traversal work"
            " (pale% = bell parts, out = parts crossing the window edge)");
        printf("%-12s","");
        for(int v=0;v<FLOWER_VIEWS;v++)
            if(FLOWER_SHOTS[v].zoom<=0)printf("       fit/--- ");
            else printf("   %4.2f/%4.2f ",(double)FLOWER_SHOTS[v].zoom,
                        (double)FLOWER_SHOTS[v].aim);
        printf("\n");
        unsigned long tot[8]={0};
        for(int s=0;s<FLOWER_SPECIES_COUNT;s++) {
            printf("%-12s",flower_names[s]);
            for(int v=0;v<FLOWER_VIEWS;v++) {
                elapsed=4;
                bloom_view=v;
                bloom_species=(flower_species_t)s;
                bloom_elapsed=FLOWER_SHOTS[v].hold*0.5f;
                flower_prepare_rotating(0.0f,0,0);
                assert(flower_fade()>0.999f);
                unsigned long visits,pale;unsigned outside;
                boxes(&visits,&pale,&outside);
                if(pass) {
                    flower_draw(full,0,H);
                    background_frame();
                    unsigned long covered=0;
                    for(int k=0;k<W*H;k++)covered+=full[k]!=background[k];
                    printf("  %11lu ",covered);
                    tot[v]+=covered;
                } else {
                    printf("  %6lu %2lu%% %2u/%-2u",visits,
                           visits?pale*100/visits:0,outside,count);
                    tot[v]+=visits;
                }
            }
            printf("\n");
        }
        printf("%-12s","mean");
        for(int v=0;v<FLOWER_VIEWS;v++)
            printf("  %11lu ",tot[v]/FLOWER_SPECIES_COUNT);
        printf("\n");
    }
    return 0;
}
