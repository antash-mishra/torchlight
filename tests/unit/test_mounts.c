/* Mount-table parsing, escape decoding and which changes touch indexed roots. */
#include "test.h"
#include "torchlight/mounts.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#define BASE_TABLE                                                                                 \
    "1 0 252:1 / / rw,relatime shared:1 - ext4 /dev/mapper/root rw\n"                              \
    "25 1 0:22 / /run rw,nosuid shared:5 - tmpfs tmpfs rw\n"                                       \
    "30 1 8:2 / /boot/efi rw,relatime shared:31 - vfat /dev/sda2 rw\n"
static tl_mounts *parse(const char *text) {
    tl_mounts *mounts = NULL;
    CHECK(mounts_parse(text, strlen(text), &mounts) == TL_OK);
    return mounts;
}
static bool affect(const char *before, const char *after, const char *root) {
    tl_mounts *earlier = parse(before), *later = parse(after);
    const char *roots[] = {root};
    bool affected = mounts_affect(earlier, later, roots, 1);
    mounts_destroy(earlier);
    mounts_destroy(later);
    return affected;
}
/* Which added, removed or replaced mounts concern a root at /home/me. */
static void relevance(void) {
    const char *root = "/home/me";
    /* An hourly backup check mounts a disk far from the root, then unmounts. */
    const char *backup = BASE_TABLE "41 25 8:1 / /run/timeshift/77/backup rw - ext4 /dev/sda1 rw\n";
    CHECK(!affect(BASE_TABLE, backup, root) && !affect(backup, BASE_TABLE, root));
    const char *snap = BASE_TABLE "42 1 7:3 / /snap/core/1 ro - squashfs /dev/loop3 ro\n";
    CHECK(!affect(BASE_TABLE, snap, root));
    /* Mounts below, at and above the root hide or reveal indexed entries. */
    const char *below = BASE_TABLE "43 1 8:17 / /home/me/usb rw - vfat /dev/sdb1 rw\n";
    CHECK(affect(BASE_TABLE, below, root) && affect(below, BASE_TABLE, root));
    const char *at = BASE_TABLE "44 1 0:50 / /home/me rw - ecryptfs /home/.me rw\n";
    CHECK(affect(BASE_TABLE, at, root));
    const char *above = BASE_TABLE "45 1 252:2 / /home rw - ext4 /dev/mapper/home rw\n";
    CHECK(affect(BASE_TABLE, above, root));
    const char *stacked = BASE_TABLE "46 1 0:60 / / rw - overlay overlay rw\n";
    CHECK(affect(BASE_TABLE, stacked, root));
    /* A sibling sharing the root's prefix is not below it. */
    const char *sibling = BASE_TABLE "47 1 8:18 / /home/mean rw - ext4 /dev/sdb2 rw\n";
    CHECK(!affect(BASE_TABLE, sibling, root));
    /* A remount changes only options: no change at all. */
    const char *remounted = "1 0 252:1 / / ro,relatime shared:1 - ext4 /dev/mapper/root ro\n"
                            "25 1 0:22 / /run rw,nosuid shared:5 - tmpfs tmpfs rw\n"
                            "30 1 8:2 / /boot/efi rw,relatime shared:31 - vfat /dev/sda2 rw\n";
    CHECK(!affect(BASE_TABLE, remounted, "/"));
    /* A reused mount id at another place is a different mount. */
    const char *reused = BASE_TABLE "41 25 8:1 / /home/me/backup rw - ext4 /dev/sda1 rw\n";
    CHECK(affect(backup, reused, root));
    /* Unknown snapshots count as changed; no roots means nothing to affect. */
    tl_mounts *base = parse(BASE_TABLE);
    const char *roots[] = {root};
    CHECK(mounts_affect(NULL, base, roots, 1) && mounts_affect(base, NULL, roots, 1));
    CHECK(!mounts_affect(base, base, roots, 1) && !mounts_affect(base, base, NULL, 0));
    CHECK(mounts_affect(base, base, NULL, 1));
    mounts_destroy(base);
}
/* The kernel escapes space, tab, newline and backslash as \ooo. */
static void escapes(void) {
    const char *table =
        BASE_TABLE "50 1 8:33 / /home/me/My\\040Drive\\134x rw - vfat /dev/sdc1 rw\n";
    CHECK(affect(BASE_TABLE, table, "/home/me/My Drive\\x"));
    CHECK(!affect(BASE_TABLE, table, "/home/me/My Drive2"));
    /* Escapes the kernel never writes stay literal. */
    const char *literal = BASE_TABLE "51 1 8:34 / /media/a\\9b\\0000 rw - vfat /dev/sdd1 rw\n";
    CHECK(affect(BASE_TABLE, literal, "/media/a\\9b\\0000"));
}
static void malformed(void) {
    const char *invalid[] = {
        "1 0 252:1 /\n",       "x 0 252:1 / / rw\n",  "1 0 252:1 / relative rw\n",
        "1  0 252:1 / / rw\n", " 1 0 252:1 / / rw\n", "1 0 252:1 / /\n2 0 1:1 /"};
    tl_mounts *valid = parse(BASE_TABLE);
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        tl_mounts *mounts = valid; /* an error must clear it */
        CHECK(mounts_parse(invalid[i], strlen(invalid[i]), &mounts) == TL_INVALID);
        CHECK(mounts == NULL);
    }
    mounts_destroy(valid);
    tl_mounts *mounts = NULL;
    const char with_nul[] = "1 0 252:1 / / rw\0\n";
    CHECK(mounts_parse(with_nul, sizeof(with_nul) - 1, &mounts) == TL_INVALID && mounts == NULL);
    CHECK(mounts_parse(NULL, 0, &mounts) == TL_INVALID && mounts_parse("", 0, NULL) == TL_INVALID);
    /* Blank lines and a missing final newline are fine; five fields suffice. */
    const char *loose = "\n1 0 252:1 / / rw\n\n2 1 0:5 / /run";
    CHECK(mounts_parse(loose, strlen(loose), &mounts) == TL_OK && mounts_count(mounts) == 2);
    mounts_destroy(mounts);
    CHECK(mounts_parse("", 0, &mounts) == TL_OK && mounts_count(mounts) == 0);
    mounts_destroy(mounts);
    mounts_destroy(NULL);
    CHECK(mounts_count(NULL) == 0);
}
static void reading(void) {
    tl_mounts *mounts = NULL;
    CHECK(mounts_read("/proc/self/mountinfo", &mounts) == TL_OK && mounts_count(mounts) > 0);
    mounts_destroy(mounts);
    CHECK(mounts_read("/nonexistent/torchlight-mountinfo", &mounts) == TL_IO && mounts == NULL);
    CHECK(mounts_read(NULL, &mounts) == TL_INVALID &&
          mounts_read("/proc/self/mountinfo", NULL) == TL_INVALID);
    char path[] = "/tmp/torchlight-mounts-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(write(fd, BASE_TABLE, strlen(BASE_TABLE)) == (ssize_t)strlen(BASE_TABLE));
    CHECK(mounts_read(path, &mounts) == TL_OK && mounts_count(mounts) == 3);
    mounts_destroy(mounts);
    /* A table past the read limit is refused rather than parsed in part. */
    CHECK(ftruncate(fd, (off_t)MOUNTS_MAX_BYTES + 1) == 0 && close(fd) == 0);
    CHECK(mounts_read(path, &mounts) == TL_LIMIT && mounts == NULL);
    CHECK(unlink(path) == 0);
}
void test_mounts(void) {
    relevance();
    escapes();
    malformed();
    reading();
}
