// Host: gcc -O2 -Wall -Wextra tools/test_flower.c -lm -o .cache/test_flower
#include "../main/scene/scene_mem.c"
#include "../main/scene/garden.c"
#include "../main/scene/flower.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
static uint16_t full[W*H],assembled[W*H],background[W*H];
static void background_frame(void) {
    GardenFrame f;garden_prepare(&f,elapsed);
    if(seed_map)f.seed=((GardenFrame*)(seed_map+32*32))->seed;
    for(int y=0;y<H;y++)garden_row(background+y*W,y,&f);
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
    struct {uint16_t head[8],data[W*8],tail[8];} band;
    unsigned long covered_crystal=0;
    clock_t start=clock();
    // The crystal flower used to be drawn two ways and the two silhouettes
    // compared. The mesh is gone -- it cost 17,472 bytes of .bss for the whole
    // life of the boot and the analytic path stores nothing -- so what is left
    // to check is that the one remaining path is self-consistent: a strip drawn
    // on its own equals the same rows of the whole frame, it writes nothing
    // outside the rows it was given, and it leaves the left half alone.
    for(int frame=0;frame<60;frame++) {
        elapsed=frame*.7f;
        flower_prepare(1.0f/30,frame%3==0?-180:180,frame%2?-180:180,FLOWER_CRYSTAL);
        flower_draw(full,0,H);
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
            covered_crystal+=full[y*W+x]!=background[y*W+x];
    }
    // The flower is actually on screen rather than a field of background.
    assert(covered_crystal>60000);
    // Center rays hit the front pole; this also detects a wrong quadratic root.
    for(int i=0;i<PETALS;i++)assert(petals[i].invzz>0&&isfinite(petals[i].invzz));
    memset(&band,0xa5,sizeof band);flower_draw(band.data,-1,8);
    for(int k=0;k<W*8;k++)assert(band.data[k]==0xa5a5);
    elapsed=4;flower_prepare(0,0,0,FLOWER_CRYSTAL);
    flower_draw(full,0,H);ppm(".cache/flower-ray.ppm",full);
    const char *files[]={"", ".cache/flower-valley.ppm", ".cache/flower-sunflower.ppm", ".cache/flower-snowdrop.ppm",
        ".cache/flower-tulip.ppm", ".cache/flower-daffodil.ppm", ".cache/flower-crocus.ppm", ".cache/flower-calla.ppm"};
    _Static_assert(sizeof files/sizeof files[0]==FLOWER_SPECIES_COUNT,"preview list");
    for(int species=1;species<FLOWER_SPECIES_COUNT;species++) {
        unsigned covered=0;
        for(int frame=0;frame<60;frame++) {
            elapsed=frame*.7f;
            flower_prepare(.033f,frame%3==0?-180:180,frame%2?-180:180,(flower_species_t)species);
            assert(count>8&&count<MAX_PARTS);
            flower_draw(full,0,H);
            background_frame();
            for(int y=0;y<H;y+=8) {
                memset(&band,0xa5,sizeof band);int h=H-y<8?H-y:8;
                flower_draw(band.data,y,h);
                for(int k=0;k<8;k++)assert(band.head[k]==0xa5a5&&band.tail[k]==0xa5a5);
                for(int k=W*h;k<W*8;k++)assert(band.data[k]==0xa5a5);
                memcpy(assembled+y*W,band.data,W*h*2);
            }
            assert(memcmp(full,assembled,sizeof full)==0);
            for(int y=0;y<H;y++)for(int x=0;x<W;x++) {
                bool mark=full[y*W+x]!=background[y*W+x];
                if(x<X0||y<12||y>119)assert(!mark);
                covered+=mark;
            }
        }
        assert(covered>18000);
        elapsed=4;flower_prepare(0,0,0,(flower_species_t)species);
        flower_draw(full,0,H);ppm(files[species],full);
        printf("SPECIES_OK %d: 60 poses, %u analytic parts, no stored vertices\n",species,count);
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
    // The shared block is recycled between scenes and given back when an app
    // starts, so a scene that takes it must rebuild what it caches there. That
    // is the one thing this arrangement can get wrong, and it fails silently --
    // a stale seed_map is a plausible-looking flower with the wrong texture.
    // Drawing the same pose either side of a release must agree exactly.
    elapsed=4;flower_prepare(0,0,0,FLOWER_SUNFLOWER);
    flower_draw(full,0,H);
    memcpy(assembled,full,sizeof full);
    scene_mem_release();
    elapsed=4;flower_prepare(0,0,0,FLOWER_SUNFLOWER);
    flower_draw(full,0,H);
    assert(memcmp(full,assembled,sizeof full)==0);
    // And a foreign owner taking the block in between must not change that.
    static const char other_owner;
    bool stolen;
    void *b=scene_mem(&other_owner,64,&stolen);
    assert(b&&stolen);
    memset(b,0x5a,64);
    elapsed=4;flower_prepare(0,0,0,FLOWER_SUNFLOWER);
    flower_draw(full,0,H);
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
    // Four menu rows became one that rotates the three botanicals. Changing
    // species rebuilds the whole part list between one frame and the next, so
    // the swap is hidden behind a dissolve; what has to be true is that every
    // swap lands on a frame with nothing of either plant on it, that all three
    // are actually reached, and that the part list is valid at every single
    // frame rather than only at the poses the per-species loop above samples.
    unsigned seen[FLOWER_SPECIES_COUNT]={0};
    unsigned swaps=0,dissolving=0,opaque=0;
    flower_species_t previous=flower_current_species();
    unsigned previous_layout=bloom_rng;
    // Four rotations at 30 fps. Long enough that the generator has to produce
    // every species rather than happening to.
    const int ROTATIONS=24;
    const int FRAMES=(int)(ROTATIONS*FLOWER_ROTATE_S*30)+120;
    for(int frame=0;frame<FRAMES;frame++) {
        flower_prepare_rotating(1.0f/30,frame%5==0?-90:90,frame%3?60:-60);
        float fade=flower_fade();
        assert(fade>=0&&fade<=1);
        // The rotation only ever shows a botanical; CRYSTAL is not in it.
        flower_species_t now=flower_current_species();
        unsigned layout=((GardenFrame*)(seed_map+32*32))->seed;
        assert((layout!=previous_layout)==(now!=previous));
        previous_layout=layout;
        assert(now>=FLOWER_VALLEY&&now<FLOWER_SPECIES_COUNT);
        seen[now]++;
        assert(count>8&&count<MAX_PARTS);
        if(now!=previous) {
            // The frame the part list was replaced on. It must be invisible:
            // fade exactly zero, and therefore not one pixel of either plant.
            swaps++;
            assert(fade==0);
            flower_draw(full,0,H);
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
    // Four intervals of 40 s in 4x40x30+120 frames: four swaps, and the extra
    // 120 frames are there so the last one is followed by visible frames.
    assert(swaps==(unsigned)ROTATIONS);
    for(int s=FLOWER_VALLEY;s<FLOWER_SPECIES_COUNT;s++)
        assert(seen[s]>0);   // all three botanicals actually came up
    // The dissolve happened and was brief: 1.2 s of every 40 at 30 fps is 36
    // frames a swap, so it is a small fraction of the time but not zero.
    assert(dissolving>0&&dissolving<FRAMES/8);
    assert(opaque>FRAMES*3/4);
    // Naming a species directly must be unaffected by any of that.
    elapsed=4;flower_prepare(0,0,0,FLOWER_SUNFLOWER);
    assert(flower_fade()==1);
    flower_draw(full,0,H);
    assert(memcmp(full,assembled,sizeof full)==0);
    printf("ROTATION_OK: %u swaps over %d frames, all botanicals, every "
           "swap on a blank frame, %u dissolving / %u opaque; interval %.0fs "
           "fade %.1fs\n",
           swaps,FRAMES,dissolving,opaque,(double)FLOWER_ROTATE_S,(double)FLOWER_FADE_S);

    printf("FLOWER_OK: strip equivalence, bounds, 60 poses per species, block recycling;"
           " static residue %zu bytes, shared block %zu; host %.3fs (not device timing)\n",
           sizeof petals+sizeof depth+sizeof seed_map+sizeof elapsed
           +sizeof bell_slopes+sizeof bell_offsets+sizeof count+sizeof current_species
           +sizeof seeds_ready,
           (size_t)FLOWER_BYTES,
           (double)(clock()-start)/CLOCKS_PER_SEC);
}
