/* Search-thread personal ranking state (see personal.h). The usage summary
 * is split into file items and application items whenever its version
 * changes; application items are matched to the leased desktop snapshot's
 * positions whenever desktop_gen changes. Per query, only boost values are
 * copied into the prepared lists. */
#include "torchlight/personal.h"
#include "torchlight/ipc.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct recent_search {
    char search_id[IPC_HISTORY_ID_BYTES + 1];
    char query[LEXICAL_QUERY_BYTES + 1];
};
struct tl_personal {
    tl_usage *usage;
    /* History was cleared since startup: a startup summary is stale. */
    bool cleared;
    struct recent_search searches[PERSONAL_RECENT_SEARCHES];
    char events[PERSONAL_RECENT_EVENTS][IPC_HISTORY_ID_BYTES + 1];
    size_t next_search, next_event;
    /* Item indices by kind for usage version mapped_version (zero forces a
     * split), and the application items found in desktop snapshot
     * mapped_desktop_gen with their positions in app_boosts. key advances
     * with every split and keys catalog lease mappings: unlike a version, it
     * never repeats when an adopted summary replaces the live one. */
    uint64_t mapped_version, mapped_desktop_gen, key;
    size_t file_items[USAGE_MAX_ITEMS], app_all[USAGE_MAX_ITEMS], app_items[USAGE_MAX_ITEMS];
    uint64_t file_ids[USAGE_MAX_ITEMS];
    int file_values[USAGE_MAX_ITEMS];
    tl_lexical_boost app_boosts[USAGE_MAX_ITEMS];
    size_t file_count, app_total, app_count;
    atomic_size_t items;
};

