#include "../main/scene/garden.c"
#include "../main/scene/glass_rain.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
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
        const int G=GARDEN_GLOW_MAX;(void)G;
        assert(lo[0]==qlo[0]&&lo[1]==qlo[1]&&lo[2]==qlo[2]);   /* only ever adds */
#if GARDEN_NO_MOTES
        // The build with the swarm removed. "The motes add light" cannot hold
        // where there are none, and the useful assertion is its opposite: with
        // them gone the frame must be EXACTLY the light, or something other
        // than the swarm is drawing into the shaft.
        assert(hi[0]==qhi[0]&&hi[1]==qhi[1]&&hi[2]==qhi[2]);
#else
        assert(hi[0]>qhi[0]&&hi[0]<=qhi[0]+(((5*G)>>1)>>3)+1);
        assert(hi[1]>qhi[1]&&hi[1]<=qhi[1]+(((6*G)>>2)>>2)+1);
        assert(hi[2]>=qhi[2]&&hi[2]<=qhi[2]+((G/3)>>3)+1);
#endif
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
#if !GARDEN_NO_MOTES
    // Everything from here to CONTRACT_OK is about the swarm, so a build
    // with the swarm removed skips it rather than failing it. That build is
    // a measurement -- it is how the feature is priced -- so it is worth
    // having the rest of this file still run in it.
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
        // A zeroed frame is sixteen sleeping particles, and the swarm now
        // ASSEMBLES rather than appearing: the population is what a birth rate
        // and a capacity leave behind, and births are one a frame with a chance
        // that grows with the deficit. Sixteen frames is about two thirds of a
        // second, so this is the boot behaviour and not a warm-up hidden in a
        // test -- but it does mean "the first frame" is no longer when the
        // swarm exists, and asserting there would only measure the birth rate.
        for(int w=0;w<40;w++)garden_prepare(&g,10.0f+w*0.04f);
        int awake=0;
        for(int i=0;i<GARDEN_MOTES;i++) {
            if(!g.mote[i].speed)continue;
            awake++;
            int my=g.mote[i].y>>4,c,h;
            assert(my>=0&&my<135);
            garden_shaft(my,&g,&c,&h);
            assert((g.mote[i].x>>4)>c-h&&(g.mote[i].x>>4)<c+h);
        }
        assert(awake>=GARDEN_LIVE_LO&&awake<=GARDEN_MOTES);
        // Two minutes, not one. The dying latch fires about once every fifteen
        // seconds now that cohesion keeps the swarm together, and a sixty-second
        // window caught a quiet stretch and reported zero -- a feature that
        // works looking exactly like a feature that has become unreachable.
        // The window is part of the assertion, not an arbitrary length.
        const int F=3000;                       /* 120 s at 25 fps */
        unsigned visible=0,shafted=0,slow=0,moves=0,deaths=0,far=0;
        unsigned latched=0,unlatched=0,dimmed=0;
        // The mean the crossings are measured against. A constant rather than
        // the run's own mean, so the test does not move with the thing it is
        // testing -- and it is the one number here taken from a measurement
        // rather than derived, so it is written where that is obvious.
        #define GARDEN_DIA_REF 59
        long diasum=0,offsum=0;int dia_lo=32767,dia_hi=0,off_hi=0;
        int prev_dia=0,last_cross=-1,crossn=0;double gaps=0,gap2=0;
        // The population, which now varies. Counted as a distribution rather
        // than a number, because "a bell with the mode at 60%" is a claim about
        // a shape and only a histogram can fail it.
        long livehist[GARDEN_MOTES+1]={0};
        unsigned livechg=0,empty=0,retired=0,doomed=0;
        // Events as the frame itself reports them, rather than inferred from
        // the state afterwards. garden_prepare counts what it did; three
        // separate reconstructions of that in this file have each been wrong
        // about a denominator, and this one cannot be.
        unsigned births=0,dies=0,deathn=0;
        double deathgap=0,deathgap2=0;int last_death=-1;
        int prev_live=-1;
        uint8_t pspeed[GARDEN_MOTES]={0};
        double stepsum=0;
        int prev_age[GARDEN_MOTES]={0};
        int8_t psign[GARDEN_MOTES]={0};
        uint8_t pdim[GARDEN_MOTES]={0};
        int16_t px[GARDEN_MOTES]={0},py[GARDEN_MOTES]={0};
        for(int fr=0;fr<F;fr++) {
            garden_prepare(&g,10.0f+fr*0.04f);
            births+=g.born;dies+=g.died;
            if(g.died) {
                if(last_death>=0) {
                    double gp=fr-last_death;
                    deathn++;deathgap+=gp;deathgap2+=gp*gp;
                }
                last_death=fr;
            }
            for(int i=0;i<GARDEN_MOTES;i++) {
                const GardenMote *m=&g.mote[i];
                // A replacement, and only a replacement. A mote that went to
                // sleep because the population shrank, or woke because it grew,
                // also resets its age -- and counting those as churn would have
                // made the turnover bound fail for a reason that is the feature
                // working. The distinguishing fact is that a cull happens while
                // the mote is awake on both sides of it.
                if(fr&&pspeed[i]&&m->speed&&(m->age<prev_age[i]||m->age==0))deaths++;
                else if(fr&&pspeed[i]&&m->speed) {
                    int d=abs(m->x-px[i])+abs(m->y-py[i]);
                    moves++;stepsum+=d/16.0;
                    if(d<=4)slow++;             /* a quarter of a pixel: a hover */
                }
                // The latch, which is the one invariant here that is not a matter
                // of taste: the sign flips once and never back. A region test
                // would let a particle sitting on the line flicker, and this is
                // the assertion that would notice if one ever crept back in.
                // The latch fires for two different reasons now, and they are
                // not the same feature: crossing into the bottom fifth, and
                // being told to retire because the swarm shrank. Both use the
                // same fade on purpose; the test has to tell them apart or the
                // assertion that the bottom fifth is still reachable would pass
                // on retirements alone.
                if(psign[i]>0&&m->dir<0) {
                    latched++;
                    if((m->y>>4)>GARDEN_DOOM)doomed++;else retired++;
                }
                if(psign[i]<0&&m->dir>0&&m->age>0)unlatched++;
                if(m->dir<0&&pdim[i]&&m->dim>pdim[i]&&m->age>0)dimmed++;
                psign[i]=m->dir;pdim[i]=m->dim;pspeed[i]=m->speed;
                prev_age[i]=m->age;px[i]=m->x;py[i]=m->y;
                if(!m->speed)continue;          /* asleep: not on the glass */
                // Scoped to the awake, and that scope is the invariant. A
                // sleeping mote has never been seeded and its dir is still the
                // zero the frame was allocated with; an awake one must have a
                // sign, because the sign is where the dying latch lives.
                assert(m->dir!=0);
                int my=m->y>>4,c,h;
                assert(my>=0&&my<135);
                garden_shaft(my,&g,&c,&h);
                visible++;
                if((m->x>>4)>c-h&&(m->x>>4)<c+h)shafted++;
            }
            // The swarm as a body, measured every frame rather than at the end.
            // Cohesion has exactly two failure modes and they are opposite:
            // sixteen things pulled to one point become one point, and a gain
            // too weak lets them disperse. Both show up as "the diameter
            // changed", so the diameter needs a floor as well as a ceiling.
            {
                int lo_x=32767,hi_x=-32768,lo_y=32767,hi_y=-32768,ax=0,ay=0,nl=0;
                for(int i=0;i<GARDEN_MOTES;i++) {
                    if(!g.mote[i].speed)continue;
                    int mx=g.mote[i].x>>4,my2=g.mote[i].y>>4;
                    if(mx<lo_x)lo_x=mx;
                    if(mx>hi_x)hi_x=mx;
                    if(my2<lo_y)lo_y=my2;
                    if(my2>hi_y)hi_y=my2;
                    ax+=mx;ay+=my2;nl++;
                }
                livehist[nl]++;
                if(!nl){empty++;prev_live=nl;prev_dia=0;continue;}
                if(prev_live>=0&&nl!=prev_live)livechg++;
                prev_live=nl;
                int dia=(hi_x-lo_x)+(hi_y-lo_y);
                diasum+=dia;
                // Breathing. A cohesion loop that oscillates has a REGULAR
                // period, so the test is not the variance of the diameter but
                // the regularity of its crossings of its own mean: a swarm
                // driven by noise crosses at irregular intervals, one that is
                // ringing crosses like a metronome. Measured against a mean
                // from the previous run of the same 1500 frames, which is why
                // it is a second pass rather than an accumulator.
                if(fr) {
                    if((prev_dia<GARDEN_DIA_REF)!=(dia<GARDEN_DIA_REF)) {
                        if(last_cross>=0) {
                            double gapv=fr-last_cross;
                            crossn++;gaps+=gapv;gap2+=gapv*gapv;
                        }
                        last_cross=fr;
                    }
                }
                prev_dia=dia;
                if(dia<dia_lo)dia_lo=dia;
                if(dia>dia_hi)dia_hi=dia;
                // Where the body sits, so phototaxis is checked as a statement
                // about the group rather than about any one mote.
                int ccx=ax/nl,ccy=ay/nl,sc,sh2;
                garden_shaft(ccy,&g,&sc,&sh2);
                offsum+=abs(ccx-sc);
                if(abs(ccx-sc)>off_hi)off_hi=abs(ccx-sc);
            }
        }
        double step=stepsum/moves,hover=100.0*slow/moves;
        double dia=(double)diasum/F,off=(double)offsum/F;
        int live_mode=0,live_lo=0,live_hi=0;double live_mean=0;
        double death_gap=0,death_spread=0;
        assert(shafted==visible);               /* never outside the light it needs */
        // Widened from 1.2, and for a reason rather than to fit: the shared
        // surge exists to make the swarm dash together, so the mean step MUST
        // rise when it fires. The ceiling still has to catch teleportation --
        // a mote three pixels across that moves more than about two pixels a
        // frame stops overlapping itself and strobes.
        assert(step>0.5&&step<2.0);             /* darting, not creeping or strobing */
        assert(hover>4&&hover<30);              /* it pauses, and not all the time */
        (void)far;
        // Cohesion. The swarm holds a body: never a point, never the screen.
        // The floor is the one that catches the failure everybody gets -- a
        // dead zone too small, or a gain too strong, and sixteen midges become
        // one bright dot.
        assert(dia_lo>10);                      /* never collapses to a point */
        assert(dia_hi<150);                     /* never disperses */
        assert(dia>25&&dia<110);                /* and holds a size on average */
        // Phototaxis, as a statement about the group. The centroid lives near
        // the axis rather than at the edge of the beam, which is the whole of
        // what "toward the light" means when q peaks on the axis.
        //
        // The worst-case bound is 60 and not 45, and the reason is population
        // rather than tuning: the centroid of a smaller swarm is a noisier
        // estimate and wanders further. Measured over fifteen minutes, the
        // worst excursion is 16 px when thirteen are awake and 45 when seven
        // are -- so a bound set from a sixteen-mote run was a bound that would
        // fail the first time the swarm thinned, which is now every few
        // seconds. The mean is unaffected and stays where it was.
        assert(off<20&&off_hi<60);
        // The population, as a shape. The mode is the thing that was asked for
        // -- 60% of the array -- and the floor is the thing that would be
        // noticed if it were wrong: a beam with nothing in it reads as a broken
        // feature, not as variation, so no frame may be empty and none may fall
        // below the floor the header sets.
        {
            int mode=0,seen=0,lo_n=GARDEN_MOTES,hi_n=0;
            double meanl=0;
            for(int i=0;i<=GARDEN_MOTES;i++) {
                if(livehist[i]>livehist[mode])mode=i;
                if(livehist[i]){seen++;if(i<lo_n)lo_n=i;if(i>hi_n)hi_n=i;}
                meanl+=(double)i*livehist[i];
            }
            meanl/=F;
            assert(empty==0);                   /* the beam is never bare */
            assert(lo_n>=GARDEN_LIVE_LO);       /* the floor is the floor */
            assert(hi_n<=GARDEN_MOTES);
            assert(mode==GARDEN_LIVE_MODE);     /* the mode is what was asked for */
            assert(meanl>GARDEN_LIVE_MODE-2&&meanl<GARDEN_LIVE_MODE+2);
#if GARDEN_LIVE_FIXED
            // The switch pins the CAP, and the cap is a ceiling rather than a
            // count: a death drops the population below it and a birth brings
            // it back, so even pinned it breathes by a mote or two. That is not
            // a weaker version of the assertion, it is the correct one -- the
            // population has not been a number anybody sets since it became
            // what two rates leave behind.
            // And it breathes UPWARD, which is worth stating because it is
            // the opposite of what it looks like it should do. The capacity
            // rule does not count a dying mote, so the replacement is born
            // while the old one is still fading and the number of things on
            // the glass is the cap plus however many fades are running. That
            // overlap is deliberate -- it is what stops the swarm thinning
            // visibly every time one leaves -- and this is where it shows.
            assert(lo_n>=GARDEN_LIVE_MODE&&hi_n<=GARDEN_LIVE_MODE+3);
#else
            assert(seen>=5);                    /* a distribution, not a constant */
            // The population changes often now and that is the feature: it is
            // what two rates against a drifting cap leave behind, not a number
            // anybody sets. So the rate gets a loose bound and the REGULARITY
            // gets a tight one -- a birth rule that was smooth would make the
            // deaths a metronome, and the trace of them a clock. Same statistic
            // as the breathing test, for the same reason.
            assert(livechg>20&&livechg<(unsigned)F/2);
            double bm=deathn?deathgap/deathn:0;
            double bs=deathn?sqrt(deathgap2/deathn-bm*bm):0;
            assert(deathn>20&&bm>0&&bs/bm>0.4);
            death_gap=bm;death_spread=bm?bs/bm:0;
#endif
            live_mode=mode;live_lo=lo_n;live_hi=hi_n;live_mean=meanl;
        }
        // Not breathing. The interval between crossings has to be irregular:
        // a ringing swarm gives a metronome, and its spread over its mean
        // collapses. Measured at 1.2 here; 0.4 is the line.
        {
            double gm=crossn?gaps/crossn:0;
            double gs=crossn?sqrt(gap2/crossn-gm*gm):0;
            assert(crossn>50&&gm>0&&gs/gm>0.4);
        }
        // Turnover used to be bounded above at one replacement per eight
        // seconds, because a swarm held together by cohesion almost never lost
        // one to the shaft's edge and churn was the only failure available.
        // The capacity rule makes turnover the mechanism rather than an
        // accident, so the bound has to move -- but the reason it existed does
        // not: a replacement is hidden by an eight-frame ramp and a death by a
        // second-long fade, so the two together set how fast the swarm can turn
        // over without popping. One event every eight frames is that limit.
        assert(dies*8<(unsigned)F);
        assert(births*8<(unsigned)F);
