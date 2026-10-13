/* Place prior for file ranking: dependency-tree markers recognized from
 * normalized directory names, level counting and the capped penalty. */
#include "torchlight/place.h"
#include <string.h>
/* Folder names that by convention hold software installed or vendored by a
 * tool rather than files a person makes. Compared case-folded. */
static const char *const MARKERS[] = {
    "node_modules", "bower_components", "site-packages", "dist-packages", "vendor",
    "third_party",  "third-party",      "thirdparty",    "sysroot",       "sdk"};
/* Versioned install folders start with at most this many digits, which
 * leaves out dotted dates such as 2024.05.01. */
enum { PLACE_VERSION_LEAD_DIGITS = 3 };
static bool digit(uint32_t symbol) {
    return symbol >= '0' && symbol <= '9';
}
static bool equals(tl_text name, const char *ascii) {
    size_t length = strlen(ascii);
    if (name.length != length)
        return false;
    for (size_t i = 0; i < length; i++) {
        if (name.symbols[i] != (uint32_t)(unsigned char)ascii[i])
            return false;
    }
    return true;
}
/* Go modules live in name@vX.Y.Z folders, and the module cache's download
 * area in folders named exactly "@v". */
static bool go_module(tl_text name) {
    if (name.length == 2 && name.symbols[0] == '@' && name.symbols[1] == 'v')
        return true;
    for (size_t i = 0; i + 2 < name.length; i++) {
        if (name.symbols[i] == '@' && name.symbols[i + 1] == 'v' && digit(name.symbols[i + 2]))
            return true;
    }
    return false;
}
/* v?D{1,3}(.D+)+ over the whole name. */
static bool version_folder(tl_text name) {
    size_t i = name.length != 0 && name.symbols[0] == 'v' ? 1 : 0, lead = 0;
    for (; i < name.length && digit(name.symbols[i]); i++)
        lead++;
    if (lead == 0 || lead > PLACE_VERSION_LEAD_DIGITS)
        return false;
    size_t groups = 0;
    while (i < name.length) {
        if (name.symbols[i] != '.')
            return false;
        size_t start = ++i;
        for (; i < name.length && digit(name.symbols[i]); i++) {
        }
        if (i == start)
            return false;
        groups++;
    }
    return groups != 0;
}
bool place_dependency_name(tl_text name) {
    if (name.length == 0 || name.symbols == NULL)
        return false;
    for (size_t i = 0; i < sizeof(MARKERS) / sizeof(MARKERS[0]); i++) {
        if (equals(name, MARKERS[i]))
            return true;
    }
    return go_module(name) || version_folder(name);
}
tl_place place_child(tl_place parent, tl_text name, bool usable) {
    if (!usable)
        return parent;
    tl_place child = {.level = parent.level == UINT32_MAX ? UINT32_MAX : parent.level + 1,
                      .dependency = parent.dependency || place_dependency_name(name)};
    return child;
}
int place_penalty(tl_place parent) {
    /* The root's own folder is level 1, so an entry directly inside it is at
     * depth 0. */
    uint32_t depth = parent.level == 0 ? 0 : parent.level - 1;
    uint32_t extra = depth > PLACE_FREE_DEPTH ? depth - PLACE_FREE_DEPTH : 0;
    /* Compare before multiplying: deep paths must not overflow. */
    int penalty = extra >= PLACE_PENALTY_MAX / PLACE_DEPTH_PENALTY + 1
                      ? PLACE_PENALTY_MAX
                      : (int)extra * PLACE_DEPTH_PENALTY;
    if (parent.dependency)
        penalty += PLACE_DEPENDENCY_PENALTY;
    return penalty > PLACE_PENALTY_MAX ? PLACE_PENALTY_MAX : penalty;
}
