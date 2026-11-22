/* Host test for the runtime character player.
 *   cc -O1 -I main -I main/hone tools/ayatsuri/model/test_track_character.c main/hone/track_model.c -o t
 *   ./t cloud.hca OUTDIR
 * Renders every face, every emote and a sweep of clip frames through the same
 * band entry points the firmware uses, writes PPM frames and a checksum list. */
#include "hone/track_character.h"
#include "ayatsuri/ayatsuri_live_model_fixture_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 240
#define H 135

/* Writable stand-ins for the build's const table, filled in from the command line. */
const ay_character_blob ay_character_blobs[1] __attribute__((section(".data"))) = {{0}};
const unsigned ay_character_blob_count __attribute__((section(".data"))) = 0;

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = aligned_alloc(16, (*size + 15) & ~15u);
    if (fread(buf, 1, *size, f) != *size) return NULL;
    fclose(f);
    return buf;
}

static uint32_t crc;
static uint16_t frame[W * H];

static int render(unsigned pose)
{
    unsigned packets; bool fast; uint32_t us;
    if (!ay_live_model_prepare(pose, &packets, &fast, &us)) return -1;
    memset(frame, 0, sizeof frame);
    for (unsigned y = 0; y < H; y += 8) {
        uint16_t band[W * 8];
        unsigned left, right;
        if (!ay_live_model_band(y, band, &left, &right)) return -2;
        unsigned rows = H - y < 8 ? H - y : 8;
        memcpy(&frame[y * W], band, rows * W * sizeof *band);
    }
    if (!ay_live_model_finish(true)) return -3;
    crc = 2166136261u;
    for (unsigned i = 0; i < W * H; ++i) { crc ^= frame[i] & 255; crc *= 16777619u; crc ^= frame[i] >> 8; crc *= 16777619u; }
    return (int)packets;
}

static void write_ppm(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (unsigned i = 0; i < W * H; ++i) {
        uint16_t p = frame[i];
        unsigned r = p >> 11, g = (p >> 5) & 63, b = p & 31;
        unsigned char px[3] = {(unsigned char)(r * 255 / 31), (unsigned char)(g * 255 / 63), (unsigned char)(b * 255 / 31)};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    size_t size;
    uint8_t *data = slurp(argv[1], &size);
    if (!data) { puts("cannot read asset"); return 2; }
    const char *why = ay_character_check(data, size);
    printf("check: %s\n", why ? why : "ok");
    if (why) return 1;
    /* The table is const in the library; this test swaps it through the symbol. */
    ((ay_character_blob *)ay_character_blobs)[0] = (ay_character_blob){"test", data, size};
    *(unsigned *)&ay_character_blob_count = 1;
    if (!ay_character_select(0)) { puts("select failed"); return 1; }
    printf("name=%s sha=%s poses=%u faces=%u emotes=%u clips=%u period=%u\n", ay_character_name(0), ay_character_sha256(),
           ay_character_pose_count(), ay_character_inspection_count(), ay_character_emote_count(),
           ay_character_clip_count(), ay_character_period_ms());
    unsigned faces = ay_character_inspection_count(), emotes = ay_character_emote_count();
    char name[64];
    for (unsigned f = 0; f < faces; ++f) {
        int n = render(f);
        printf("face %u %-10s packets=%d crc=%08x\n", f, ay_character_face_label(f), n, crc);
        snprintf(name, sizeof name, "face%u", f); write_ppm(argv[2], name);
    }
    for (unsigned e = 1; e <= emotes; ++e) {
        ay_character_set_emote(e);
        int n = render(0);
        printf("emote %u %-10s packets=%d crc=%08x\n", e, ay_character_emote_label(e), n, crc);
        snprintf(name, sizeof name, "emote%u", e); write_ppm(argv[2], name);
    }
    ay_character_set_emote(0);
    unsigned total = ay_character_pose_count();
    unsigned bad = 0;
    for (unsigned p = faces; p < total; ++p) {
        int n = render(p);
        if (n < 0) ++bad;
        if ((p - faces) % 21 == 0) {
            printf("frame %u packets=%d crc=%08x\n", p - faces, n, crc);
            snprintf(name, sizeof name, "frame%03u", p - faces); write_ppm(argv[2], name);
        }
    }
    /* a face chosen by a still pose must persist through the clip */
    render(3);
    render(faces + 10);
    printf("persist: face=%u (want 3)\n", ay_character_face());
    printf("bad_frames=%u\n", bad);
    ay_live_model_close();
    return bad ? 1 : 0;
}