#if !GARDEN_LIVE_FIXED
        // The floor belongs to the free-cap build only. With the cap pinned the
        // only deaths left are the shaft and the bottom fifth, and those were
        // always rare -- which is itself the measurement: nearly all of the
        // turnover is the capacity rule, and pinning it takes the swarm back to
        // one death every fifteen seconds.
        assert(dies>10&&births>10);
#endif
        // Births and deaths have to balance over a window this long, or the
        // population is not being held by a capacity at all -- it is drifting,
        // and the drift would be invisible until the swarm emptied or filled.
        assert(births+GARDEN_MOTES>=dies&&dies+GARDEN_MOTES>=births);
        (void)deaths;
        // The latch must still be reachable. Cohesion pulls the swarm toward an
        // anchor well above the dying line, and a slightly stronger pull would
        // make the bottom-fifth fade dead code that still passes every test
        // above -- which is the failure VISIBLE was written for, in a different
        // place.
        assert(doomed>0);
        assert(unlatched==0);                   /* one-way, and this is the whole of it */
        assert(dimmed==0);                      /* the fade only ever runs down */
        printf("SWARM_OK: %d..%d awake (mode %d, mean %.1f) of %d; %.2f px/frame,"
               " hovering %.0f%% of frames; diameter %.0f px (%d..%d); centroid"
               " %.1f px off the axis (worst %d); %u births %u deaths in %d s"
               " (%u to the bottom fifth, %u over capacity or out of the beam;"
               " one every %.1f s, spread %.2f of the mean)\n",
               live_lo,live_hi,live_mode,live_mean,GARDEN_MOTES,step,hover,
               dia,dia_lo,dia_hi,off,off_hi,births,dies,F/25,doomed,retired,
               death_gap/25.0,death_spread);
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
#endif
    printf("CONTRACT_OK: mean r=%.4f g=%.4f b=%.4f  light %d..%d/%d..%d/%d..%d"
           " exact, with motes %d..%d/%d..%d/%d..%d"
           "  edge=%.4f dither chi=%.2f P(left)=%.4f P(up)=%.4f\n",
           mr,mg,mb,qlo[0],qhi[0],qlo[1],qhi[1],qlo[2],qhi[2],
           lo[0],hi[0],lo[1],hi[1],lo[2],hi[2],edge,chi,ph,pv);
    // The frame is a parameter block that lives in the releasable flower scene
    // allocation. It grew from 16 bytes to 212 for the swarm, to 484 for the
    // row index, and back to 448 when the per-mote home went.
    //
    // The bound has fired once, for a trace of the swarm's births and deaths
    // whose ring pushed it past 512. That was argued, raised to 640, and then
    // the trace was measured at 3.3 to 4.0 ms and removed -- so the bound is
    // back where it was, and the episode is worth one line: the argument for
    // raising it was sound and the feature still did not survive contact with
    // a frame counter. A bound is not what decides whether something belongs.
    assert(sizeof(GardenFrame)<=512);
    printf("GARDEN_OK: changing=%u gradients=%u lit_left=%u rain_pixels=%u; frame parameters=%zu bytes; no persistent garden arrays\n",
           changing,gradient,lit_left,modified,sizeof(GardenFrame));
}
