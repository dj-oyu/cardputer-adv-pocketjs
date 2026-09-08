#pragma once
// The shot list for the FLOWER scene: what the camera is looking at, and from
// where, for each cut. Data and constraints only -- no renderer, no state.
//
// It is a separate file so that composing the sequence is separable from making
// it draw. Everything below the line marked CONTRACT is what a person changing
// these numbers has to know, and it is all of it: nothing else in flower.c will
// surprise you, and nothing here needs a rebuild of anything else.
//
// ---------------------------------------------------------------------------
// CONTRACT -- the renderer's properties, learned the hard way today
// ---------------------------------------------------------------------------
//
// THE PROJECTION IS ORTHOGRAPHIC. screen_x = 180 + s*(wx-cx), screen_y = 65 -
// s*(wy-cy). There is no divide by z anywhere, so there is no perspective
// convergence at any angle. A low-angle shot and a three-point perspective are
// both convergence effects and CANNOT be produced by choosing angles here; past
// about twenty degrees a rotation stops reading as "the camera is below" and
// starts reading as "the object is tipped over". An orthographic camera is also
// infinitely far away by construction, which is the flatness that makes distant
// framings look cheap. A shear on dx by (1 + k*(y-y0)) would buy the
// convergence for one multiply and is not built; it changes ray geometry and
// wants its own change and its own measurement.
//
// THERE IS NO YAW. rotate() in flower_species.c mixes x and y -- the two SCREEN
// axes -- so its first parameter is a ROLL about the view direction. Driving it
// with viewpoint angles put the plant at 36, 87 and 142 degrees in the picture
// plane; eighty-seven degrees is a lily lying on its side. It is fixed at zero
// here and the test asserts that. A real turn around a standing plant needs x
// and z mixed, which is one line in flower_species.c -- the board owner's file.
// Until that exists, the horizontal half of a viewpoint is not available and
// these shots differ in height and framing only.
//
// PITCH mixes y and z, so for a stem standing along y with z near zero it is a
// foreshortening rather than a lean: the plant keeps its vertical and the camera
// changes height. Negative is the low angle (looking up); positive looks down.
// Derived from the rotation and the depth convention, not measured -- the
// obvious empirical check could not tell the signs apart, because in an
// orthographic projection there is nothing for it to measure.
//
// AIM is a fraction of the plant's own height, 0 at the base and 1 at the tip,
// not a screen row. The species differ in height by more than these shots
// differ from each other, so a row would be wrong on half of them.
//
// ZOOM <= 0 MEANS "THE MAGNIFICATION THAT FITS THIS PLANT", computed per
// species from its world bounding box with a margin, and never magnifying past
// 1.00. It comes out between 0.80 and 1.00 across the fourteen, so a constant
// crops thirteen of them: the size difference between species is larger than
// the difference between shots, which is the same reason aim is a fraction of
// the plant's height and not a row. A shot whose job is to show the whole plant
// has to ask for the fit rather than name a number. A fitted shot also ignores
// aim and centres on the plant, because a frame of the whole thing cannot also
// be aimed at part of it. Positive zooms keep their constants -- a close-up
// cropping is framing, not a fault.
//
// ZOOM multiplies the projection. The plant is measured in world units, so this
// is the camera walking in rather than the image being enlarged. At 2.60 the
// lily-of-the-valley was green blobs and three white dots -- a shot whose job is
// "this is the flower" has to still be the flower.
//
// CUTS, NOT PANS. Each shot is held, then the plant dissolves and the next is
// already framed. An animated translation or rotation inside a still scene read
// as cheap; the dissolve already existed for the species change and now carries
// both, so a view change and a plant change are the same event. Adding a shot
// lengthens the time a species is on screen: the species interval is
// FLOWER_ROTATE_S and each shot holds it divided by the shot count.
//
// WHAT IS CHECKED. tools/test_flower.c asserts every pitch within
// FLOWER_PITCH_MAX, yaw exactly zero, zoom in 1..4, aim in 0..1, that adjacent
// shots differ in size by more than sixteen percent INCLUDING ACROSS THE WRAP,
// that the dwells sum to FLOWER_ROTATE_S, and that the closest pair of shots
// differs in more than 300 pixels -- "N viewpoints" is a claim that can pass
// while every view looks the same, and "a size ladder" is one that can pass
// while four of five shots are the same size.
// ---------------------------------------------------------------------------
#define FLOWER_PITCH_MAX 0.349f          /* twenty degrees */
// ONE FLOWER, ONE CUT. There is no cutting inside a species: the shot is
// chosen once and held for the whole interval, and the only dissolve is the one
// that changes the plant.
//
// WHY, and it is a cost reason rather than a taste one. Zoom walks the camera
// in, so a higher rung covers more pixels, and every covered pixel pays shade()
// and on bell species bell() as well. Measured on hardware once the framing
// actually reached the projection: the 2.35 rung took one species from the
// high twenties down to 17.3 fps. Nobody had seen this before because zoom was
// disconnected until the day this was written -- the cost arrived with the fix,
// not with the ladder. A ladder of four rungs means paying the worst rung for a
// quarter of every species' time, and paying it as a visible drop.
//
// SO THE LADDER IS GONE, AND WITH IT THE REASON FOR MOST OF THIS FILE'S RULES.
// Shot size, adjacent-size steps, the order of cuts, the dwell split: all of it
// was about how one cut reads against the next, and there is no next. What
// survives is the single question of how to frame a plant, and the constraints
// in the contract above, which are properties of the renderer and did not
// change.
//
// WHAT REPLACES IT, IF ANYTHING. A single shot may still differ BETWEEN
// species -- a sunflower and a lily-of-the-valley need not be framed alike, and
// with no intra-species cut there is nothing to pay for the difference. That
// would be a per-species table rather than a per-shot one, and whether it is
// wanted is a question for whoever is looking at the sheets.
//
// WHAT THE COST ACTUALLY LOOKS LIKE, from tools/flower_shot_cost.sh, in pixels
// the plant covers -- the quantity that drives the per-pixel work, exact and
// deterministic, unlike a host frame time:
//
//   rung   1.00    1.28    1.55    2.35        mean over all fourteen species
//          2094    3221    4348    5514
//
// It is NOT a square law, because a tighter frame crops the plant as it
// enlarges it. Lily-of-the-valley covers 2356 pixels at 1.00 and only 1891 at
// 2.35; snowdrop and calla are also flat or cheaper at the top rung. The cost
// is concentrated in the wide-headed species -- anemone 2365 to 8733, sunflower
// 3171 to 8442, crocus 2955 to 8331. A per-species table is therefore not
// uniformly more expensive than a fixed 1.00; it is more expensive exactly
// where a big head is enlarged.
//
// AND THE HOST DOES NOT RANK THE SPECIES THE WAY THE DEVICE DOES. Both host
// metrics put sunflower and anemone worst at 2.35; the board's worst was
// echinacea at 17.3 fps, which is mid-pack in coverage. Per-pixel cost varies
// by species, not just pixel count. Use the host numbers for the shape of the
// curve across rungs and measure the chosen rung on hardware.
//
// DWELL is the whole interval now. The sum still has to equal FLOWER_ROTATE_S
// -- that is what the species swap and ROTATION_OK's swap count are measured
// against -- and with one entry the sum IS the entry.
typedef struct { float pitch,zoom,aim,hold; } flower_shot_t;
static const flower_shot_t FLOWER_SHOTS[]={
    // The whole plant, near level, fitted to the window per species. The aim is
    // carried but unused: a fitted shot centres on the plant.
    { -0.06f, 0.00f, 0.50f, 40.0f },
};
#define FLOWER_VIEWS ((int)(sizeof FLOWER_SHOTS/sizeof FLOWER_SHOTS[0]))
// The shortest dwell, which is what a bound on "how much of the time is spent
// dissolving" has to be taken against. With one shot it is the interval.
#define FLOWER_VIEW_MIN_S 40.0f
