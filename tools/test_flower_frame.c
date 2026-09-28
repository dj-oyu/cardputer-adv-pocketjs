/* Prepared FLOWER frames must survive later preparation and scene eviction.
 * gcc -O2 -Wall -Wextra -Werror -Imain/scene -Itools/hostshim \
 *   tools/test_flower_frame.c main/scene/canopy_pie.c \
 *   main/scene/garden_decor_pie.c -lm -o .cache/test_flower_frame
 */
#include "../main/scene/fxmath.c"
#include "../main/scene/scene_mem.c"
#include "../main/scene/garden.c"
#include "../main/scene/flower.c"
#include "../main/scene/flower_species.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t expected[2][W * H], actual[W * H], strips[W * H];
static const uint32_t species_hashes[FLOWER_SPECIES_COUNT] = {
    0x1069ac08u, 0x117d7635u, 0xe23e8db2u, 0x99d1d105u,
    0x693c4f95u, 0xadfb3f4fu, 0xf4467ec8u, 0xa18c6b44u,
    0x659da3b5u, 0xe7cc7f24u, 0xdb653294u, 0x0abe74ddu,
    0xb15809f9u, 0x0d478f74u,
};
static const uint32_t device_species_hashes[FLOWER_SPECIES_COUNT] = {
    0x1069ac08u, 0x36baae55u, 0xe23e8db2u, 0x52d3a32du,
    0x693c4f95u, 0xf9ee9882u, 0xf4467ec8u, 0xb586b2a8u,
    0x659da3b5u, 0xf7d7eefcu, 0xdb653294u, 0xb76c93c9u,
    0xb15809f9u, 0x3a3c41b4u,
};

static uint32_t pixel_hash(const uint16_t *pixels)
{
    const uint8_t *bytes = (const uint8_t *)pixels;
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < sizeof expected[0]; ++i)
        hash = (hash ^ bytes[i]) * UINT32_C(16777619);
    return hash;
}

static uint32_t frame_hash(const flower_frame *frame)
{
    size_t bytes = (size_t)frame->state.parts * sizeof(Petal) +
                   32u * 32u + sizeof(GardenFrame);
    const uint8_t *data = flower_frame_scene(frame);
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < bytes; ++i)
        hash = (hash ^ data[i]) * UINT32_C(16777619);
    return hash;
}

static void check_lease(flower_frame *frame, const uint16_t *reference)
{
    uint32_t before = frame_hash(frame);
    flower_frame_draw(frame, actual, 0, H);
    assert(!memcmp(actual, reference, sizeof actual));
    for (int bottom = H; bottom > 0;) {
        int height = bottom < 8 ? bottom : 8;
        bottom -= height;
        flower_frame_draw(frame, strips + bottom * W, bottom, height);
    }
    assert(!memcmp(strips, reference, sizeof strips));
    flower_frame_draw(frame, actual, 0, H);
    assert(!memcmp(actual, reference, sizeof actual));
    assert(frame_hash(frame) == before);
}

int main(int argc, char **argv)
{
    assert(argc == 1 || argc == 2);
    flower_frame *frames[2] = {flower_frame_create(), flower_frame_create()};
    assert(frames[0] && frames[1]);
    assert(!flower_frame_capture(NULL));
    flower_frame_draw(frames[0], actual, 0, H);
    for (int species = 0; species < FLOWER_SPECIES_COUNT; species += 2) {
        for (int i = 0; i < 2; ++i) {
            elapsed = 3.5f + (float)species;
            flower_grain_frame = (unsigned)(species * 13 + i);
            flower_prepare(.033f, 0, 0,
                           (flower_species_t)(species + i));
            flower_draw(expected[i], 0, H);
            for (int y = 0; y < H; y += 8) {
                int height = H - y < 8 ? H - y : 8;
                flower_draw(actual + y * W, y, height);
            }
            assert(!memcmp(actual, expected[i], sizeof actual));
            assert(pixel_hash(expected[i]) == species_hashes[species + i]);
            printf("FLOWER_SPECIES_HOST species=%d hash=%08x\n", species + i,
                   pixel_hash(expected[i]));
            if (argc == 2) {
                char path[512];
                assert(snprintf(path, sizeof path, "%s/%02d.rgb565", argv[1],
                                species + i) < (int)sizeof path);
                FILE *output = fopen(path, "wb");
                assert(output);
                assert(fwrite(expected[i], sizeof expected[i], 1, output) == 1);
                assert(fclose(output) == 0);
            }
            assert(flower_frame_capture(frames[i]));
        }
        /* Drawing the old lease must leave the currently prepared scene live. */
        check_lease(frames[0], expected[0]);
        flower_draw(actual, 0, H);
        assert(!memcmp(actual, expected[1], sizeof actual));
        scene_mem_release();
        check_lease(frames[0], expected[0]);
        check_lease(frames[1], expected[1]);
    }
    /* A rotating frame also holds its camera, dissolve, weather and grain. */
    for (int i = 0; i < 12; ++i) {
        flower_prepare_rotating(.1f, 0, 0);
        flower_draw(expected[0], 0, H);
        assert(flower_frame_capture(frames[0]));
        flower_prepare_rotating(.1f, 0, 0);
        scene_mem_release();
        check_lease(frames[0], expected[0]);
    }
    /* Device's named-shot probe evicts scene_mem before each species and then
     * holds that one prepared frame for display. Keep these independent hashes
     * separate from the paired lease/transition fixture above. */
    for (int species = 0; species < FLOWER_SPECIES_COUNT; ++species) {
        scene_mem_release();
        elapsed = 3.5f + (float)(species & ~1);
        flower_grain_frame = (unsigned)((species & ~1) * 13 + (species & 1));
        flower_prepare(.033f, 0, 0, (flower_species_t)species);
        flower_draw(expected[0], 0, H);
        assert(pixel_hash(expected[0]) == device_species_hashes[species]);
        printf("FLOWER_SPECIES_DEVICE_REF species=%d hash=%08x\n", species,
               pixel_hash(expected[0]));
    }
    printf("flower frame lease passed; bytes=%zu+%zu for two frames\n",
           flower_frame_bytes(frames[0]), flower_frame_bytes(frames[1]));
    flower_frame_destroy(frames[0]);
    flower_frame_destroy(frames[1]);
    scene_mem_release();
    return 0;
}
