// How stale may a traced flower be before anyone can see it?
//
// ANSWERED, 2026-09-08: not at all, and the caching line is closed. The table
// this prints shows channel steps of 23-48 out of 31/63/31 at k=1 -- a single
// frame of reuse -- because what moves between frames is the specular
// highlight, not the surface under it. There is no interval at which that is
// invisible, so there was never a cadence to choose, and no reprojection can
// help because a warp moves pixels and this moves their colour. ray_row traces
// every frame. Kept because the measurement is the reason, and the next person
// should be able to read it rather than re-derive it.
//
// ray_row is 30-40 ms on the device, which is more than everything else in the
// frame put together, so the obvious move is to stop tracing it every frame:
// keep the traced pixels, reuse them for a few frames, retrace. The question
// that decides whether that works is not "how fast" but "how far does the
// flower move in a frame", and until tilt was cut out of flower_prepare that
// question had no answer -- the orientation depended on the accelerometer, so
// two runs of the same code drew different pictures. It is now a pure function
// of `elapsed`, which is what makes the comparison below possible at all.
//
// What it measures, for a reuse of k frames:
//
//   drift    the largest distance, in screen pixels, that any part's centre
//            moves between the traced frame and the frame it is reused on.
//            This is the geometry, independent of shading, and it is what a
//            reprojection warp could in principle correct.
//   edge     the largest movement of any part's integer bounding box. Breath
//            and cup change radii rather than positions, so a part can deform
//            without its centre moving at all; reported separately because it
//            is quantised to whole pixels and would otherwise swamp `drift`.
//   moved    the share of pixels that differ from a frame traced properly.
//   step     the largest per-channel difference, in RGB565 units, over all of
//            them. A large `moved` with a step of 1 is a different thing from
//            a small `moved` with a step of 20; the first is invisible and the
//            second is an edge in the wrong place.
//
// The cache being modelled holds the flower's own colour plus coverage, and
// composites over the *current* garden -- not the dissolved result, because the
// woodland behind the flower moves every frame and a frozen backdrop would drag
// it along. Coverage is recovered here by rendering the same moment with and
// without the flower and taking the pixels that differ, which needs no change
// to flower.c. Its one blind spot is a flower pixel that happens to equal the
// garden pixel behind it: it is called "not covered" and takes the current
// garden instead, which is the same colour to within the reason it was missed.
#include "../main/scene/scene_mem.c"
#include "../main/scene/garden.c"
#include "../main/scene/flower.c"
#include <stdio.h>
#include <math.h>

static uint16_t now_full[W*H],now_bg[W*H],old_full[W*H],old_bg[W*H],reused[W*H];
static float cx0[MAX_PARTS],cy0[MAX_PARTS];
static int bx0[MAX_PARTS],bX0[MAX_PARTS],by0[MAX_PARTS],bY0[MAX_PARTS];

// flower_prepare advances `elapsed` by dt, so seeding it one step short lands
// the frame exactly on T. Tilt is not passed: it no longer reaches anything.
static void at(float T,float dt,flower_species_t sp) {
    elapsed=T-dt;
    flower_prepare(dt,0,0,sp);
}
static void draw_full(uint16_t *out) { flower_draw(out,0,H); }
static void draw_bg(uint16_t *out) {
    const GardenFrame *g=(const GardenFrame*)(seed_map+32*32);
    for(int y=0;y<H;y++)garden_row(out+y*W,y,g);
}
// The projection ray_row inverts: screen x = 180 + SCALE*cx, y = 65 - SCALE*cy.
static void snapshot_centres(void) {
    for(unsigned i=0;i<count&&i<MAX_PARTS;i++) {
        cx0[i]=180+SCALE*petals[i].c.x;
        cy0[i]=65-SCALE*petals[i].c.y;
        bx0[i]=petals[i].xmin;bX0[i]=petals[i].xmax;
        by0[i]=petals[i].ymin;bY0[i]=petals[i].ymax;
    }
}
static int edge_moved;
// Two different things, so two numbers. The centre displacement is continuous
// and is what a reprojection warp could correct; the bounding box is integer
// and jumps by a whole pixel at a time, so taking the larger of the two would
// just report 1 forever.
static double drift_since(void) {
    double worst=0;edge_moved=0;
    for(unsigned i=0;i<count&&i<MAX_PARTS;i++) {
        double dx=180+SCALE*petals[i].c.x-cx0[i];
        double dy=65-SCALE*petals[i].c.y-cy0[i];
        double d=sqrt(dx*dx+dy*dy);
        if(d>worst)worst=d;
        // A centre that has not moved does not mean the part has not: breath
        // and cup change radii, which moves the outline and nothing else.
        int e[4]={abs(petals[i].xmin-bx0[i]),abs(petals[i].xmax-bX0[i]),
                  abs(petals[i].ymin-by0[i]),abs(petals[i].ymax-bY0[i])};
        for(int k=0;k<4;k++)if(e[k]>edge_moved)edge_moved=e[k];
    }
    return worst;
}

int main(int argc,char**argv) {
    const char *nm[]={"CRYSTAL","VALLEY","SUNFLOWER","SNOWDROP","TULIP",
                      "DAFFODIL","CROCUS","CALLA"};
    // The rate the device actually runs at with the PIE kernel in. Pass a
    // different one to see how the answer moves with it; the whole point is
    // that a slower frame makes staleness worse, not better.
    double fps=argc>1?atof(argv[1]):15.0;
    float dt=(float)(1.0/fps);
    printf("reuse of k traced frames, at %.1f fps (dt=%.4f s)\n\n",fps,dt);
    printf("%-10s %2s  %8s  %8s  %5s\n","species","k","drift px","moved","step");
    for(int sp=0;sp<8;sp++) {
        for(int k=1;k<=6;k++) {
            double worst_drift=0,worst_moved=0;int worst_step=0,worst_edge=0;
            // Several base times, because the yaw is steady but the breathing
            // is not: the fastest moment is not the average one.
            for(int t=0;t<8;t++) {
                float T=3.1f+t*1.7f;
                at(T-k*dt,dt,(flower_species_t)sp);
                snapshot_centres();
                draw_full(old_full);
                at(T-k*dt,dt,(flower_species_t)sp);
                draw_bg(old_bg);
                at(T,dt,(flower_species_t)sp);
                double dr=drift_since();int ed=edge_moved;
                draw_full(now_full);
                at(T,dt,(flower_species_t)sp);
                draw_bg(now_bg);
                unsigned moved=0;int step=0;
                for(int i=0;i<W*H;i++) {
                    reused[i]=old_full[i]!=old_bg[i]?old_full[i]:now_bg[i];
                    if(reused[i]==now_full[i])continue;
                    moved++;
                    int a[3]={(reused[i]>>11)&31,(reused[i]>>5)&63,reused[i]&31};
                    int b[3]={(now_full[i]>>11)&31,(now_full[i]>>5)&63,now_full[i]&31};
                    for(int c=0;c<3;c++){int e=abs(a[c]-b[c]);if(e>step)step=e;}
                }
                double frac=100.0*moved/(W*H);
                if(dr>worst_drift)worst_drift=dr;
                if(ed>worst_edge)worst_edge=ed;
                if(frac>worst_moved)worst_moved=frac;
                if(step>worst_step)worst_step=step;
            }
            printf("%-10s %2d  %8.3f  %4d  %7.3f%%  %5d\n",
                   k==1?nm[sp]:"",k,worst_drift,worst_edge,worst_moved,worst_step);
        }
    }
    return 0;
}
