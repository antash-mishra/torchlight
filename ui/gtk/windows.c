/* Open-window snapshots over a swappable source, application matching and title shortening. */
#include "torchlight/windows.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#define DESKTOP_SUFFIX ".desktop"
/* Separators browsers and editors put before their own name in window titles. */
static const char *const TITLE_SEPARATORS[] = {" - ", " \xe2\x80\x93 ", " \xe2\x80\x94 "};
struct tl_windows {
    tl_window_source source;
    bool has_source;
    tl_window windows[WINDOWS_MAX];
    size_t count;
};
tl_status windows_create(const tl_window_source *source, tl_windows **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (source != NULL &&
        (source->list == NULL || source->activate == NULL || source->destroy == NULL))
        return TL_INVALID;
    tl_windows *windows = calloc(1, sizeof(*windows));
    if (windows == NULL)
        return TL_NOMEM;
    if (source != NULL) {
        windows->source = *source;
        windows->has_source = true;
    }
    *out = windows;
    return TL_OK;
}
void windows_destroy(tl_windows *windows) {
    if (windows == NULL)
        return;
    if (windows->has_source)
        windows->source.destroy(windows->source.context);
    free(windows);
}
tl_status windows_refresh(tl_windows *windows) {
    if (windows == NULL)
        return TL_INVALID;
    windows->count = 0;
    if (!windows->has_source)
        return TL_OK;
    size_t count = 0;
    tl_status status =
        windows->source.list(windows->source.context, windows->windows, WINDOWS_MAX, &count);
    if (status != TL_OK)
        return status;
    if (count > WINDOWS_MAX)
        return TL_INVALID;
    for (size_t i = 0; i < count; i++)
        if (windows->windows[i].handle != 0)
            windows->windows[windows->count++] = windows->windows[i];
    return TL_OK;
}
size_t windows_count(const tl_windows *windows) {
    return windows == NULL ? 0 : windows->count;
}
const tl_window *windows_get(const tl_windows *windows, size_t index) {
    return windows != NULL && index < windows->count ? &windows->windows[index] : NULL;
}
tl_status windows_activate(tl_windows *windows, size_t index) {
    const tl_window *window = windows_get(windows, index);
    if (window == NULL || !windows->has_source)
        return TL_INVALID;
    return windows->source.activate(windows->source.context, window->handle);
}
static char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}
/* Whether desktop_id is name followed by ".desktop". canonical compares the name
 * lowercased with spaces as hyphens, as GNOME Shell and Cinnamon do. */
static bool desktop_name(const char *desktop_id, const char *name, bool canonical) {
    size_t length = strlen(name);
    if (length == 0)
        return false;
    /* A shorter desktop id stops at its NUL, which never equals a name byte. */
    for (size_t i = 0; i < length; i++) {
        char expected = name[i];
        if (canonical)
            expected = expected == ' ' ? '-' : ascii_lower(expected);
        if (desktop_id[i] != expected)
            return false;
    }
    return strcmp(desktop_id + length, DESKTOP_SUFFIX) == 0;
}
tl_windows_evidence windows_evidence(const tl_window *window, const char *desktop_id,
                                     const char *wm_class) {
    if (window == NULL || desktop_id == NULL)
        return WINDOWS_EVIDENCE_NONE;
    if (wm_class != NULL && wm_class[0] != 0) {
        if (strcmp(window->instance, wm_class) == 0)
            return WINDOWS_EVIDENCE_INSTANCE;
        if (strcmp(window->class_name, wm_class) == 0)
            return WINDOWS_EVIDENCE_CLASS;
    }
    if (desktop_name(desktop_id, window->app_id, false))
        return WINDOWS_EVIDENCE_APP_ID;
    if (desktop_name(desktop_id, window->instance, false) ||
        desktop_name(desktop_id, window->instance, true))
        return WINDOWS_EVIDENCE_DESKTOP_NAME;
    return WINDOWS_EVIDENCE_NONE;
}
static bool same_text(const char *left, const char *right, size_t length) {
    for (size_t i = 0; i < length; i++)
        if (ascii_lower(left[i]) != ascii_lower(right[i]))
            return false;
    return true;
}
/* Whether suffix (length bytes, not terminated) names the application: the
 * whole name, its first or last words, or text ending with its first word. */
static bool names_application(const char *suffix, size_t length, const char *name) {
    size_t name_length = strlen(name);
    if (length == 0 || name_length == 0)
        return false;
    if (length == name_length)
        return same_text(suffix, name, length);
    if (length < name_length) {
        bool first = name[length] == ' ' && same_text(suffix, name, length);
        bool last = name[name_length - length - 1] == ' ' &&
                    same_text(suffix, name + name_length - length, length);
        if (first || last)
            return true;
    }
    const char *space = strchr(name, ' ');
    size_t word = space == NULL ? name_length : (size_t)(space - name);
    return length > word && suffix[length - word - 1] == ' ' &&
           same_text(suffix + length - word, name, word);
}
/* Byte length of the title once a trailing application name is removed. */
static size_t title_length(const char *title, const char *app_name) {
    size_t length = strlen(title), best = SIZE_MAX, separator = 0;
    for (size_t i = 0; i < sizeof(TITLE_SEPARATORS) / sizeof(TITLE_SEPARATORS[0]); i++) {
        size_t size = strlen(TITLE_SEPARATORS[i]);
        for (const char *found = strstr(title, TITLE_SEPARATORS[i]); found != NULL;
             found = strstr(found + 1, TITLE_SEPARATORS[i]))
            if (best == SIZE_MAX || (size_t)(found - title) > best) {
                best = (size_t)(found - title);
                separator = size;
            }
    }
    if (best == SIZE_MAX || best == 0 || app_name == NULL)
        return length;
    size_t start = best + separator;
    return names_application(title + start, length - start, app_name) ? best : length;
}
void windows_short_title(const char *title, const char *app_name, char *out, size_t capacity) {
    if (out == NULL || capacity == 0)
        return;
    out[0] = 0;
    if (title == NULL)
        return;
    size_t length = title_length(title, app_name);
    if (length >= capacity) {
        length = capacity - 1;
        /* Back off continuation bytes so the copy ends on a whole character. */
        while (length > 0 && ((unsigned char)title[length] & 0xC0) == 0x80)
            length--;
    }
    memcpy(out, title, length);
    out[length] = 0;
}
