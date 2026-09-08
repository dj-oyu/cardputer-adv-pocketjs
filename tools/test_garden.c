#include "../main/scene/garden.c"
#include "../main/scene/glass_rain.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t before[240*135],after[240*135],wet[240*135];
int main(void) {
    // Noise stays bounded and continuous at lattice and period boundaries.
    for(unsigned p=0;p<65536;p++) {
        int v=garden_motion(p,419),next=garden_motion(p+1,419);
        assert(v>=0&&v<=255&&abs(v-next)<=2);
        assert(v==garden_motion(p+65536,419));
    }
    // Seed changes rearrange vegetation, while drawing the same seed is stable.
    GardenFrame fixed={0};garden_prepare(&fixed,12);
    for(int y=0;y<135;y++)garden_row(before+y*240,y,&fixed);
    fixed.seed=12345;
    for(int y=0;y<135;y++)garden_row(after+y*240,y,&fixed);
    assert(memcmp(before,after,sizeof before));
    for(int y=0;y<135;y++)garden_row(wet+y*240,y,&fixed);
    assert(!memcmp(wet,after,sizeof wet));
    GardenFrame a={0},b={0};garden_prepare(&a,4);garden_prepare(&b,9);
    unsigned changing=0,gradient=0,lit_left=0;
    for(int y=0;y<135;y++) {
        struct {uint16_t lo[8],row[240],hi[8];} guard;
        memset(&guard,0xa5,sizeof guard);garden_row(guard.row,y,&a);
        for(int i=0;i<8;i++)assert(guard.lo[i]==0xa5a5&&guard.hi[i]==0xa5a5);
        memcpy(before+y*240,guard.row,480);garden_row(after+y*240,y,&b);
        for(int x=0;x<240;x++) {
            changing+=before[y*240+x]!=after[y*240+x];
            if(x<239)gradient+=guard.row[x]!=guard.row[x+1];
            if(x<120)lit_left+=((guard.row[x]>>5)&63)>6;
        }
    }
    assert(changing>10000&&gradient>12000&&lit_left>14000);
    // The lens has background features to sample, including behind the menu.
    drops[0]=(RainDrop){.x=44,.y=57,.start=25,.age=2,.life=3,.radius=4};
    drops[1]=(RainDrop){.x=175,.y=80,.start=47,.age=2,.life=3,.radius=4};
    memcpy(wet,before,sizeof wet);glass_rain_draw(wet,0,135);
    unsigned modified=0;for(int i=0;i<240*135;i++)modified+=wet[i]!=before[i];
    assert(modified>80&&modified<500);
    memcpy(after,before,sizeof after);
    for(int y=0;y<135;y+=8)glass_rain_draw(after+y*240,y,135-y<8?135-y:8);
    assert(!memcmp(wet,after,sizeof wet));
    // ---------------------------------------------------------------------
    // The contract that replaces bit-exactness for the noise.
    //
    // The PIE kernel is allowed to move pixels -- the field is procedural mist
    // and a two-bit dither, and neither carries information a viewer can miss.
    // What it is NOT allowed to move is how bright the scene is, or how coarse
    // the noise is, because those are visible without anyone being able to say
    // which pixel changed. So the licence is written down as numbers here
    // rather than as a sentence in a commit message: same per-channel range,
    // same per-channel mean, and a dither that stays uniform and uncorrelated.
    //
    // A future kernel that drifts the mean dark, or a hash whose neighbours
    // agree too often, fails this and not a code review.
    // ---------------------------------------------------------------------
    double sum[3]={0,0,0};int lo[3]={99,99,99},hi[3]={0,0,0};unsigned n=0;
    int qlo[3]={99,99,99},qhi[3]={0,0,0};        /* the same frames, motes hushed */
    unsigned lit=0,seen[8]={0,0,0,0,0,0,0,0};   /* how far a mote moves a pixel, in steps */
    unsigned long rough=0;
    // One swarm, warmed up. A fresh GardenFrame every sample would seed fourteen
    // particles at age zero, and the eight-frame ramp that stops a replacement
    // blinking would hold every one of them at an eighth of its brightness --
    // so the contract would be measuring a scene that never appears.
    GardenFrame warm={0};
    for(int w=0;w<16;w++)garden_prepare(&warm,w*0.04f);
    for(int f=0;f<12;f++) {
        GardenFrame g=warm;garden_prepare(&g,f*2.9f);
        GardenFrame q=g;for(int i=0;i<GARDEN_MOTES;i++)q.mote[i].glow=0;
        for(int y=0;y<135;y++) {
            garden_row(before+y*240,y,&g);
            garden_row(after+y*240,y,&q);
            for(int x=0;x<240;x++) {
                uint16_t p=after[y*240+x];
                int c[3]={(p>>11)&31,(p>>5)&63,p&31};
                uint16_t w=before[y*240+x];
                int e[3]={(w>>11)&31,(w>>5)&63,w&31},worst=0;
                for(int k=0;k<3;k++) {
                    if(c[k]<qlo[k])qlo[k]=c[k];
                    if(c[k]>qhi[k])qhi[k]=c[k];
                    // Green is six bits, so a step there is half a step of the
                    // other two. Counted in fifths of the whole range, which is
                    // the unit the eye is actually working in.
                    int dd=abs(e[k]-c[k])*(k==1?1:2);
                    if(dd>worst)worst=dd;
                }
                if(worst) {
                    lit++;seen[worst>7?7:worst]++;
                }
            }
            for(int x=0;x<240;x++) {
                uint16_t p=before[y*240+x];
                int c[3]={(p>>11)&31,(p>>5)&63,p&31};
                for(int k=0;k<3;k++) {
                    sum[k]+=c[k];
                    if(c[k]<lo[k])lo[k]=c[k];
                    if(c[k]>hi[k])hi[k]=c[k];
                }
                n++;
            }
            // The field's own spatial frequency, not just the dither's. A
            // kernel that quietly coarsens the noise -- one octave instead of
            // two, say -- keeps the mean and the range and would otherwise
            // pass everything above; this is what notices.
            for(int x=16;x<240;x++) {
                int a2=(before[y*240+x]>>5)&63,b2=(before[y*240+x-16]>>5)&63;
                rough+=abs(a2-b2);
            }
        }
    }
    double mr=sum[0]/n,mg=sum[1]/n,mb=sum[2]/n;
    // Measured on the scalar reference; the lane model drifts 0.13% dark, so
    // 1% is loose enough for an approximation and tight enough to catch a
    // scene that has actually changed brightness.
    // The light, with the motes hushed: exact, and it is the same 1..13/7..25/
    // 3..10 the contract has always asserted. This half is deterministic --
    // nothing in it depends on where fourteen particles happen to be -- so
    // equality is the right test and a lane kernel that shifts the scene by a
    // step still fails here.
    assert(qlo[0]==1&&qhi[0]==13&&qlo[1]==7&&qhi[1]==25&&qlo[2]==3&&qhi[2]==10);
    // The swarm on top, and this half must NOT be an equality. The maximum of a
    // channel over twelve frames is now decided by where a particle happens to
    // be, and that is a float phase away from being a different number on a
    // different compiler -- an exact 17/28/10 is the value that came out here
    // today, not a contract. (It cost a red herring already: this assertion
    // failed on the lead's machine with numbers that appear nowhere in this
    // tree's history.)
    //
    // So: bounded above by what a mote can physically add, and strictly
    // brighter than the light alone, which is the actual claim -- the feature
    // adds light, and it adds no more than one mote's worth.
    //
    // The ceiling is derived, not observed, and it is written in terms of the
    // brightness constant so that raising the brightness moves the bound
    // instead of invalidating it. garden_shade_pixel takes the mote as
    // `sun += (extra*q)>>8` with q <= 256, so a mote adds at most its own glow.
    //   red    cr += (5*G)>>1,     red is cr>>3
    //   green  cg += (6*G)>>2,     green is cg>>2
    //   blue   cb += (G*M3)>>16,   blue is cb>>3
    // A mote whose gain, gate or fade grew would breach these; a mote that
    // stopped drawing would fail the strict inequality below.
    {
        const int G=GARDEN_GLOW_MAX;
        assert(lo[0]==qlo[0]&&lo[1]==qlo[1]&&lo[2]==qlo[2]);   /* only ever adds */
        assert(hi[0]>qhi[0]&&hi[0]<=qhi[0]+(((5*G)>>1)>>3)+1);
        assert(hi[1]>qhi[1]&&hi[1]<=qhi[1]+(((6*G)>>2)>>2)+1);
        assert(hi[2]>=qhi[2]&&hi[2]<=qhi[2]+((G/3)>>3)+1);
    }
    assert(mr>4.752&&mr<4.848&&mg>13.822&&mg<14.101&&mb>7.086&&mb<7.229);
    // ---------------------------------------------------------------------
    // How far the lanes are allowed to be from the loop they replace. The
    // licence is a bound, not a hope: every pixel of twelve frames is compared
    // against garden_row_scalar, and both the share that moves and the largest
    // step in any channel are printed and asserted. For scale, the ocean
    // approximation this project already ships (d266ecf) moves 10.1% with the
    // same per-channel bound.
    // ---------------------------------------------------------------------
    unsigned moved=0,total=0,step[3]={0,0,0};
    for(int fr=0;fr<12;fr++) {
        GardenFrame g={0};garden_prepare(&g,fr*2.9f);
        // Motes off for this one. garden_row_scalar is the statement of the
        // *light*, and it has never drawn dust; leaving the motes on here would
        // report them as approximation error and swamp the thing being
        // measured, which is how far the lane arithmetic is from the scalar
        // arithmetic and nothing else.
        for(int i=0;i<GARDEN_MOTES;i++)g.mote[i].glow=0;
        for(int y=0;y<135;y++) {
            uint16_t lane[240],ref[240];
            garden_pixels_row(lane,y,&g);garden_row_scalar(ref,y,&g);
            for(int x=0;x<240;x++) {
                total++;
                if(lane[x]==ref[x])continue;
                moved++;
                int c[3]={(lane[x]>>11)&31,(lane[x]>>5)&63,lane[x]&31};
                int d2[3]={(ref[x]>>11)&31,(ref[x]>>5)&63,ref[x]&31};
                for(int k=0;k<3;k++) {
                    unsigned e=(unsigned)abs(c[k]-d2[k]);
                    if(e>step[k])step[k]=e;
                }
            }
        }
    }
    assert(step[0]<=1&&step[1]<=1&&step[2]<=1);
    assert(moved*100<total*12);
    printf("APPROX_OK: %u/%u pixels move (%.2f%%), max step r=%u g=%u b=%u\n",
           moved,total,100.0*moved/total,step[0],step[1],step[2]);
    // The dither: uniform over its four values, and uncorrelated with the
    // pixel to its left and the pixel above. A cheaper hash may replace the
    // one in garden.c, but not a worse one.
    unsigned bin[4]={0,0,0,0},same_h=0,same_v=0;
    for(int y=0;y<135;y++)for(int x=0;x<240;x++) {
        int d=garden_dither(x,y);
        assert(d>=0&&d<4);bin[d]++;
        if(x)same_h+=d==garden_dither(x-1,y);
        if(y)same_v+=d==garden_dither(x,y-1);
    }
    double expect=135*240/4.0,chi=0;
    for(int k=0;k<4;k++)chi+=(bin[k]-expect)*(bin[k]-expect)/expect;
    double ph=(double)same_h/(135*239),pv=(double)same_v/(134*240);
    double edge=(double)rough/(12.0*135*224);
    // 1.5131 on the reference. This catches a change in the field's amplitude
    // or its scale; it does NOT catch swapping the fine octave for a second
    // coarse one, which moves it 1.4% because the coarse octave already carries
    // three quarters of the weight. That much is below any cheap statistic and
    // is left to the eye.
    assert(edge>1.437&&edge<1.589);
    assert(chi<20);                       // 3 dof; ~11.3 is the 99% point
    assert(ph>0.235&&ph<0.265&&pv>0.235&&pv<0.265);
    // ---------------------------------------------------------------------
    // The swarm, as a thing that can fail.
    //
    // It is a look rather than a number, so what is checkable is the shape of
    // the look: that they are all in the beam, that they dart rather than
    // creep, that they pause, that they hold a volume instead of wandering off,
    // and that the swarm turns over occasionally rather than constantly or
    // never. Every one of those has been wrong at some point in getting here --
    // most memorably a hover that drew a speed of zero, which collided with
    // zero-speed meaning "never seeded" and quietly killed a particle every
    // sixty-fourth particle-frame.
    // ---------------------------------------------------------------------
    {
        GardenFrame g={0};
        // A zeroed frame is fourteen particles at (0,0), which is outside the
        // shaft. The cull is what seeds them, so this is the rule being used
        // for the thing it is for.
        garden_prepare(&g,10.0f);
        for(int i=0;i<GARDEN_MOTES;i++) {
            int my=g.mote[i].y>>4,c,h;
            assert(my>=0&&my<135);
            garden_shaft(my,&g,&c,&h);
            assert((g.mote[i].x>>4)>c-h&&(g.mote[i].x>>4)<c+h);
            assert(g.mote[i].speed>0);
        }
        const int F=1500;                       /* 60 s at 25 fps */
        unsigned visible=0,shafted=0,slow=0,moves=0,deaths=0,far=0;
        unsigned latched=0,unlatched=0,dimmed=0;
        double stepsum=0;
        int prev_age[GARDEN_MOTES]={0};
        int8_t psign[GARDEN_MOTES]={0};
        uint8_t pdim[GARDEN_MOTES]={0};
        int16_t px[GARDEN_MOTES]={0},py[GARDEN_MOTES]={0};
        for(int fr=0;fr<F;fr++) {
            garden_prepare(&g,10.0f+fr*0.04f);
            for(int i=0;i<GARDEN_MOTES;i++) {
                const GardenMote *m=&g.mote[i];
                if(fr&&(m->age<prev_age[i]||m->age==0))deaths++;
                else if(fr) {
                    int d=abs(m->x-px[i])+abs(m->y-py[i]);
                    moves++;stepsum+=d/16.0;
                    if(d<=4)slow++;             /* a quarter of a pixel: a hover */
                }
                // The latch, which is the one invariant here that is not a matter
                // of taste: the sign flips once and never back. A region test
                // would let a particle sitting on the line flicker, and this is
                // the assertion that would notice if one ever crept back in.
                if(psign[i]>0&&m->dir<0)latched++;
                if(psign[i]<0&&m->dir>0&&m->age>0)unlatched++;
                if(m->dir<0&&pdim[i]&&m->dim>pdim[i]&&m->age>0)dimmed++;
                psign[i]=m->dir;pdim[i]=m->dim;
                assert(m->dir!=0);              /* zero has no sign to read the latch from */
                prev_age[i]=m->age;px[i]=m->x;py[i]=m->y;
                int my=m->y>>4,c,h;
                assert(my>=0&&my<135);
                garden_shaft(my,&g,&c,&h);
                visible++;
                if((m->x>>4)>c-h&&(m->x>>4)<c+h)shafted++;
                int hc,hh;garden_shaft(m->hy>>4,&g,&hc,&hh);
                if(abs((m->x>>4)-(hc+m->hoff))+abs(my-(m->hy>>4))>45)far++;
            }
        }
        double step=stepsum/moves,hover=100.0*slow/moves;
        assert(shafted==visible);               /* never outside the light it needs */
        assert(step>0.5&&step<1.2);             /* darting, not creeping or teleporting */
        assert(hover>4&&hover<30);              /* it pauses, and not all the time */
        assert(far==0);                         /* holds a volume */
        // Turnover is bounded above and not below. A volume of thirty pixels
        // inside a shaft eighty wide means a particle rarely reaches the edge,
        // so zero deaths in a minute is the expected steady state and a stable
        // swarm is what a real one looks like. What must not happen is churn:
        // the cull firing often would mean particles are being replaced faster
        // than the eight-frame ramp can hide, and the swarm would flicker.
        assert(deaths*8<=F/25);                 /* at most one replacement per 8 s */
        assert(unlatched==0);                   /* one-way, and this is the whole of it */
        assert(dimmed==0);                      /* the fade only ever runs down */
        printf("SWARM_OK: %d in the shaft always; %.2f px/frame, hovering %.0f%% of"
               " frames, volume under 45 px; %u reached the bottom fifth and none"
               " came back from it; %u replacements in %d s\n",
               GARDEN_MOTES,step,hover,latched,deaths,F/25);
    }
    // The assertion this suite did not have, and its absence is why a feature
    // that never reached the eye passed everything: SWARM_OK proves the model
    // runs and CONTRACT_OK proves the scene did not change much, and "did not
    // change much" is indistinguishable from "did not appear".
    //
    // The scale to clear is the dither. It adds 0..3 to cr, cg and cb before
    // the shifts, so it moves any channel by at most one step of its own
    // width -- a mote that also moves a pixel by one step is competing with
    // the noise on equal terms and will read as noise. Counted in half-steps
    // of the 5-bit channels (green being six bits wide), the dither is 2 and a
    // mote has to beat it.
    //
    // A fifth is the bound, not a majority: a mote is brightest at its centre
    // and on the axis of the beam, and the ones at the edge SHOULD be faint --
    // that is what (glow*q)>>8 is for. What must not happen is that none of
    // them are bright.
    {
        unsigned clears=0;
        for(int k=3;k<8;k++)clears+=seen[k];   /* strictly more than the dither's 2 */
        assert(lit>200);                    /* they are drawing at all */
        assert(clears*5>lit);               /* and a fifth of it beats the dither */
    }
    printf("VISIBLE: %u pixels moved by a mote in 12 frames; steps"
           " 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7+:%u\n",
           lit,seen[1],seen[2],seen[3],seen[4],seen[5],seen[6],seen[7]);
#if GARDEN_MOTE_INDEX
    // The index is only allowed to be a faster way of asking the same question.
    // Checked against the question itself rather than against a second render,
    // because a second render would need a second build and a switch nobody
    // exercises is a switch that has stopped being true.
    //
    // Both directions matter. A missing bit drops a mote for a row -- a visible
    // flicker. A spurious bit costs nothing visible but is exactly how an index
    // silently degenerates back into the scan it was built to replace, so the
    // tightness is asserted too.
    {
        GardenFrame g=warm;
        for(int fr=0;fr<200;fr++) {
            garden_prepare(&g,fr*0.37f);
            for(int y=0;y<GARDEN_ROWS;y++) {
                unsigned want=0;
                for(int i=0;i<GARDEN_MOTES;i++) {
                    const GardenMote *m=&g.mote[i];
                    int top=m->y>>4,frac=m->y&15;
                    if(!m->glow)continue;
                    if(top==y||(top+1==y&&frac))want|=1u<<i;
                }
                assert(g.rowmask[y]==want);
            }
        }
        unsigned bits=0;
        for(int y=0;y<GARDEN_ROWS;y++)
            for(unsigned mk=g.rowmask[y];mk;mk&=mk-1)bits++;
        // 28 at most: fourteen particles across two rows each.
        assert(bits<=2*GARDEN_MOTES);
        printf("INDEX_OK: %u of %d row-particle tests survive a frame\n",
               bits,GARDEN_ROWS*GARDEN_MOTES);
    }
#endif
    printf("CONTRACT_OK: mean r=%.4f g=%.4f b=%.4f  light %d..%d/%d..%d/%d..%d"
           " exact, with motes %d..%d/%d..%d/%d..%d"
           "  edge=%.4f dither chi=%.2f P(left)=%.4f P(up)=%.4f\n",
           mr,mg,mb,qlo[0],qhi[0],qlo[1],qhi[1],qlo[2],qhi[2],
           lo[0],hi[0],lo[1],hi[1],lo[2],hi[2],edge,chi,ph,pv);
    // The frame is a parameter block that lives in the releasable flower
    // scene allocation. It grew from 16 bytes to 212 for the swarm and to 484
    // for the row index, and each of those was worth arguing about; a bound
    // here is what makes the next one an argument rather than a drift. Half a
    // kilobyte is the line: past that it is not parameters any more, and
    // whatever wants the space should be asking scene_mem for its own block.
    assert(sizeof(GardenFrame)<=512);
    printf("GARDEN_OK: changing=%u gradients=%u lit_left=%u rain_pixels=%u; frame parameters=%zu bytes; no persistent garden arrays\n",
           changing,gradient,lit_left,modified,sizeof(GardenFrame));
}
