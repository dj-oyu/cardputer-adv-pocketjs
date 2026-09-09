// What a plant is, in this firmware.
//
// A species is a list of analytic ellipsoids, and everything below is either a
// primitive that emits one or a botanical that arranges them. There is no mesh
// and no stored geometry: `part` turns two endpoints into an ellipsoid, `stem`
// walks a bezier laying them end to end, and `bell`, `trumpet` and `cup` are
// the three flower heads that recur. flower.c takes the list from here and
// works out how to draw it; nothing in this file knows that it will be.
//
// To add a species: a row in flower_species_t (flower.h) and a branch in
// flower_build_botanicals. Keep to the primitives -- they are what the
// renderer's two shapes, ellipsoid and bell, are reachable through -- and stay
// under MAX_PARTS, which flower_prepare's block is sized for. The host test
// asserts 8 < count < MAX_PARTS for every species at every pose.
#include "flower_parts.h"

// Puffy shoulders taper into a tucked-in body and a softly turned open hem.
// Radii at the seven band boundaries: 0, .91, 1.10, 1.04, .93, .86, .90.
// Same six-band intersection as the ordinary bell; only 48 bytes of constants.
const float flower_cloche_slopes[FLOWER_BELL_BANDS]={2.73f,.57f,-.18f,-.33f,-.21f,.12f};
const float flower_cloche_offsets[FLOWER_BELL_BANDS]={2.73f,1.29f,1.04f,1.04f,1.00f,.78f};