tl_status personal_create(int64_t retention_seconds, tl_personal **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_personal *personal = calloc(1, sizeof(*personal));
    if (personal == NULL)
        return TL_NOMEM;
    atomic_init(&personal->items, 0);
    tl_status status = usage_create(retention_seconds, &personal->usage);
    if (status != TL_OK) {
        free(personal);
        return status;
    }
    *out = personal;
    return TL_OK;
}
void personal_destroy(tl_personal *personal) {
    if (personal == NULL)
        return;
    usage_destroy(personal->usage);
    free(personal);
}
static void publish_items(tl_personal *personal) {
    atomic_store(&personal->items, usage_count(personal->usage));
}
void personal_adopt(tl_personal *personal, tl_usage *loaded) {
    if (personal == NULL || loaded == NULL) {
        usage_destroy(loaded);
        return;
    }
    if (personal->cleared || usage_merge(loaded, personal->usage) != TL_OK) {
        usage_destroy(loaded);
        return;
    }
    usage_destroy(personal->usage);
    personal->usage = loaded;
    personal->mapped_version = 0;
    publish_items(personal);
}
/* Copy text into a fixed buffer of capacity bytes; false when it does not fit. */
static bool copy_text(char *out, size_t capacity, const char *text) {
    size_t length = strnlen(text, capacity);
    if (length == capacity)
        return false;
    memcpy(out, text, length + 1);
    return true;
}
void personal_remember(tl_personal *personal, const char *search_id, const char *query) {
    if (personal == NULL || search_id == NULL || search_id[0] == 0 || query == NULL)
        return;
    struct recent_search *slot = &personal->searches[personal->next_search];
    if (!copy_text(slot->search_id, sizeof(slot->search_id), search_id) ||
        !copy_text(slot->query, sizeof(slot->query), query)) {
        slot->search_id[0] = 0;
        return;
    }
    personal->next_search = (personal->next_search + 1) % PERSONAL_RECENT_SEARCHES;
}
const char *personal_query_of(const tl_personal *personal, const char *search_id) {
    if (personal == NULL || search_id == NULL || search_id[0] == 0)
        return NULL;
    for (size_t i = 0; i < PERSONAL_RECENT_SEARCHES; i++)
        if (strcmp(personal->searches[i].search_id, search_id) == 0)
            return personal->searches[i].query;
    return NULL;
}
/* Whether event_id was recorded recently; otherwise remember it. */
static bool repeated_event(tl_personal *personal, const char *event_id) {
    for (size_t i = 0; i < PERSONAL_RECENT_EVENTS; i++)
        if (strcmp(personal->events[i], event_id) == 0)
            return true;
    char *slot = personal->events[personal->next_event];
    if (copy_text(slot, sizeof(personal->events[0]), event_id))
        personal->next_event = (personal->next_event + 1) % PERSONAL_RECENT_EVENTS;
    return false;
}
tl_status personal_record(tl_personal *personal, const char *event_id, tl_usage_target target,
                          const char *query, int64_t now) {
    if (personal == NULL || event_id == NULL || event_id[0] == 0)
        return TL_INVALID;
    if (repeated_event(personal, event_id))
        return TL_OK;
    tl_status status = usage_record(personal->usage, target, query, now);
    publish_items(personal);
    return status;
}
void personal_clear(tl_personal *personal) {
    if (personal == NULL)
        return;
    usage_clear(personal->usage);
    memset(personal->searches, 0, sizeof(personal->searches));
    memset(personal->events, 0, sizeof(personal->events));
    personal->cleared = true;
    personal->mapped_version = 0;
    publish_items(personal);
}
/* Split item indices by kind after the summary's indices changed. */
static void split_items(tl_personal *personal) {
    uint64_t version = usage_version(personal->usage);
    if (version == personal->mapped_version)
        return;
    personal->file_count = personal->app_total = 0;
    for (size_t i = 0; i < usage_count(personal->usage); i++) {
        tl_usage_target target = usage_target(personal->usage, i);
        if (target.file_id != 0) {
            personal->file_items[personal->file_count] = i;
            personal->file_ids[personal->file_count++] = target.file_id;
        } else {
            personal->app_all[personal->app_total++] = i;
        }
    }
    personal->mapped_version = version;
    personal->mapped_desktop_gen = 0;
    personal->key++;
}
/* Find each application item's position in the leased desktop snapshot. */
static void map_applications(tl_personal *personal, const tl_desktop *desktop) {
    uint64_t desktop_gen_now = desktop_gen(desktop);
    if (desktop_gen_now == personal->mapped_desktop_gen)
        return;
    personal->app_count = 0;
    for (size_t a = 0; a < personal->app_total; a++) {
        const char *wanted = usage_target(personal->usage, personal->app_all[a]).desktop_id;
        for (size_t p = 0; p < desktop_count(desktop); p++) {
            if (strcmp(desktop_entry(desktop, p)->desktop_id, wanted) != 0)
                continue;
            personal->app_items[personal->app_count] = personal->app_all[a];
            personal->app_boosts[personal->app_count++] = (tl_lexical_boost){p, 0};
            break;
        }
    }
    personal->mapped_desktop_gen = desktop_gen_now;
}
tl_status personal_boosts(tl_personal *personal, const tl_desktop *desktop, const char *query,
                          int64_t now, tl_catalog_boosts *files, const tl_lexical_boost **apps,
                          size_t *app_count) {
    if (files == NULL || apps == NULL || app_count == NULL)
        return TL_INVALID;
    *files = (tl_catalog_boosts){0};
    *apps = NULL;
    *app_count = 0;
    if (personal == NULL)
        return TL_INVALID;
    const int *values = NULL;
    /* Boosting first: a refresh can evict expired items and change indices. */
    tl_status status = usage_boosts(personal->usage, query, now, &values);
    if (status != TL_OK)
        return status;
    publish_items(personal);
    split_items(personal);
    for (size_t f = 0; f < personal->file_count; f++)
        personal->file_values[f] = values[personal->file_items[f]];
    *files = (tl_catalog_boosts){personal->file_ids, personal->file_values, personal->file_count,
                                 personal->key};
    if (desktop == NULL)
        return TL_OK;
    map_applications(personal, desktop);
    for (size_t a = 0; a < personal->app_count; a++)
        personal->app_boosts[a].boost = values[personal->app_items[a]];
    *apps = personal->app_boosts;
    *app_count = personal->app_count;
    return TL_OK;
}
size_t personal_items(const tl_personal *personal) {
    return personal == NULL ? 0 : atomic_load(&personal->items);
}
