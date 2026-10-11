/* Mount-table snapshots from /proc/self/mountinfo, compared so that a change
 * is acted on only where it can hide or reveal indexed paths. */
#ifndef TORCHLIGHT_MOUNTS_H
#define TORCHLIGHT_MOUNTS_H
#include "torchlight/common.h"
#include <stdbool.h>
#include <stddef.h>
/* Largest mount table read; thousands of container mounts fit easily. */
#define MOUNTS_MAX_BYTES (16U * 1024U * 1024U)
typedef struct tl_mounts tl_mounts;
/** Parse mountinfo text (length bytes, not NUL-terminated; a NUL byte is
 * invalid) into an owned snapshot. A mount is identified by its first five
 * fields (mount id, parent id, device, root and mount point), so a remount
 * that only changes options is no change; the mount point's octal escapes
 * (\040 for a space and the like) are decoded. Blank lines are skipped.
 * TL_INVALID for NULL arguments or a malformed line, TL_NOMEM; out NULL on
 * error. */
tl_status mounts_parse(const char *text, size_t length, tl_mounts **out);
/** Read path (borrowed; normally /proc/self/mountinfo) to its end and parse
 * it. TL_INVALID for NULL arguments, TL_IO when it cannot be opened or read,
 * TL_LIMIT above MOUNTS_MAX_BYTES, plus mounts_parse errors; out NULL on
 * error. */
tl_status mounts_read(const char *path, tl_mounts **out);
/** Free a snapshot; NULL allowed, no errors. */
void mounts_destroy(tl_mounts *mounts);
/** Number of mounts in a snapshot; zero for NULL. */
size_t mounts_count(const tl_mounts *mounts);
/** Whether a mount present in only one of before and after has its mount
 * point at, below or above one of roots (borrowed absolute paths): such a
 * mount can hide, reveal or replace indexed entries without any inotify
 * event. Mounts elsewhere (backup snapshots, snaps, containers, removable
 * media outside the roots) report false. A NULL snapshot is unknown and
 * reports true. No allocation, I/O or errors. */
bool mounts_affect(const tl_mounts *before, const tl_mounts *after, const char *const *roots,
                   size_t root_count);
#endif