static V cross(V a,V b) {return (V){a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
// yaw and pitch are fixed for the whole plant, so the four values derived from
// them are too. flower_build_botanicals sets them once; rotate() read them 124
// times a frame and recomputed all four every time -- 496 transcendental calls
// to produce four numbers, and on this part sinf and cosf are library routines
// with no hardware behind them.
//
// The parameters stay so that no call site has to change: this file gains
// species faster than it gains callers, and a signature change here would
// conflict with every one of them.
static float rot_c,rot_s,rot_cp,rot_sp;
static V rotate(V a,float yaw,float pitch) {
    (void)yaw;(void)pitch;
    float c=rot_c,s=rot_s,cp=rot_cp,sp=rot_sp;
    V b={c*a.x-s*a.y,s*a.x+c*a.y,a.z};
    return (V){b.x,cp*b.y-sp*b.z,sp*b.y+cp*b.z};
}
// Build long axes from endpoints, so stems, leaves and hanging petals share
// the analytic ellipsoid path. No extra mesh storage per botanical part.
static void part(V a,V b,float width,float thick,unsigned material,float yaw,float pitch) {
    if(count>=MAX_PARTS)return;
    Petal *p=&petals[count++];V d=add(b,mul(a,-1));float len=sqrtf(dot(d,d));
    p->c=rotate(mul(add(a,b),.5f),yaw,pitch);
    V u=normal(d),v=normal(cross(fabsf(u.z)>.9f?(V){0,1,0}:(V){0,0,1},u)),n=cross(u,v);
    p->axis[0]=rotate(u,yaw,pitch);p->axis[1]=rotate(v,yaw,pitch);p->axis[2]=rotate(n,yaw,pitch);
    p->radius[0]=fmaxf(len*.54f,.01f);p->radius[1]=width;p->radius[2]=thick;
    p->material=material;p->shape=0;
}
static V bezier(V a,V b,V c,float t) {return add(add(mul(a,(1-t)*(1-t)),mul(b,2*t*(1-t))),mul(c,t*t));}
static void stem(V a,V b,V c,int steps,float radius,float yaw,float pitch) {
    for(int i=0;i<steps;i++)part(bezier(a,b,c,(float)i/steps),bezier(a,b,c,(float)(i+1)/steps),radius,radius,LEAF,yaw,pitch);
}
static void bell(V top,float size,float lean,float yaw,float pitch) {
    if(count>=MAX_PARTS)return;
    Petal *p=&petals[count++];
    V down={sinf(lean),-cosf(lean),0},side={cosf(lean),sinf(lean),0};
    p->c=rotate(add(top,mul(down,size*.52f)),yaw,pitch);
    p->axis[0]=rotate(side,yaw,pitch);p->axis[1]=rotate(down,yaw,pitch);p->axis[2]=rotate((V){0,0,1},yaw,pitch);
    p->radius[0]=size*.36f;p->radius[1]=size*.52f;p->radius[2]=size*.36f;
    p->material=IVORY;p->shape=1;
}
static void trumpet(V root,V direction,float length,float radius,unsigned material,unsigned shape,float yaw,float pitch) {
    if(count>=MAX_PARTS)return;
    Petal *p=&petals[count++];V axis=normal(direction);
    V side=normal(cross(axis,fabsf(axis.z)>.9f?(V){0,1,0}:(V){0,0,1}));
    p->c=rotate(add(root,mul(axis,length*.5f)),yaw,pitch);
    p->axis[0]=rotate(side,yaw,pitch);p->axis[1]=rotate(axis,yaw,pitch);
    p->axis[2]=rotate(cross(side,axis),yaw,pitch);
    p->radius[0]=radius;p->radius[1]=length*.5f;p->radius[2]=radius;
    p->material=material;p->shape=shape;
}
// The amplitudes of the botanical animation, named rather than inline so that
// tools/flower_stale.c can price them. Raising them costs no frame time at all:
// ray_row is 30 ms of which almost everything is the 1,957 shaded pixels, and
// moving further does not shade more pixels. The only budget they spend is the
// interval a traced frame stays reusable, and that trade is direct -- speed
// times interval is about one pixel of screen displacement. The table in
// tools/flower_stale.c is that price list.
#ifndef FLOWER_SWAY
#define FLOWER_SWAY .09f
#endif
#ifndef FLOWER_BREATH
#define FLOWER_BREATH .055f
#endif
#ifndef FLOWER_CUP
#define FLOWER_CUP .055f
#endif
static void cup(V root,float size,unsigned material,float yaw,float pitch) {
    // Six tepals in two whorls: upright, overlapping ellipsoidal surfaces.
    for(int i=0;i<6;i++) {
        float a=i*PI/3+.25f,r=((material==VIOLET?.39f:.27f)+FLOWER_CUP*sinf(elapsed*.8f))*size;
        V bottom=add(root,(V){.045f*size*cosf(a),0,.045f*size*sinf(a)});
        V top=add(root,(V){r*cosf(a),size*(.94f+(i%2)*.045f),r*sinf(a)});
        part(bottom,top,size*.20f,size*.075f,material,yaw,pitch);
    }
}
void flower_build_botanicals(flower_species_t species,float yaw,float pitch) {
    // The only entry to rotate(), so the only place these have to be set. Every
    // caller of rotate -- part, bell, trumpet, and stem through part -- is
    // reachable only from here.
    rot_c=cosf(yaw);rot_s=sinf(yaw);rot_cp=cosf(pitch);rot_sp=sinf(pitch);
    count=0;
    float sway=FLOWER_SWAY*sinf(elapsed*.7f),breath=FLOWER_BREATH*sinf(elapsed*.8f);
    // Match garden.c's offscreen grass roots (screen y=142..149). The old
    // -1.35 ended near y=115, leaving each plant suspended above the ground.
    V base={-.18f,-2.2f,0};
    if(species==FLOWER_VALLEY) {
        stem(base,(V){-.55f,.5f,0},(V){.05f,1.2f,0},9,.024f,yaw,pitch);
        part(base,(V){-.9f,.3f,-.08f},.17f,.035f,LEAF,yaw,pitch);
        part(base,(V){.72f,-.2f,-.12f},.16f,.035f,LEAF,yaw,pitch);
        for(int i=0;i<5;i++) {
            float t=.95f-i*.145f;V root=bezier(base,(V){-.55f,.5f,0},(V){.05f,1.2f,0},t);
            float side=i%2?-1:1;
            V top=add(root,(V){side*(.29f+i*.025f)+sway,-.12f,.08f});
            stem(root,add(root,(V){side*.3f,.1f,.04f}),top,3,.014f,yaw,pitch);
            bell(top,.32f+i*.018f,side*.14f+sway,yaw,pitch);
        }
    } else if(species==FLOWER_SUNFLOWER) {
        V center={.08f,.43f,.03f};
        stem(base,(V){.04f,-.4f,0},center,7,.038f,yaw,pitch);
        part((V){-.1f,-.86f,0},(V){-.75f,-.34f,-.03f},.17f,.045f,LEAF,yaw,pitch);
        part((V){-.04f,-.64f,0},(V){.68f,-.16f,-.04f},.16f,.04f,LEAF,yaw,pitch);
        for(int i=0;i<26;i++) {
            float a=i*2*PI/26+.03f*sinf(elapsed*.3f),r=.82f+(i%2)*.08f+breath;
            V a0=add(center,(V){cosf(a)*.29f,sinf(a)*.29f,-.07f});
            V a1=add(center,(V){cosf(a)*r,sinf(a)*r,-.04f+.08f*cosf(a*3+elapsed*.4f)});
            part(a0,a1,.077f,.035f,GOLD,yaw,pitch);
        }
        part(add(center,(V){-.36f,0,.045f}),add(center,(V){.36f,0,.045f}),.385f,.13f,SEED,yaw,pitch);
    } else if(species==FLOWER_SNOWDROP) {
        // A nodding head and three separated outer tepals make the silhouette
        // distinct from a radial flower, even on the 1.14-inch display.
        V top={.25f,.53f,.08f};
        stem(base,(V){-.65f,1.72f,0},(V){.16f,1.03f,0},9,.025f,yaw,pitch);
        stem((V){.16f,1.03f,0},(V){.4f,1.01f,.03f},top,4,.025f,yaw,pitch);
        part(base,(V){-.69f,.15f,0},.057f,.025f,LEAF,yaw,pitch);
        part(base,(V){.43f,-.04f,-.1f},.052f,.025f,LEAF,yaw,pitch);
        part(add(top,(V){0,.06f,0}),add(top,(V){0,-.15f,0}),.12f,.10f,LEAF,yaw,pitch);
        for(int i=0;i<3;i++) {
            float a=i*2*PI/3+.2f;
            V start=add(top,(V){cosf(a)*.04f,-.12f,sinf(a)*.04f});
            V tip=add(top,(V){cosf(a)*(.47f+breath),-.98f,sinf(a)*.28f});
            part(start,tip,.132f,.048f,IVORY,yaw,pitch);
        }
        part(add(top,(V){0,-.15f,.03f}),add(top,(V){0,-.66f,.03f}),.14f,.13f,INNER,yaw,pitch);
    } else if(species==FLOWER_TULIP) {
        V head={.1f,.22f,0};
        stem(base,(V){.1f,-.5f,0},head,7,.034f,yaw,pitch);
        part(base,(V){-.66f,.08f,-.1f},.14f,.03f,LEAF,yaw,pitch);
        part((V){-.08f,-1,0},(V){.69f,-.2f,-.06f},.13f,.03f,LEAF,yaw,pitch);
        cup(head,1,ROSE,yaw,pitch);
    } else if(species==FLOWER_DAFFODIL) {
        V head={.08f,.53f,0};
        stem(base,(V){-.2f,.1f,0},head,7,.026f,yaw,pitch);
        for(int i=0;i<3;i++)part(base,(V){-.6f+i*.48f,.13f+i*.09f,-.13f},.055f,.025f,LEAF,yaw,pitch);
        for(int i=0;i<6;i++) {
            float a=i*PI/3+.2f;
            part(add(head,(V){.09f*cosf(a),.09f*sinf(a),0}),
                 add(head,(V){.73f*cosf(a),.73f*sinf(a),-.08f}),.18f,.045f,IVORY,yaw,pitch);
        }
        // A narrow neck feeds a broad flared mouth. Two overlapping shells
        // expose the spreading rim and its inner wall, rather than enlarging
        // the entire tube into a bulb. Both share the same directed light.
        V corona_root=add(head,(V){-.095f,-.035f,-.055f});
        V corona_axis=normal((V){.58f,.35f,1});
        trumpet(corona_root,corona_axis,.32f,.16f,CORONA,1,yaw,pitch);
        trumpet(add(corona_root,mul(corona_axis,.20f)),corona_axis,.38f,.35f,CORONA,1,yaw,pitch);
        // A recessed throat hides the stalk through the corona's tiny basal
        // opening. It sits behind the lip, so the mouth remains visibly deep.
        V throat=add(head,(V){-.056f,-.012f,.012f});
        part(add(throat,(V){-.115f,0,0}),add(throat,(V){.115f,0,0}),.125f,.01f,CORONA,yaw,pitch);
    } else if(species==FLOWER_CROCUS) {
        for(int i=0;i<3;i++) {
            V root={-.52f+i*.49f,-.66f+(i%2)*.27f,(i%2)*.12f};
            // The floral tube continues into the cup; tapering both pieces
            // to the same endpoint left a subpixel gap. Ground all three at
            // the common soil line, including the raised middle flower.
            part((V){root.x,base.y,root.z},add(root,(V){0,.16f,0}),.032f,.025f,LEAF,yaw,pitch);
            cup(root,.83f,VIOLET,yaw,pitch);
            for(int j=0;j<3;j++)part(add(root,(V){(j-1)*.035f,.24f,.015f}),
                add(root,(V){(j-1)*.065f,.72f,.015f}),.023f,.018f,GOLD,yaw,pitch);
        }
        for(int i=0;i<6;i++)part((V){-.3f+i*.12f,base.y,-.15f},
            (V){-.88f+i*.34f,-.2f+(i%3)*.15f,-.12f},.026f,.016f,LEAF,yaw,pitch);
    } else if(species==FLOWER_CALLA) {
        // The calla was the one species `sway` never reached: its head is a
        // fixed point and everything above it hangs off that, so the wind blew
        // through it. Moving the head moves the stem's top, the spathe and the
        // spadix together, which is the same nod the others already have.
        V head={.04f+sway,.02f,0};
        stem(base,(V){0,-.55f,0},head,7,.03f,yaw,pitch);
        part(base,(V){-.75f,-.25f,-.15f},.18f,.035f,LEAF,yaw,pitch);
        part(base,(V){.69f,-.45f,-.12f},.18f,.035f,LEAF,yaw,pitch);
        trumpet(head,(V){.15f,1,-.1f},1.15f,.4f,IVORY,2,yaw,pitch);
        part(add(head,(V){.015f,.17f,.055f}),add(head,(V){.08f,.96f,.04f}),.052f,.05f,GOLD,yaw,pitch);
    } else if(species==FLOWER_PLATYCODON) {
        // Platycodon grandiflorus: five pointed lobes and an inflated bud.
        V head={.18f+sway,.55f,0};
        stem(base,(V){-.22f,-.2f,0},head,7,.026f,yaw,pitch);
        for(int i=0;i<4;i++) {
            float y=-.95f+i*.22f,side=i%2?1:-1;
            part((V){-.12f,y,0},(V){side*.55f,y+.22f,-.08f},.085f,.025f,LEAF,yaw,pitch);
        }
        for(int i=0;i<5;i++) {
            float a=PI*.5f+i*2*PI/5;
            part(add(head,(V){.02f*cosf(a),.02f*sinf(a),-.045f}),
                 add(head,(V){.73f*cosf(a),.73f*sinf(a),.015f}),.22f,.06f,VIOLET,yaw,pitch);
        }
        part(add(head,(V){0,-.08f,.05f}),add(head,(V){0,.10f,.08f}),.08f,.035f,IVORY,yaw,pitch);
        V bud={-.58f,.46f,-.08f};
        stem((V){-.15f,-.3f,0},(V){-.65f,-.1f,0},bud,3,.019f,yaw,pitch);
        part(bud,add(bud,(V){0,.37f,0}),.17f,.15f,VIOLET,yaw,pitch);
    } else if(species==FLOWER_ECHINACEA) {
        // Echinacea purpurea: a raised seed cone above reflexed ray florets.
        // A brief breeze every 10*pi seconds, then rest. The squared envelope
        // starts and ends with zero velocity; the period closes at clock wrap.
        float phase=fmodf(elapsed,10*PI),breeze=0;
        if(phase<2*PI) {
            float envelope=sinf(phase*.5f);
            breeze=FLOWER_SWAY*.28f*sinf(phase)*envelope*envelope;
        }
        V head={.08f+breeze,.51f,.01f};
        stem(base,(V){-.04f,-.25f,0},head,7,.035f,yaw,pitch);
        part((V){-.12f,-.8f,0},(V){-.74f,-.33f,-.1f},.13f,.027f,LEAF,yaw,pitch);
        part((V){-.08f,-.56f,0},(V){.65f,-.12f,-.1f},.12f,.027f,LEAF,yaw,pitch);
        // One tilted flower head: the disk and the ray insertion ring share
        // an axis. Bury the ray roots inside its broad base, not under a ball.
        V axis={0,.94f,.341174f};
        for(int i=0;i<14;i++) {
            float a=i*2*PI/14;
            V radial={cosf(a),-.341174f*sinf(a),.94f*sinf(a)};
            V root=add(head,add(mul(radial,.20f),mul(axis,.035f)));
            V arch=add(head,add(mul(radial,.85f),mul(axis,.48f)));
            V tip=add(head,add(mul(radial,1.10f),mul(axis,-.36f)));
            // Lift out of the disk, open outward, then bend under the tip's
            // weight. Three overlapping chords preserve a visible arch rather
            // than the straight, downward spokes of a shuttlecock.
            for(int j=0;j<3;j++) {
                float t0=fmaxf(0,(float)j/3-.045f);
                float t1=fminf(1,(float)(j+1)/3+.045f);
                part(bezier(root,arch,tip,t0),bezier(root,arch,tip,t1),
                     j==1?.105f:.08f,.025f,ROSE,yaw,pitch);
            }
        }
        part(add(head,mul(axis,-.10f)),add(head,mul(axis,.28f)),
             .32f,.32f,SEED,yaw,pitch);
    } else if(species==FLOWER_ANEMONE) {
        // Anemone coronaria: broad scarlet sepals around dark stamens.
        V head={.08f+sway,.47f,0};
        stem(base,(V){-.3f,-.2f,0},head,7,.026f,yaw,pitch);
        for(int i=0;i<6;i++) {
            float a=i*2*PI/6;
            part((V){-.12f,-.58f,-.08f},(V){-.12f+.57f*cosf(a),-.58f+.26f*sinf(a),-.1f},.055f,.018f,LEAF,yaw,pitch);
        }
        for(int i=0;i<8;i++) {
            float a=i*2*PI/8+.2f;
            part(add(head,(V){.055f*cosf(a),.055f*sinf(a),-.06f}),
                 add(head,(V){.75f*cosf(a),.75f*sinf(a),-.01f}),.255f,.055f,RED,yaw,pitch);
        }
        part(add(head,(V){-.19f,0,.10f}),add(head,(V){.19f,0,.10f}),.21f,.075f,INK,yaw,pitch);
        for(int i=0;i<12;i++) {
            float a=i*2*PI/12;
            part(add(head,(V){.20f*cosf(a),.20f*sinf(a),.10f}),
                 add(head,(V){.29f*cosf(a),.29f*sinf(a),.08f}),.022f,.02f,INK,yaw,pitch);
        }
    } else if(species==FLOWER_NIGELLA) {
        // Nigella damascena, double form: petaloid sepals, erect styles and
        // divided bracts. The true petals are minute, not the blue structures.
        // Spend the 54-part budget on the lacy involucre and visible centre.
        // Keep the fine bracts together: a quiet 63-second drift, not a
        // separate twitch on each filament. .1 also closes at elapsed's wrap.
        float lace_sway=FLOWER_SWAY*.35f*sinf(elapsed*.1f);
        V head={.06f+lace_sway,.49f,0};
        stem(base,(V){-.05f,-.3f,0},head,4,.022f,yaw,pitch);
        // Five bracts fork around and partly in front of the flower, rather
        // than ten isolated radial needles behind it.
        for(int i=0;i<5;i++) {
            float a=.22f+i*2*PI/5;
            part(add(head,(V){0,-.12f,-.10f}),add(head,(V){.97f*cosf(a),.97f*sinf(a),.035f}),.016f,.013f,HERB,yaw,pitch);
            for(int j=0;j<3;j++) {
                float b=a+(j%2?-.22f:.24f),r=.72f+j*.13f,d=.35f+j*.13f;
                V fork=add(head,(V){d*cosf(a),d*sinf(a),-.02f});
                part(fork,add(head,(V){r*cosf(b),r*sinf(b),.09f}),.013f,.012f,HERB,yaw,pitch);
            }
        }
        for(int i=0;i<10;i++) {
            float a=PI*.5f+i*2*PI/5+(i>=5?.53f:0),r=i<5?.64f:.44f;
            float z=i<5?0:.055f;
            part(add(head,(V){.06f*cosf(a),.06f*sinf(a),z}),
                 add(head,(V){r*cosf(a),r*sinf(a),z+.025f}),i<5?.16f:.115f,.025f,BLUE,yaw,pitch);
        }
        part(add(head,(V){-.085f,0,.12f}),add(head,(V){.085f,0,.12f}),.10f,.065f,HERB,yaw,pitch);
        for(int i=0;i<5;i++) {
            float a=i*2*PI/5+.2f,c=cosf(a),s=sinf(a);
            // Five styles rise above the ovary.
            part(add(head,(V){.045f*c,.045f*s,.16f}),
                 add(head,(V){.14f*c,.18f+.11f*s,.27f}),.018f,.015f,HERB,yaw,pitch);
        }
        for(int i=0;i<8;i++) {
            float a=i*2*PI/8+.11f,c=cosf(a),s=sinf(a);
            part(add(head,(V){.07f*c,.07f*s,.15f}),
                 add(head,(V){.31f*c,.31f*s,.17f}),.02f,.015f,FILAMENT,yaw,pitch);
        }
        for(int i=0;i<2;i++) {
            float y=-1.04f+i*.40f,side=i%2?1:-1;
            V fork={side*.24f,y+.15f,-.03f};
            part((V){-.12f,y,0},fork,.018f,.013f,HERB,yaw,pitch);
            part(fork,(V){side*.55f,y+.35f,-.02f},.012f,.01f,HERB,yaw,pitch);
            part(fork,(V){side*.44f,y+.09f,-.02f},.012f,.01f,HERB,yaw,pitch);
        }
    } else if(species==FLOWER_AQUILEGIA) {
        // Aquilegia vulgaris: a nodding flower with five hooked nectar spurs.
        V head={.16f+sway,.65f,.03f};
        stem(base,(V){-.66f,1.25f,-.05f},head,9,.025f,yaw,pitch);
        for(int i=0;i<3;i++) {
            float x=-.58f+i*.4f;
            part((V){-.16f,-.9f,-.1f},(V){x,-.55f,-.1f},.12f,.028f,LEAF,yaw,pitch);
        }
        for(int i=0;i<5;i++) {
            float a=i*2*PI/5+.2f,c=cosf(a),s=sinf(a);
            V root=add(head,(V){.18f*c,-.09f,.16f*s});
            V knee=add(head,(V){.40f*c,.43f,.33f*s});
            part(root,knee,.055f,.04f,VIOLET,yaw,pitch);
            part(knee,add(head,(V){.21f*c,.51f,.20f*s}),.04f,.028f,VIOLET,yaw,pitch);
            part(root,add(head,(V){.69f*c,-.36f,.49f*s}),.12f,.035f,VIOLET,yaw,pitch);
            part(root,add(head,(V){.25f*c,-.55f,.22f*s}),.12f,.05f,VIOLET,yaw,pitch);
        }
        for(int i=0;i<3;i++)part(add(head,(V){(i-1)*.035f,-.35f,.15f}),
            add(head,(V){(i-1)*.05f,-.67f,.16f}),.014f,.014f,GOLD,yaw,pitch);
    } else if(species==FLOWER_FRITILLARIA) {
        // Fritillaria meleagris, not F. persica: two large chequered bells.
        stem(base,(V){-.31f,.4f,0},(V){-.1f,1.13f,0},9,.022f,yaw,pitch);
        for(int i=0;i<4;i++) {
            float y=-.9f+i*.31f,side=i%2?1:-1;
            part((V){-.2f,y,0},(V){side*.60f,y+.33f,-.08f},.037f,.019f,LEAF,yaw,pitch);
        }
        for(int i=0;i<2;i++) {
            float side=i?1:-1;V top={side*.46f+sway,.83f-i*.30f,.03f};
            stem((V){-.1f,.98f-i*.14f,0},(V){side*.52f,1.32f-i*.25f,0},top,4,.02f,yaw,pitch);
            trumpet(top,(V){side*.05f,-1,.08f},.76f,.31f,CHECKER,FLOWER_SHAPE_CLOCHE,yaw,pitch);
        }
    } else if(species==FLOWER_IRIS) {
        // Iris ensata: three broad falls with yellow signals, small standards.
        V head={.09f+sway,.64f,0};
        stem(base,(V){.05f,-.2f,0},head,8,.03f,yaw,pitch);
        for(int i=0;i<5;i++)part(add(base,(V){i*.08f-.15f,0,-.1f}),
            (V){-.62f+i*.28f,.12f+(i%3)*.19f,-.12f},.044f,.018f,LEAF,yaw,pitch);
        for(int i=0;i<3;i++) {
            float a=i*2*PI/3+PI*.5f,c=cosf(a),s=sinf(a);
            V knee=add(head,(V){.35f*c,.10f*s,.22f*s});
            V tip=add(head,(V){.86f*c,-.37f+.23f*s,.39f*s});
            part(head,knee,.14f,.04f,VIOLET,yaw,pitch);
            part(knee,tip,.26f,.045f,VIOLET,yaw,pitch);
            part(add(head,(V){.12f*c,-.015f+.05f*s,.08f+.10f*s}),
                 add(head,(V){.45f*c,-.12f+.12f*s,.10f+.23f*s}),.035f,.016f,GOLD,yaw,pitch);
            part(head,add(head,(V){.30f*c,.49f+.06f*s,.17f*s}),.085f,.035f,VIOLET,yaw,pitch);
        }
    }
}
