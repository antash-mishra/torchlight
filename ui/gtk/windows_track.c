/* Open windows reported one event at a time, listed most recently used first (ADR 0052). */
#include "windows_track.h"
#include <stdlib.h>
#include <string.h>
/* Room for this many windows at first; the array doubles when it fills. */
#define TRACK_INITIAL_CAPACITY 16
struct tracked {
    uint64_t id;
    void *data;
    /* When the window last became active, on the track's own clock; zero if never. */
    uint64_t activated;
    bool active, ready;
    char app_id[WINDOWS_CLASS_BYTES];
    char title[WINDOWS_TITLE_BYTES];
};
struct windows_track {
    struct tracked *windows;
    size_t count, capacity;
    /* Ids count up from one, in the order the compositor announced the windows. */
    uint64_t last_id, clock;
};
tl_status windows_track_create(windows_track **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = calloc(1, sizeof(**out));
    return *out == NULL ? TL_NOMEM : TL_OK;
}
void windows_track_destroy(windows_track *track) {
    if (track == NULL)
        return;
    free(track->windows);
    free(track);
}
static tl_status grow(windows_track *track) {
    if (track->count < track->capacity)
        return TL_OK;
    size_t capacity = track->capacity == 0 ? TRACK_INITIAL_CAPACITY : track->capacity * 2;
    if (capacity < track->capacity || capacity > SIZE_MAX / sizeof(struct tracked))
        return TL_NOMEM;
    struct tracked *windows = realloc(track->windows, capacity * sizeof(struct tracked));
    if (windows == NULL)
        return TL_NOMEM;
    track->windows = windows;
    track->capacity = capacity;
    return TL_OK;
}
tl_status windows_track_add(windows_track *track, void *data, uint64_t *id) {
    if (track == NULL || id == NULL)
        return TL_INVALID;
    tl_status status = grow(track);
    if (status != TL_OK)
        return status;
    struct tracked *window = &track->windows[track->count++];
    *window = (struct tracked){.id = ++track->last_id, .data = data};
    *id = window->id;
    return TL_OK;
}
static struct tracked *lookup(const windows_track *track, uint64_t id) {
    if (track == NULL)
        return NULL;
    for (size_t i = 0; i < track->count; i++)
        if (track->windows[i].id == id)
            return &track->windows[i];
    return NULL;
}
void windows_track_set_title(windows_track *track, uint64_t id, const char *title) {
    struct tracked *window = lookup(track, id);
    if (window != NULL && title != NULL)
        windows_copy_text(window->title, sizeof(window->title), title, strlen(title), false);
}
void windows_track_set_app_id(windows_track *track, uint64_t id, const char *app_id) {
    struct tracked *window = lookup(track, id);
    if (window != NULL && app_id != NULL)
        windows_copy_name(window->app_id, sizeof(window->app_id), app_id);
}
void windows_track_set_active(windows_track *track, uint64_t id, bool active) {
    struct tracked *window = lookup(track, id);
    if (window == NULL)
        return;
    if (active && !window->active)
        window->activated = ++track->clock;
    window->active = active;
}
void windows_track_set_ready(windows_track *track, uint64_t id) {
    struct tracked *window = lookup(track, id);
    if (window != NULL)
        window->ready = true;
}
void *windows_track_remove(windows_track *track, uint64_t id) {
    struct tracked *window = lookup(track, id);
    if (window == NULL)
        return NULL;
    void *data = window->data;
    /* Listing sorts, so the last window can fill the gap. */
    *window = track->windows[--track->count];
    return data;
}
void *windows_track_find(const windows_track *track, uint64_t id) {
    const struct tracked *window = lookup(track, id);
    return window == NULL ? NULL : window->data;
}
void *windows_track_data_at(const windows_track *track, size_t index) {
    return track != NULL && index < track->count ? track->windows[index].data : NULL;
}
size_t windows_track_count(const windows_track *track) {
    return track == NULL ? 0 : track->count;
}
/* Whether a lists before b: the more recently active first, then in the compositor's order,
 * which for windows announced at once is its own guess at their use. Ids are unique, so two
 * windows never tie. */
static bool lists_before(const struct tracked *a, const struct tracked *b) {
    if (a->activated != b->activated)
        return a->activated > b->activated;
    return a->id < b->id;
}
static bool listable(const struct tracked *window, const char *skip_app_id) {
    return window->ready && (skip_app_id == NULL || strcmp(window->app_id, skip_app_id) != 0);
}
static void fill(tl_window *out, const struct tracked *window) {
    *out = (tl_window){.handle = window->id};
    memcpy(out->app_id, window->app_id, sizeof(out->app_id));
    memcpy(out->class_name, window->app_id, sizeof(out->class_name));
    memcpy(out->instance, window->app_id, sizeof(out->instance));
    memcpy(out->title, window->title, sizeof(out->title));
}
/* The listable window that lists first among those after previous (all when NULL). */
static const struct tracked *next_window(const windows_track *track, const char *skip_app_id,
                                         const struct tracked *previous) {
    const struct tracked *best = NULL;
    for (size_t i = 0; i < track->count; i++) {
        const struct tracked *window = &track->windows[i];
        if (!listable(window, skip_app_id) || (previous != NULL && !lists_before(previous, window)))
            continue;
        if (best == NULL || lists_before(window, best))
            best = window;
    }
    return best;
}
size_t windows_track_list(const windows_track *track, const char *skip_app_id, tl_window *out,
                          size_t capacity) {
    if (track == NULL || out == NULL)
        return 0;
    /* A selection pass per slot: at most WINDOWS_MAX passes over a few dozen windows, with
     * no allocation. */
    size_t written = 0;
    const struct tracked *previous = NULL;
    while (written < capacity) {
        const struct tracked *window = next_window(track, skip_app_id, previous);
        if (window == NULL)
            break;
        fill(&out[written++], window);
        previous = window;
    }
    return written;
}
