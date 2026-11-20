/* Export the actual serial renderer's 240x135 little-endian RGB565 frames. */
#include "ayatsuri/ayatsuri_live_model_fixture_api.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2) { fputs("usage: render_track FRAMES.rgb565\n", stderr); return 2; }
    FILE *out = fopen(argv[1], "wb");
    if (!out) return 1;
    uint16_t band[240 * 8], row[240];
    uint8_t bytes[240 * 2];
    puts("pose,triangles,nonzero_pixels,fnv1a565");
    for (unsigned pose = 0; pose < AY_LIVE_MODEL_POSE_COUNT; ++pose) {
        unsigned faces, count = 0; bool fast; uint32_t us, hash = UINT32_C(2166136261);
        if (!ay_live_model_prepare(pose, &faces, &fast, &us)) {
            fprintf(stderr, "prepare failed at pose %u\n", pose); return 1;
        }
        for (unsigned y = 0; y < 135; y += 8) {
            unsigned left, right;
            if (!ay_live_model_band(y, band, &left, &right)) return 1;
            for (unsigned r = 0; r < 8 && y + r < 135; ++r) {
                if (!ay_live_model_row(y + r, row) || memcmp(row, &band[r * 240], sizeof row)) return 1;
                for (unsigned x = 0; x < 240; ++x) {
                    if (row[x] && (x < left || x >= right)) return 1;
                    count += row[x] != 0;
                    hash = (hash ^ row[x]) * UINT32_C(16777619);
                    bytes[2*x] = row[x] & 255;
                    bytes[2*x + 1] = row[x] >> 8;
                }
                if (fwrite(bytes, 1, sizeof bytes, out) != sizeof bytes) return 1;
            }
        }
        printf("%u,%u,%u,%08" PRIx32 "\n", pose, faces, count, hash);
        if (!ay_live_model_finish(true)) return 1;
    }
    return ay_live_model_close() && fclose(out) == 0 ? 0 : 1;
}
