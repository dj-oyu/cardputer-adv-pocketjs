// Host test for the half of fs.volume.sd that decides what an app may reach.
// No card, no driver, no IDF: main/pocket/sd_path.c compiled by the host gcc,
// line for line the same file the firmware links.
//
//   gcc -O2 -Wall -Wextra -Werror -I main/pocket tools/test_sd.c
//       main/pocket/sd_path.c -o .cache/test_sd.exe && .cache/test_sd.exe
//
// The escape cases are the point. A grant that can be walked out of is worse
// than no SD support at all, and it is the failure least likely to show up by
// using the feature -- every one of these paths "works" if the check is missing.
#include <stdio.h>
#include <string.h>
#include "sd_path.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static sd_path_result_t build(const sd_media_t *m, const char *rel, char *out, size_t n) {
    return sd_path_build(m, rel, strlen(rel), out, n);
}

int main(void) {
    sd_media_t m;
    char buf[SD_FSPATH_MAX];

    // ---- a zeroed struct grants nothing -------------------------------------
    sd_media_reset(&m);
    CHECK(!sd_media_usable(&m), "fresh media usable");
    CHECK(m.generation == 0, "generation starts %u", m.generation);
    CHECK(!sd_generation_valid(&m, 0), "generation 0 accepted");
    CHECK(build(&m, "a", buf, sizeof buf) == SD_PATH_NO_GRANT, "no grant not refused");

    // ---- grant before media: still refuses, and with the ungranted reason ----
    CHECK(sd_media_grant(&m, "pocket", 6), "grant rejected");
    CHECK(build(&m, "a", buf, sizeof buf) == SD_PATH_DISCONNECTED,
          "granted but absent should be DISCONNECTED");

    // ---- mount ---------------------------------------------------------------
    sd_media_mounted(&m);
    CHECK(m.generation == 1, "generation after mount %u", m.generation);
    CHECK(sd_media_usable(&m), "mounted+granted not usable");
    CHECK(sd_generation_valid(&m, 1), "generation 1 rejected");
    CHECK(!sd_generation_valid(&m, 2), "future generation accepted");

    // ---- translation ---------------------------------------------------------
    CHECK(build(&m, "", buf, sizeof buf) == SD_PATH_OK && !strcmp(buf, "/sd/pocket"),
          "root -> %s", buf);
    CHECK(build(&m, "a", buf, sizeof buf) == SD_PATH_OK && !strcmp(buf, "/sd/pocket/a"),
          "one component -> %s", buf);
    CHECK(build(&m, "a/b/c", buf, sizeof buf) == SD_PATH_OK &&
          !strcmp(buf, "/sd/pocket/a/b/c"), "three components -> %s", buf);
    // UTF-8 passes through untouched: the encoding is FATFS_API_ENCODING_UTF_8
    // precisely so a name is not rewritten on its way to the card.
    CHECK(build(&m, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", buf, sizeof buf) == SD_PATH_OK &&
          !strcmp(buf, "/sd/pocket/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"),
          "utf-8 mangled -> %s", buf);

    // ---- escapes: every one of these must be refused -------------------------
    static const char *escapes[] = {
        "..", "../", "../x", "a/..", "a/../..", "a/../../etc",
        ".", "./x", "a/./b", "/abs", "a/", "a//b", "\\", "a\\b", "..\\..",
    };
    for (size_t i = 0; i < sizeof escapes / sizeof *escapes; i++) {
        sd_path_result_t r = build(&m, escapes[i], buf, sizeof buf);
        CHECK(r == SD_PATH_INVALID, "escape \"%s\" returned %d (%s)",
              escapes[i], (int)r, r == SD_PATH_OK ? buf : "");
    }

    // ---- a grant itself cannot contain a separator ---------------------------
    sd_media_t g;
    sd_media_reset(&g);
    CHECK(!sd_media_grant(&g, "a/b", 3), "grant with slash accepted");
    CHECK(!sd_media_grant(&g, "..", 2), "grant of .. accepted");
    CHECK(!sd_media_grant(&g, ".", 1), "grant of . accepted");
    CHECK(!sd_media_grant(&g, "a\\b", 3), "grant with backslash accepted");
    CHECK(!sd_media_grant(&g, "", 0), "empty grant accepted");
    { char big[SD_ROOT_MAX + 8]; memset(big, 'x', sizeof big);
      CHECK(!sd_media_grant(&g, big, sizeof big), "oversized grant accepted"); }

    // ---- length ---------------------------------------------------------------
    { char small[8];
      CHECK(build(&m, "abcdefghijklmnop", small, sizeof small) == SD_PATH_TOO_LONG,
            "overflow not caught"); }

    // ---- removal invalidates the generation AND the grant ---------------------
    sd_media_removed(&m);
    CHECK(!sd_media_usable(&m), "removed media still usable");
    CHECK(!sd_generation_valid(&m, 1), "handle survived removal");
    CHECK(!m.granted, "grant survived removal");
    // Section 3: reinsertion does not revive anything. A remount is a NEW
    // generation, and without a fresh grant it is still refused.
    sd_media_mounted(&m);
    CHECK(m.generation == 2, "remount generation %u", m.generation);
    CHECK(!sd_generation_valid(&m, 1), "old handle revived by remount");
    CHECK(build(&m, "a", buf, sizeof buf) == SD_PATH_NO_GRANT,
          "remount silently reconnected an app to a new card");

    // ---- a mounted card that answered wrongly is not ABSENT --------------------
    sd_media_reset(&m);
    sd_media_grant(&m, "pocket", 6);
    sd_media_mounted(&m);
    sd_media_failed(&m);
    CHECK(!sd_media_usable(&m), "failed media usable");
    CHECK(build(&m, "a", buf, sizeof buf) == SD_PATH_DISCONNECTED,
          "failed media wrong reason");

    if (failures == 0) printf("test_sd: all checks passed\n");
    else printf("test_sd: %d FAILURES\n", failures);
    return failures != 0;
}
