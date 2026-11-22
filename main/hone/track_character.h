#ifndef HONE_TRACK_CHARACTER_H
#define HONE_TRACK_CHARACTER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Runtime-selectable characters for the track player.
 *
 * A character is a Hone character asset ("HCA1", written by
 * tools/ayatsuri/asset/export_character_track.py) or, when the firmware is
 * built with the exported fox header, that compile-time table. All of them
 * share one pose index space so the Companion driver does not care which one
 * it is showing:
 *   [0, inspection)           one still pose per face preset
 *   [inspection, pose_count)  the frames of the selected clip
 * Face and emote are player state, not baked into the clip: a preset chosen
 * by a still pose stays in force while a clip plays, and an emote overlay
 * applies to every pose. */

/* begin/end rather than a length: the linker knows both addresses, but not their difference. */
typedef struct {
    const char *name;
    const uint8_t *begin;
    const uint8_t *end;
} ay_character_blob;

/* Provided by the build (a generated table) or by the host test. In firmware each
 * blob is an HCZ container (a deflated HCA) that track_store.c unpacks into the
 * ay_chars partition; the host test supplies plain HCA images. */
extern const ay_character_blob ay_character_blobs[];
extern const unsigned ay_character_blob_count;

/* The plain HCA image of a blob, mapped read-only until the next call or release.
 * NULL when it cannot be unpacked. */
const uint8_t *ay_character_image(unsigned blob, size_t *size);
void ay_character_image_release(void);

/* True when every embedded character is already unpacked in flash. Installing
 * erases and rewrites the partition (seconds, once per firmware with new data). */
bool ay_character_store_ready(void);
bool ay_character_store_install(void);

unsigned ay_character_count(void);
const char *ay_character_name(unsigned index);
unsigned ay_character_current(void);
/* Switches the active character. Refused while a frame is pinned. */
bool ay_character_select(unsigned index);

/* Pose space of the active character. */
unsigned ay_character_pose_count(void);
unsigned ay_character_inspection_count(void);

/* Face presets are selected with the still poses; labels are for the HUD. */
const char *ay_character_face_label(unsigned preset);
unsigned ay_character_face(void);
bool ay_character_set_face(unsigned preset);

/* Emote overlays: 0 is none, 1..emote_count are the character's marks. */
unsigned ay_character_emote_count(void);
unsigned ay_character_emote(void);
bool ay_character_set_emote(unsigned emote);
const char *ay_character_emote_label(unsigned emote);

/* Clips are alternative motions over the same geometry. */
unsigned ay_character_clip_count(void);
unsigned ay_character_clip(void);
bool ay_character_set_clip(unsigned clip);
const char *ay_character_clip_label(unsigned clip);
/* Intended milliseconds between clip frames. */
unsigned ay_character_period_ms(void);
/* Hex SHA-256 of the source model, for the boot log. */
const char *ay_character_sha256(void);

/* Parse and fully validate an HCA image without selecting it (host tests,
 * flash-time checks). Returns NULL for a well-formed image, else the reason. */
const char *ay_character_check(const uint8_t *data, size_t size);

#endif
