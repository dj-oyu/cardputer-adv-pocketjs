// Host: gcc -O2 -Wall -Wextra tools/test_flower.c -lm -o .cache/test_flower
#include "../main/scene/scene_mem.c"
#include "../main/scene/garden.c"
#include "../main/scene/flower.c"
#include "../main/scene/flower_species.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "flower_catalog.h"
static uint16_t full[W*H],assembled[W*H],background[W*H];
// Pin the grain. It moves once a frame by design, so any assertion that two
// renders of the same moment are identical has to hold it still -- the same
// reason hush_swarm() exists, and the same failure if it is forgotten: a
// mismatch that looks like a rendering bug and is a clock.
static void hush_grain(void) { flower_grain_frame=7; }
static void background_frame(void) {
    hush_grain();
    // The *same* frame flower_draw will use, not a fresh one prepared to the
    // same time. The garden now carries a swarm, and preparing a second frame
    // advances a second swarm -- the two would agree everywhere except at
    // fourteen points of light, which is exactly where this assertion looks.
    GardenFrame f={0};
    if(seed_map)f=*(const GardenFrame*)(seed_map+32*32);
    else garden_prepare(&f,elapsed);
    for(int y=0;y<H;y++)garden_row_blend(background+y*W,y,&f,bloom_garden_old_seed,bloom_garden_mix);
}
// The swarm's state lives in the shared block, so releasing the block loses it
// and the next frame seeds a new one. That is correct and unavoidable -- there
// is nowhere else for it to live -- but the checks below are about whether a
// *stale seed texture* survives a recycle, and fourteen points of light moving
// would answer a different question loudly. Silenced for those, not for the
// frame comparisons that come before them.
static void hush_swarm(void) {
    if(!seed_map)return;
    GardenFrame *g=(GardenFrame*)(seed_map+32*32);
    for(int i=0;i<GARDEN_MOTES;i++)g->mote[i].glow=0;
}
static void ppm(const char *path,const uint16_t *p) {
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",W,H);
    for(int i=0;i<W*H;i++) {
        unsigned c=p[i];unsigned char rgb[3]={((c>>11)&31)*255/31,((c>>5)&63)*255/63,(c&31)*255/31};
        assert(fwrite(rgb,1,3,f)==3);
    }
    assert(fclose(f)==0);
}
int main(void) {
    setvbuf(stdout,NULL,_IOLBF,0);
    struct {uint16_t head[8],data[W*8],tail[8];} band;
    unsigned long covered_sunflower=0;
    clock_t start=clock();
    // Use the sunflower's ellipsoids to check that a strip drawn
    // on its own equals the same rows of the whole frame, it writes nothing
    // outside the rows it was given, and it leaves the left half alone.
    for(int frame=0;frame<60;frame++) {
        elapsed=frame*.7f;
        flower_prepare(1.0f/30,frame%3==0?-180:180,frame%2?-180:180,FLOWER_SUNFLOWER);
        hush_grain();flower_draw(full,0,H);
        background_frame();
        for(int y=0;y<H;y+=8) {
            memset(&band,0xa5,sizeof band);int h=H-y<8?H-y:8;
            flower_draw(band.data,y,h);
            for(int k=0;k<8;k++)assert(band.head[k]==0xa5a5&&band.tail[k]==0xa5a5);
            for(int k=W*h;k<W*8;k++)assert(band.data[k]==0xa5a5);
            memcpy(assembled+y*W,band.data,W*h*2);
        }
        assert(memcmp(full,assembled,sizeof full)==0);
        for(int y=0;y<H;y++)for(int x=0;x<X0;x++)assert(full[y*W+x]==background[y*W+x]);
        for(int y=12;y<120;y++)for(int x=X0;x<W;x++)
            covered_sunflower+=full[y*W+x]!=background[y*W+x];
    }
    // The flower is actually on screen rather than a field of background.
    assert(covered_sunflower>60000);
    // Center rays hit the front pole; this also detects a wrong quadratic root.
    for(unsigned i=0;i<count;i++)assert(petals[i].invzz>0&&isfinite(petals[i].invzz));
    memset(&band,0xa5,sizeof band);flower_draw(band.data,-1,8);
    for(int k=0;k<W*8;k++)assert(band.data[k]==0xa5a5);
    flower_prepare(0,0,0,(flower_species_t)-1);
    assert(current_species==FLOWER_VALLEY&&count>0);
    flower_prepare(0,0,0,FLOWER_SPECIES_COUNT);
    assert(current_species==FLOWER_VALLEY&&count>0);
    for(int species=0;species<FLOWER_SPECIES_COUNT;species++) {
        unsigned covered=0;
        for(int frame=0;frame<60;frame++) {
            elapsed=frame*.7f;
            flower_prepare(.033f,frame%3==0?-180:180,frame%2?-180:180,(flower_species_t)species);
            assert(count>8&&count<MAX_PARTS);
            for(unsigned i=0;i<count;i++) {
                assert(isfinite(petals[i].invzz)&&petals[i].invzz>0);
                for(int j=0;j<3;j++) {
                    assert(petals[i].radius[j]>0&&isfinite(petals[i].radius[j]));
                    assert(fabsf(dot(petals[i].axis[j],petals[i].axis[j])-1)<.0001f);
                    assert(fabsf(dot(petals[i].axis[j],petals[i].axis[(j+1)%3]))<.0001f);
                }
            }
            hush_grain();flower_draw(full,0,H);
            background_frame();
            for(int y=0;y<H;y+=8) {
                memset(&band,0xa5,sizeof band);int h=H-y<8?H-y:8;
                hush_grain();flower_draw(band.data,y,h);
                for(int k=0;k<8;k++)assert(band.head[k]==0xa5a5&&band.tail[k]==0xa5a5);
                for(int k=W*h;k<W*8;k++)assert(band.data[k]==0xa5a5);
                memcpy(assembled+y*W,band.data,W*h*2);
            }
            assert(memcmp(full,assembled,sizeof full)==0);
            for(int y=0;y<H;y++)for(int x=0;x<W;x++) {
                bool mark=full[y*W+x]!=background[y*W+x];
                if(x<X0||y<12)assert(!mark);
                covered+=mark;
            }
            // Every plant must enter the bottom edge instead of ending in
            // mid-air. Require actual rendered coverage, not only a bound.
            unsigned rooted=0;
            for(int x=X0;x<W;x++)rooted+=full[(H-1)*W+x]!=background[(H-1)*W+x];
            assert(rooted>0);
        }
        assert(covered>18000);
        elapsed=4;flower_prepare(0,0,0,(flower_species_t)species);
        char path[96];snprintf(path,sizeof path,".cache/flower-%s.ppm",flower_names[species]);
        hush_grain();flower_draw(full,0,H);ppm(path,full);
        printf("SPECIES_OK %s: 60 poses, %u analytic parts, no stored vertices\n",flower_names[species],count);
    }
    // Analytic bell sanity checks independent of the assembled plant.
    // Built by hand, so it needs the same derivation flower_prepare does; see
    // petal_reciprocals. Without it bell_hit divides by uninitialised memory.
    Petal p={.axis={{1,0,0},{0,1,0},{0,0,1}},.radius={1,1,1}};
    petal_reciprocals(&p);
    float z=-1000;V n;
    assert(bell_hit_at(&p,0,0,&z,&n)&&fabsf(z-.88f)<.0001f);
    z=-1000;assert(!bell_hit_at(&p,1.3f,0,&z,&n));
    p.axis[1]=(V){0,0,1};p.axis[2]=(V){0,1,0};petal_reciprocals(&p);
    z=-1000;assert(bell_hit_at(&p,.5f,0,&z,&n)&&z<.9f); // No artificial cap across the mouth.
    // Puffy shoulders extend beyond the unit radius without getting culled.
    p.axis[1]=(V){0,1,0};p.axis[2]=(V){0,0,1};
    p.shape=FLOWER_SHAPE_CLOCHE;petal_reciprocals(&p);
    z=-1000;assert(bell_hit_at(&p,0,0,&z,&n)&&fabsf(z-1.04f)<.0001f);
    z=-1000;assert(bell_hit_at(&p,1.06f,-.34f,&z,&n)&&z>0);
    z=-1000;assert(bell_hit_at(&p,0,-.8f,&z,&n)&&fabsf(z-.546f)<.0001f);
    z=-1000;assert(bell_hit_at(&p,0,.9f,&z,&n)&&fabsf(z-.888f)<.0001f);
    // The shared block is recycled between scenes and given back when an app
    // starts, so a scene that takes it must rebuild what it caches there. That
    // is the one thing this arrangement can get wrong, and it fails silently --
    // a stale seed_map is a plausible-looking flower with the wrong texture.
    // Drawing the same pose either side of a release must agree exactly.
    elapsed=4;flower_prepare(0,0,0,FLOWER_SUNFLOWER);hush_swarm();
    hush_grain();flower_draw(full,0,H);
    memcpy(assembled,full,sizeof full);
    scene_mem_release();
    elapsed=4;flower_prepare(0,0,0,FLOWER_SUNFLOWER);hush_swarm();
    hush_grain();flower_draw(full,0,H);
    assert(memcmp(full,assembled,sizeof full)==0);
    // And a foreign owner taking the block in between must not change that.
    static const char other_owner;
    bool stolen;
    void *b=scene_mem(&other_owner,64,&stolen);
    assert(b&&stolen);
    memset(b,0x5a,64);
    elapsed=4;flower_prepare(0,0,0,FLOWER_SUNFLOWER);hush_swarm();
    hush_grain();flower_draw(full,0,H);
    assert(memcmp(full,assembled,sizeof full)==0);
    // ---- the block is 16-byte aligned ----------------------------------
    // scene_mem.h promises this because the vector rows load 128 bits at a
    // time and a misaligned base does not fault -- it zeroes the low four
    // address bits and draws a plausible picture. Checked across the shapes
    // that can move a block: a fresh take, a release and re-take, a grow, and
    // a take by another owner at another size.
    //
    // The pointer checks below are weak evidence on their own: glibc's malloc
    // already returns 16-aligned memory on x86-64, so they would pass even if
    // align_up() did nothing. The device's allocator is the one that returns 8.
    // So the arithmetic is checked directly, over every residue a raw block can
    // land on -- that part is the same code the device runs.
    for(uintptr_t base=1;base<=128;base++) {
        uintptr_t up=(uintptr_t)align_up((void *)base);
        assert((up&15)==0);          // aligned
        assert(up>=base);            // never below what was allocated
        assert(up-base<16);          // and never wastes more than the 15 paid for
    }
    scene_mem_release();
    for(unsigned size=1;size<=8192;size=size*3+7) {
        bool r;
        void *a=scene_mem(&other_owner,size,&r);
        assert(a&&((uintptr_t)a&15)==0);
        void *b=scene_bulk(&other_owner,size,&r);
        assert(b&&((uintptr_t)b&15)==0);
        // A fresh block is zeroed, so the ocean's read-past-the-drawn-pixels
        // pad never yields whatever the allocator happened to hold.
        for(size_t i=0;i<size;i++)assert(((unsigned char *)b)[i]==0);
        void *c=scene_mem(&flower_owner,size+1,&r);
        assert(c&&((uintptr_t)c&15)==0);
    }
    scene_mem_release();

    // ---- the single FLOWER row's rotation ------------------------------
    // The menu row rotates all botanicals. Changing
    // species rebuilds the whole part list between one frame and the next, so
    // the swap is hidden behind a dissolve; what has to be true is that every
    // swap lands on a frame with nothing of either plant on it, that all species
    // are actually reached, and that the part list is valid at every single
    // frame rather than only at the poses the per-species loop above samples.
    unsigned seen[FLOWER_SPECIES_COUNT]={0};
    unsigned swaps=0,dissolving=0,opaque=0;
    flower_species_t previous=flower_current_species();
    unsigned previous_layout=0;
    // Cover the enlarged collection. Floating accumulation can put each swap
    // one frame after 40 seconds, so allow that frame per interval explicitly.
    const int ROTATIONS=128;
    const int FRAMES=ROTATIONS*((int)(FLOWER_ROTATE_S*30)+1)+120;
    for(int frame=0;frame<FRAMES;frame++) {
        flower_prepare_rotating(1.0f/30,frame%5==0?-90:90,frame%3?60:-60);
        float fade=flower_fade();
        assert(fade>=0&&fade<=1);
        // The rotation only ever shows a valid botanical.
        flower_species_t now=flower_current_species();
        unsigned layout=((GardenFrame*)(seed_map+32*32))->seed;
        // Periodic reconfiguration remains, but its first frame shows the
        // old layout exactly, with the main light still at its old shape.
        if(frame)assert((layout!=previous_layout)==(now!=previous));
        previous_layout=layout;
        assert(now>=FLOWER_VALLEY&&now<FLOWER_SPECIES_COUNT);
        seen[now]++;
        assert(count>8&&count<MAX_PARTS);
        if(now!=previous) {
            // The frame the part list was replaced on. It must be invisible:
            // fade exactly zero, and therefore not one pixel of either plant.
            swaps++;
            assert(fade==0);
            assert(bloom_garden_mix==0);
            hush_grain();flower_draw(full,0,H);
            background_frame();
            assert(memcmp(full,background,sizeof full)==0);
            // And never the same plant twice: a rotation that repeats looks
            // like it has stopped.
            assert(now!=previous);
            previous=now;
            continue;
        }
        if(fade>=1)opaque++; else dissolving++;
    }
    // One swap per interval, and the extra
    // 120 frames are there so the last one is followed by visible frames.
    // Within one, not exact. A species now lasts five shots instead of four,
    // and each shot boundary quantises to a frame and drops the overshoot, so
    // the accumulated dt over five thousand seconds lands a fraction of a
    // rotation away from where four shots did. The count being exact was a
    // property of one view-per-species, not of the rotation.
    assert(swaps+1>=(unsigned)ROTATIONS&&swaps<=(unsigned)ROTATIONS+1);
    printf("BACKGROUND_CONTINUITY_OK: %u periodic layouts begin on exact old endpoint\n",swaps);
    for(int s=FLOWER_VALLEY;s<FLOWER_SPECIES_COUNT;s++)
        assert(seen[s]>0);   // every botanical actually came up
    // The dissolve happened and was brief: 1.2 s of every 40 at 30 fps is 36
    // frames a swap, so it is a small fraction of the time but not zero.
    // Derived from the timing rather than from what came out. Every shot ends
    // in a dissolve now, not every species, so the fraction of frames spent
    // fading is FADE_S/VIEW_S -- 12% at four shots of ten seconds. The old
    // bound was FRAMES/8, which this passes by four hundred frames out of
    // 154,000: one more viewpoint would have failed it, for the reason that the
    // feature works. Twice the expected fraction is the bound, and it moves
    // with the constants instead of having to be re-fitted.
    assert(dissolving>0);
    assert(dissolving<(unsigned)(FRAMES*2.0*FLOWER_FADE_S/FLOWER_VIEW_MIN_S));
    assert(opaque>FRAMES*3/4);
    // Naming a species directly must be unaffected by any of that.
    // Against the same `assembled` as the recycle checks above, so the swarm is
    // hushed here for the same reason: 28,920 frames of rotation have advanced
    // it, and this assertion is about the species and the dissolve.
    elapsed=4;flower_prepare(0,0,0,FLOWER_SUNFLOWER);hush_swarm();
    assert(flower_fade()==1);
    hush_grain();flower_draw(full,0,H);
    assert(memcmp(full,assembled,sizeof full)==0);
    printf("ROTATION_OK: %u swaps over %d frames, all botanicals, every "
           "swap on a blank frame, %u dissolving / %u opaque; interval %.0fs "
           "fade %.1fs\n",
           swaps,FRAMES,dissolving,opaque,(double)FLOWER_ROTATE_S,(double)FLOWER_FADE_S);

    // ---------------------------------------------------------------------
    // The viewpoints, and the defocus.
    //
    // "N views, then a new flower" can fail silently in a way nothing else here
    // notices: the cycle can run, the timing can be right, and every view can
    // look the same. So the views are compared against each other rather than
    // just counted -- and the tilt-shift is measured as detail present in the
    // band and absent away from it, which is the only thing that distinguishes
    // it from a filter that is switched on but doing nothing.
    // ---------------------------------------------------------------------
    {
        static uint16_t shot[FLOWER_VIEWS][W*H];
        unsigned lit[FLOWER_VIEWS]={0};
        long cx[FLOWER_VIEWS]={0};
        // Start of a fresh plant, then walk to the middle of each view.
        while(flower_fade()<1)flower_prepare_rotating(0.04f,0,0);
        for(int v=0;v<FLOWER_VIEWS;v++) {
            flower_prepare_rotating(0.0f,0,0);
            hush_grain();flower_draw(shot[v],0,H);
            background_frame();
            for(int k=0;k<W*H;k++)
                if(shot[v][k]!=background[k]) {
                    lit[v]++;cx[v]+=k%W;
                }
            assert(lit[v]>400);
            cx[v]/=(long)lit[v];
            // Advance to the middle of the next view.
            for(int f=0;f<(int)(FLOWER_SHOTS[v].hold*25);f++)
                flower_prepare_rotating(0.04f,0,0);
        }
        // Every pair of views has to differ, and by more than a rounding: a
        // plant turned by a quarter of a circle is a different silhouette.
        unsigned worst=~0u;
        for(int a=0;a<FLOWER_VIEWS;a++)
            for(int b=a+1;b<FLOWER_VIEWS;b++) {
                unsigned diff=0;
                for(int k=0;k<W*H;k++)diff+=shot[a][k]!=shot[b][k];
                if(diff<worst)worst=diff;
            }
        assert(worst>300);
        // Pixels are the wrong evidence for this claim, and that was invisible
        // all day. The shots are sampled seconds apart, so the plant's sway and
        // pose differ between them and the frames differ in pixels EVEN IF THE
        // CAMERA IS BYTE-IDENTICAL -- which it was, for four hours, because
        // zoom and aim were being thrown away before they reached the
        // projection. This assertion passed the whole time, on a feature that
        // was disconnected.
        //
        // So the camera itself is checked, not its output: the framing a shot
        // asks for has to actually arrive at the projection. Comparing outputs
        // when the claim is about inputs is the same error as comparing pixels
        // to prove a size ladder.
        for(int v=0;v<FLOWER_VIEWS;v++) {
            while(flower_fade()<1)flower_prepare_rotating(0.04f,0,0);
            if(bloom_view!=v)continue;
            flower_prepare_rotating(0.0f,0,0);
            if(FLOWER_SHOTS[v].zoom>0) {
            if(FLOWER_SHOTS[v].zoom>0) {
                assert(cam_s>SCALE*FLOWER_SHOTS[v].zoom-0.01f);
                assert(cam_s<SCALE*FLOWER_SHOTS[v].zoom+0.01f);
            } else assert(cam_s>0&&cam_s<=SCALE+0.01f);
            } else {
                // A fitted shot has no number to compare against -- the whole
                // point is that the renderer picks it per species. What can be
                // asserted is the property that was asked for: something was
                // chosen, and it never magnifies.
                assert(cam_s>0&&cam_s<SCALE+0.01f);
            }
        }
        // And the dwells still sum to the species interval, which is what
        // ROTATION_OK's swap count is measured against.
        {
            float sum=0;
            for(int v=0;v<FLOWER_VIEWS;v++)sum+=FLOWER_SHOTS[v].hold;
            assert(sum>FLOWER_ROTATE_S-0.01f&&sum<FLOWER_ROTATE_S+0.01f);
        }
        // Twenty degrees, checked on the table rather than on the clamp -- the
        // clamp would hide an entry that had drifted past it, and the point is
        // that the shots are chosen inside the bound, not trimmed to it.
        for(int v=0;v<FLOWER_VIEWS;v++) {
            // Both signs now -- a low angle and a near high angle were asked
            // for by name. What is bounded is the magnitude, because past
            // twenty degrees an orthographic camera stops reading as a camera.
            assert(FLOWER_SHOTS[v].pitch<=FLOWER_PITCH_MAX);
            assert(FLOWER_SHOTS[v].pitch>=-FLOWER_PITCH_MAX);
            // Framing, not just angle: a shot that neither moves the camera
            // nor changes what it points at is not a shot, and four of those
            // in a row is what "no intent per cut" was describing.
            // zoom <= 0 asks the renderer to fit the plant to the window;
            // anything else is a magnification and stays in range.
            assert(FLOWER_SHOTS[v].zoom<=0.0f||
                   (FLOWER_SHOTS[v].zoom>=1.0f&&FLOWER_SHOTS[v].zoom<=4.0f));
            // The size ladder, asserted rather than trusted. Two adjacent
            // shots close in size read as a stutter rather than as a cut, and
            // the previous list had four of five within sixteen percent -- a
            // failure that no ordering could have fixed and that nothing here
            // would have caught. Adjacent includes the wrap: the last shot is
            // followed by the first, across the dissolve.
            //
            // It is a claim about how one cut reads against the NEXT, so it
            // says nothing when a species holds a single shot: with one entry
            // the wrap compares a shot to itself, the ratio is 1 by
            // construction, and the assertion would fail on a table that has
            // no ladder to get wrong. Guarded rather than deleted -- cutting
            // within a species was turned off for a frame-rate reason, not a
            // compositional one, and if it comes back this has to hold again
            // on the way in.
            if(FLOWER_VIEWS>1) {
                float a=FLOWER_SHOTS[v].zoom;
                float b=FLOWER_SHOTS[(v+1)%FLOWER_VIEWS].zoom;
                float hi=a>b?a:b,lo=a>b?b:a;
                assert(hi/lo>1.16f);
            }
            // Bounded by the interval rather than by a constant that quietly
            // assumed several shots would share it: one shot holds all of it.
            assert(FLOWER_SHOTS[v].hold>=4.0f&&FLOWER_SHOTS[v].hold<=FLOWER_ROTATE_S);
            assert(FLOWER_SHOTS[v].aim>=0.0f&&FLOWER_SHOTS[v].aim<=1.0f);
        }
        // ---- a fitted shot actually fits -----------------------------
        // Showing the whole plant is the wide shot's only job, and a constant
        // zoom failed it on thirteen of the fourteen species with nothing
        // noticing, because a cropped plant looks like a framed one unless you
        // know it continued past the edge. So the property is asserted, not the
        // number: for a fitted shot, on every species, nothing the plant draws
        // may touch the window border. This is the check that was missing, and
        // it is per species because the fit is per species.
        for(int v=0;v<FLOWER_VIEWS;v++) {
            if(FLOWER_SHOTS[v].zoom>0)continue;
            for(int s=0;s<FLOWER_SPECIES_COUNT;s++) {
                elapsed=4;
                bloom_view=v;
                bloom_species=(flower_species_t)s;
                bloom_elapsed=FLOWER_SHOTS[v].hold*0.5f;
                flower_prepare_rotating(0.0f,0,0);
                hush_grain();flower_draw(full,0,H);
                background_frame();
                int xlo=W,xhi=-1,ylo=H,yhi=-1;
                for(int y=0;y<H;y++)for(int x=0;x<W;x++)
                    if(full[y*W+x]!=background[y*W+x]) {
                        if(x<xlo)xlo=x;
                        if(x>xhi)xhi=x;
                        if(y<ylo)ylo=y;
                        if(y>yhi)yhi=y;
                    }
                assert(xhi>=0);          /* the plant is on screen at all */
                assert(xlo>X0&&xhi<W-1); /* and clear of every border */
                assert(ylo>12&&yhi<H-1);
            }
        }
        // With one shot there is no pair, and printing `worst` would report
        // an untouched sentinel as a measurement -- the exact shape of claim
        // this block exists to stop.
        if(FLOWER_VIEWS>1)
            printf("VIEWS_OK: %d shots a plant over %.0f s; the closest"
                   " pair of views differs in %u pixels\n",
                   FLOWER_VIEWS,(double)FLOWER_ROTATE_S,worst);
        else
            printf("VIEWS_OK: one shot a plant, zoom %.2f held %.0f s;"
                   " no pair to compare\n",
                   (double)FLOWER_SHOTS[0].zoom,(double)FLOWER_SHOTS[0].hold);
    }
    // ---------------------------------------------------------------------
    // The log lines, counted rather than read.
    //
    // A SPLIT line with twenty-six conversions had three arguments written in
    // the wrong place -- the text at the end, the values after `rest=` -- and
    // every argument past the twenty-first went to the wrong conversion. A
    // float promoted through varargs came out of an integer slot as 1072049757
    // on six consecutive lines. It cost a flash and a measurement session.
    //
    // The compiler cannot catch it: ESP_LOGI carries no format attribute here,
    // and the block is inside #ifdef ESP_PLATFORM so the host never compiles
    // it at all. So this counts the conversions and the arguments in the source
    // text, which is crude and is the only check available. It is the same
    // technique tools/test_menu_rows.c uses on shell.c's draw order, and for
    // the same reason: the property is real, and reading it off the real file
    // beats asserting it about a copy.
    // ---------------------------------------------------------------------
    {
        FILE *fp=fopen("main/scene/flower.c","rb");
        assert(fp);
        static char src[262144];
        size_t n=fread(src,1,sizeof src-1,fp);
        fclose(fp);
        src[n]=0;
        unsigned checked=0;
        for(char *at=strstr(src,"ESP_LOGI(");at;at=strstr(at+1,"ESP_LOGI(")) {
            int depth=0,instr=0,conv=0,commas=0;
            for(char *c=at+8;*c;c++) {
                if(instr) {
                    if(*c=='\\'&&c[1]) { c++;continue; }
                    if(*c=='"') { instr=0;continue; }
                    // A conversion, and %% is not one.
                    if(*c=='%') {
                        if(c[1]=='%') { c++;continue; }
                        conv++;
                    }
                    continue;
                }
                if(*c=='"') { instr=1;continue; }
                if(*c=='/'&&c[1]=='/') { while(*c&&*c!='\n')c++; continue; }
                if(*c=='('||*c=='[') { depth++;continue; }
                if(*c==')'||*c==']') {
                    depth--;
                    if(depth==0)break;
                    continue;
                }
                if(*c==','&&depth==1)commas++;
            }
            // ESP_LOGI(tag, fmt, ...): the first comma separates tag from
            // format, so the arguments are the commas after it.
            assert(commas>=1);
            unsigned args=(unsigned)commas-1;
            assert(args==(unsigned)conv);
            checked++;
        }
        assert(checked>=2);
        printf("LOGFMT_OK: %u log lines, conversions and arguments agree\n",checked);
    }
    printf("FLOWER_OK: strip equivalence, bounds, 60 poses per species, block recycling;"
           " static residue %zu bytes, shared block %zu; host %.3fs (not device timing)\n",
           sizeof petals+sizeof depth+sizeof seed_map+sizeof elapsed
           +sizeof bell_slopes+sizeof bell_offsets+sizeof count+sizeof current_species
           +sizeof seeds_ready,
           (size_t)FLOWER_BYTES,
           (double)(clock()-start)/CLOCKS_PER_SEC);
}
