/* /proc/self/mountinfo parsing and comparison. Each line reads
 *   36 35 98:0 /mnt1 /mnt/parent rw,noatime master:1 - ext3 /dev/root rw
 * (mount id, parent id, major:minor, root, mount point, options, ...). The
 * first five fields identify a mount: a remount keeps them, while a mount
 * added or removed, even at a reused mount id, changes them. */
#include "torchlight/mounts.h"
#include "torchlight/path.h"
#include "torchlight/vec.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Fields of a line that identify its mount; the last of them is the mount point. */
#define MOUNTS_IDENTITY_FIELDS 5
/* First read buffer, doubled up to MOUNTS_MAX_BYTES. */
#define MOUNTS_READ_BYTES 65536U
/* The kernel escapes a mount point byte as a backslash and three octal digits. */
#define MOUNTS_ESCAPE_DIGITS 3
#define MOUNTS_BYTE_MAX 255
struct mount {
    char *identity; /* the first five fields as written, escapes intact */
    char *point;    /* the mount point, decoded */
};
struct tl_mounts {
    tl_vec *mounts; /* struct mount, ascending by identity */
};
void mounts_destroy(tl_mounts *mounts) {
    if (mounts == NULL)
        return;
    struct mount *items = vec_data(mounts->mounts);
    for (size_t i = 0; i < vec_count(mounts->mounts); i++) {
        free(items[i].identity);
        free(items[i].point);
    }
    vec_destroy(mounts->mounts);
    free(mounts);
}
size_t mounts_count(const tl_mounts *mounts) {
    return mounts == NULL ? 0 : vec_count(mounts->mounts);
}
static bool octal_digit(char c) {
    return c >= '0' && c <= '7';
}
/* Value of the escape starting at field[i] (a backslash), or -1 when the
 * bytes there are not one the kernel writes. */
static int escape_value(const char *field, size_t length, size_t i) {
    if (field[i] != '\\' || i + MOUNTS_ESCAPE_DIGITS >= length)
        return -1;
    int value = 0;
    for (size_t digit = 1; digit <= MOUNTS_ESCAPE_DIGITS; digit++) {
        if (!octal_digit(field[i + digit]))
            return -1;
        value = value * 8 + (field[i + digit] - '0');
    }
    return value == 0 || value > MOUNTS_BYTE_MAX ? -1 : value;
}
/* Copy a mount point field, decoding its escapes. */
static char *decode_point(const char *field, size_t length) {
    char *point = malloc(length + 1);
    if (point == NULL)
        return NULL;
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        int value = escape_value(field, length, i);
        if (value < 0) {
            point[out++] = field[i];
            continue;
        }
        point[out++] = (char)value;
        i += MOUNTS_ESCAPE_DIGITS;
    }
    point[out] = 0;
    return point;
}
static bool all_digits(const char *text, size_t length) {
    for (size_t i = 0; i < length; i++)
        if (text[i] < '0' || text[i] > '9')
            return false;
    return length != 0;
}
/* Add the mount one line (length bytes, without its newline) describes. */
static tl_status parse_line(tl_vec *mounts, const char *line, size_t length) {
    size_t ends[MOUNTS_IDENTITY_FIELDS], fields = 0, start = 0;
    for (size_t i = 0; i <= length && fields < MOUNTS_IDENTITY_FIELDS; i++) {
        if (i < length && line[i] != ' ')
            continue;
        if (i == start)
            return TL_INVALID; /* an empty field */
        ends[fields++] = i;
        start = i + 1;
    }
    if (fields < MOUNTS_IDENTITY_FIELDS || !all_digits(line, ends[0]))
        return TL_INVALID;
    size_t point = ends[MOUNTS_IDENTITY_FIELDS - 2] + 1, end = ends[MOUNTS_IDENTITY_FIELDS - 1];
    if (line[point] != '/')
        return TL_INVALID;
    struct mount mount = {strndup(line, end), decode_point(line + point, end - point)};
    tl_status status = mount.identity == NULL || mount.point == NULL ? TL_NOMEM : TL_OK;
    if (status == TL_OK)
        status = vec_append(mounts, &mount);
    if (status != TL_OK) {
        free(mount.identity);
        free(mount.point);
    }
    return status;
}
static int mount_compare(const void *left, const void *right) {
    const struct mount *a = left, *b = right;
    return strcmp(a->identity, b->identity);
}
tl_status mounts_parse(const char *text, size_t length, tl_mounts **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (text == NULL || memchr(text, 0, length) != NULL)
        return TL_INVALID;
    tl_mounts *mounts = calloc(1, sizeof(*mounts));
    if (mounts == NULL)
        return TL_NOMEM;
    tl_status status = vec_create(sizeof(struct mount), &mounts->mounts);
    for (size_t start = 0; status == TL_OK && start < length;) {
        const char *newline = memchr(text + start, '\n', length - start);
        size_t end = newline == NULL ? length : (size_t)(newline - text);
        if (end > start)
            status = parse_line(mounts->mounts, text + start, end - start);
        start = end + 1;
    }
    if (status != TL_OK) {
        mounts_destroy(mounts);
        return status;
    }
    if (vec_count(mounts->mounts) > 1)
        qsort(vec_data(mounts->mounts), vec_count(mounts->mounts), sizeof(struct mount),
              mount_compare);
    *out = mounts;
    return TL_OK;
}
/* Read descriptor to its end into a growing buffer. The kernel builds /proc
 * tables per read call, so the size is unknown until a read returns zero;
 * one byte past MOUNTS_MAX_BYTES tells a table at the limit from a larger one. */
static tl_status read_all(int fd, char **out, size_t *length) {
    const size_t limit = (size_t)MOUNTS_MAX_BYTES + 1;
    size_t capacity = 0;
    *out = NULL;
    *length = 0;
    for (;;) {
        if (*length == capacity) {
            if (capacity == limit)
                return TL_LIMIT;
            capacity = capacity == 0 ? MOUNTS_READ_BYTES : capacity * 2;
            if (capacity > limit)
                capacity = limit;
            char *grown = realloc(*out, capacity);
            if (grown == NULL)
                return TL_NOMEM;
            *out = grown;
        }
        ssize_t count = read(fd, *out + *length, capacity - *length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return TL_IO;
        if (count == 0)
            return *length > MOUNTS_MAX_BYTES ? TL_LIMIT : TL_OK;
        *length += (size_t)count;
    }
}
tl_status mounts_read(const char *path, tl_mounts **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (path == NULL)
        return TL_INVALID;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return TL_IO;
    char *text = NULL;
    size_t length = 0;
    tl_status status = read_all(fd, &text, &length);
    int closed = close(fd);
    if (status == TL_OK && closed != 0)
        status = TL_IO;
    if (status == TL_OK)
        status = mounts_parse(text, length, out);
    free(text);
    return status;
}
/* A mount at or below a root hides or reveals indexed entries; one above a
 * root (its filesystem, or a mount stacked over a parent) replaces it all. */
static bool affects(const char *point, const char *const *roots, size_t root_count) {
    for (size_t i = 0; i < root_count; i++)
        if (path_within(point, roots[i]) || path_within(roots[i], point))
            return true;
    return false;
}
bool mounts_affect(const tl_mounts *before, const tl_mounts *after, const char *const *roots,
                   size_t root_count) {
    if (before == NULL || after == NULL || (roots == NULL && root_count != 0))
        return true;
    const struct mount *earlier = vec_const_data(before->mounts);
    const struct mount *later = vec_const_data(after->mounts);
    size_t i = 0, j = 0, earlier_count = vec_count(before->mounts),
           later_count = vec_count(after->mounts);
    /* Merge the two sorted lists; a mount in only one of them changed. */
    while (i < earlier_count || j < later_count) {
        int order = i == earlier_count ? 1
                    : j == later_count ? -1
                                       : strcmp(earlier[i].identity, later[j].identity);
        if (order == 0) {
            i++;
            j++;
            continue;
        }
        const struct mount *changed = order < 0 ? &earlier[i++] : &later[j++];
        if (affects(changed->point, roots, root_count))
            return true;
    }
    return false;
}
