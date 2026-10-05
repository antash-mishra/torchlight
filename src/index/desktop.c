/* XDG precedence, visibility and localized search with background snapshot swaps. */
#include "torchlight/desktop.h"
#include "torchlight/hashmap.h"
#include "torchlight/vec.h"
#include <gio/gdesktopappinfo.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#define DESKTOP_REFRESH_SECONDS 1
#define DESKTOP_FIELD_BYTES 4096
#define DESKTOP_SCAN_DEPTH 16
#define DESKTOP_FILE_BYTES (64 * 1024)
/* A typed name token (Chrome in Google Chrome) beats a filename prefix.
 * Metadata-only matches retain their ordinary lexical strength. */
#define DESKTOP_NAME_PREFIX_BONUS 2000
struct entry {
    tl_desktop_entry public;
    char *key, *generic_name, *keywords;
    uint64_t fingerprint;
};
struct snapshot {
    tl_vec *entries, *seen;
    tl_hashmap *ids;
    tl_lexical *engine;
    tl_lexical_workspace *workspace;
};
struct tl_desktop {
    pthread_mutex_t lock, wake_lock;
    pthread_cond_t wake;
    pthread_t thread;
    bool started, stop, refresh;
    struct snapshot *active;
    uint64_t next_id;
};
static void snapshot_destroy(struct snapshot *snapshot) {
    if (snapshot == NULL)
        return;
    struct entry *entries = vec_data(snapshot->entries);
    for (size_t i = 0; i < vec_count(snapshot->entries); i++) {
        g_free((void *)entries[i].public.desktop_id);
        g_free((void *)entries[i].public.filename);
        g_free((void *)entries[i].public.name);
        g_free((void *)entries[i].public.icon);
        g_free(entries[i].key);
        g_free(entries[i].generic_name);
        g_free(entries[i].keywords);
    }
    char **seen = vec_data(snapshot->seen);
    for (size_t i = 0; i < vec_count(snapshot->seen); i++)
        g_free(seen[i]);
    vec_destroy(snapshot->seen);
    hashmap_destroy(snapshot->ids);
    vec_destroy(snapshot->entries);
    lexical_workspace_destroy(snapshot->workspace);
    lexical_destroy(snapshot->engine);
    free(snapshot);
}
static tl_status snapshot_create(struct snapshot **out) {
    *out = calloc(1, sizeof(**out));
    if (*out == NULL)
        return TL_NOMEM;
    tl_status status = vec_create(sizeof(struct entry), &(*out)->entries);
    if (status == TL_OK)
        status = vec_create(sizeof(char *), &(*out)->seen);
    if (status == TL_OK)
        status = hashmap_create(&(*out)->ids);
    if (status == TL_OK)
        status = lexical_create(&(*out)->engine);
    if (status == TL_OK)
        status = lexical_set_prefix_bonus((*out)->engine, DESKTOP_NAME_PREFIX_BONUS);
    return status;
}
struct id_key {
    const struct snapshot *snapshot;
    const char *id;
};
static bool same_id(const void *context, uint32_t value) {
    const struct id_key *key = context;
    char *const *seen = vec_const_data(key->snapshot->seen);
    return strcmp(key->id, seen[value]) == 0;
}
static tl_status remember_id(struct snapshot *snapshot, const char *id, bool *duplicate) {
    struct id_key key = {snapshot, id};
    uint64_t hash = hashmap_hash(HASHMAP_HASH_SEED, id, strlen(id));
    uint32_t unused;
    *duplicate = hashmap_find(snapshot->ids, hash, same_id, &key, &unused);
    if (*duplicate)
        return TL_OK;
    if (strlen(id) >= DESKTOP_FIELD_BYTES || vec_count(snapshot->seen) == DESKTOP_MAX_ENTRIES)
        return TL_LIMIT;
    char *copy = g_strdup(id);
    tl_status status = vec_append(snapshot->seen, &copy);
    if (status != TL_OK) {
        g_free(copy);
        return status;
    }
    return hashmap_insert(snapshot->ids, hash, (uint32_t)(vec_count(snapshot->seen) - 1));
}
static bool settings_category(GDesktopAppInfo *info) {
    char **categories = g_desktop_app_info_get_string_list(info, "Categories", NULL);
    bool settings = false;
    if (categories != NULL)
        for (size_t i = 0; categories[i] != NULL; i++)
            if (strcmp(categories[i], "Settings") == 0)
                settings = true;
    g_strfreev(categories);
    return settings;
}
static char *search_key(GDesktopAppInfo *info) {
    char *key = g_strconcat("/Applications/", g_app_info_get_display_name(G_APP_INFO(info)), NULL);
    for (size_t i = strlen("/Applications/"); key[i] != 0; i++)
        if (key[i] == '/')
            key[i] = ' ';
    return key;
}
/* Preserve Name when a desktop supplies a different display label, but keep
 * both separate from keywords and from filesystem folder context. */
static char *generic_field(GDesktopAppInfo *info) {
    const char *generic = g_desktop_app_info_get_generic_name(info);
    const char *name = g_app_info_get_name(G_APP_INFO(info));
    if (name != NULL && strlen(name) >= DESKTOP_FIELD_BYTES)
        name = NULL;
    if (generic != NULL && strlen(generic) >= DESKTOP_FIELD_BYTES)
        generic = NULL;
    return g_strconcat(name == NULL ? "" : name, " ", generic == NULL ? "" : generic, NULL);
}
static char *keyword_field(GDesktopAppInfo *info) {
    const char *const *keywords = g_desktop_app_info_get_keywords(info);
    GString *text = g_string_new("");
    if (keywords != NULL)
        for (size_t i = 0; keywords[i] != NULL; i++) {
            if (strlen(keywords[i]) > DESKTOP_FIELD_BYTES - text->len)
                break;
            g_string_append(text, keywords[i]);
            if (text->len < DESKTOP_FIELD_BYTES)
                g_string_append_c(text, ' ');
        }
    return g_string_free(text, FALSE);
}
static GDesktopAppInfo *entry_info(const char *filename, uint64_t *revision) {
    GKeyFile *file = g_key_file_new();
    GDesktopAppInfo *info = NULL;
    char *data = NULL;
    gsize length = 0;
    if (!g_key_file_load_from_file(file, filename, G_KEY_FILE_NONE, NULL))
        goto cleanup;
    /* One parsed read defines both metadata and revision. A replacement after
     * this read must fail launch validation and get a new id on refresh. */
    info = g_desktop_app_info_new_from_keyfile(file);
    if (info == NULL)
        goto cleanup;
    data = g_key_file_to_data(file, &length, NULL);
    if (data == NULL) {
        g_object_unref(info);
        info = NULL;
        goto cleanup;
    }
    *revision = hashmap_hash(HASHMAP_HASH_SEED, filename, strlen(filename));
    *revision = hashmap_hash(*revision, data, length);
cleanup:
    g_free(data);
    g_key_file_unref(file);
    return info;
}
static tl_status add_entry(struct snapshot *snapshot, const char *filename, const char *id) {
    bool duplicate = false;
    tl_status status = remember_id(snapshot, id, &duplicate);
    if (status != TL_OK || duplicate)
        return status;
    /* Remember even hidden/malformed overrides: they mask lower-priority copies. */
    struct stat info_stat;
    if (strlen(filename) >= DESKTOP_FIELD_BYTES || stat(filename, &info_stat) != 0 ||
        !S_ISREG(info_stat.st_mode) || info_stat.st_size < 0 ||
        info_stat.st_size > DESKTOP_FILE_BYTES)
        return TL_OK;
    uint64_t revision = 0;
    GDesktopAppInfo *info = entry_info(filename, &revision);
    if (info == NULL)
        return TL_OK;
    const char *name = g_app_info_get_display_name(G_APP_INFO(info));
    if (g_desktop_app_info_get_is_hidden(info) || !g_app_info_should_show(G_APP_INFO(info)) ||
        name == NULL || strlen(name) >= DESKTOP_FIELD_BYTES) {
        g_object_unref(info);
        return TL_OK;
    }
    struct entry entry = {0};
    entry.public.desktop_id = g_strdup(id);
    entry.public.filename = g_strdup(filename);
    entry.public.name = g_strdup(name);
    GIcon *icon = g_app_info_get_icon(G_APP_INFO(info));
    entry.public.icon =
        icon == NULL ? g_strdup("application-x-executable-symbolic") : g_icon_to_string(icon);
    if (entry.public.icon == NULL || strlen(entry.public.icon) >= DESKTOP_FIELD_BYTES) {
        g_free((void *)entry.public.icon);
        entry.public.icon = g_strdup("application-x-executable-symbolic");
    }
    entry.public.settings = settings_category(info);
    entry.key = search_key(info);
    entry.generic_name = generic_field(info);
    entry.keywords = keyword_field(info);
    entry.fingerprint = revision;
    entry.public.revision = revision;
    g_object_unref(info);
    status = vec_append(snapshot->entries, &entry);
    if (status != TL_OK) {
        g_free((void *)entry.public.desktop_id);
        g_free((void *)entry.public.filename);
        g_free((void *)entry.public.name);
        g_free((void *)entry.public.icon);
        g_free(entry.key);
        g_free(entry.generic_name);
        g_free(entry.keywords);
    }
    return status;
}
static tl_status scan_directory(struct snapshot *snapshot, const char *root, const char *relative,
                                unsigned depth) {
    if (depth > DESKTOP_SCAN_DEPTH)
        return TL_LIMIT;
    char *directory = g_build_filename(root, relative, NULL);
    GDir *dir = g_dir_open(directory, 0, NULL);
    g_free(directory);
    if (dir == NULL)
        return TL_OK;
    tl_status status = TL_OK;
    const char *name;
    while (status == TL_OK && (name = g_dir_read_name(dir)) != NULL) {
        char *child = g_build_filename(relative, name, NULL);
        char *filename = g_build_filename(root, child, NULL);
        if (g_file_test(filename, G_FILE_TEST_IS_DIR) &&
            !g_file_test(filename, G_FILE_TEST_IS_SYMLINK))
            status = scan_directory(snapshot, root, child, depth + 1);
        else if (g_str_has_suffix(name, ".desktop")) {
            char *id = g_strdup(child);
            for (size_t i = 0; id[i] != 0; i++)
                if (id[i] == '/')
                    id[i] = '-';
            status = add_entry(snapshot, filename, id);
            g_free(id);
        }
        g_free(filename);
        g_free(child);
    }
    g_dir_close(dir);
    return status;
}
static int entry_compare(const void *left, const void *right) {
    const struct entry *a = left, *b = right;
    return strcmp(a->public.desktop_id, b->public.desktop_id);
}
static tl_status discover(struct snapshot **out) {
    tl_status status = snapshot_create(out);
    char *root = g_build_filename(g_get_user_data_dir(), "applications", NULL);
    if (status == TL_OK)
        status = scan_directory(*out, root, "", 0);
    g_free(root);
    const char *const *system = g_get_system_data_dirs();
    for (size_t i = 0; status == TL_OK && system[i] != NULL; i++) {
        root = g_build_filename(system[i], "applications", NULL);
        status = scan_directory(*out, root, "", 0);
        g_free(root);
    }
    if (status == TL_OK && vec_count((*out)->entries) > 1)
        qsort(vec_data((*out)->entries), vec_count((*out)->entries), sizeof(struct entry),
              entry_compare);
    return status;
}
static uint64_t previous_id(const struct snapshot *old, const struct entry *entry) {
    if (old == NULL)
        return 0;
    const struct entry *entries = vec_const_data(old->entries);
    for (size_t i = 0; i < vec_count(old->entries); i++)
        if (strcmp(entries[i].public.desktop_id, entry->public.desktop_id) == 0 &&
            entries[i].fingerprint == entry->fingerprint)
            return entries[i].public.id;
    return 0;
}
static int id_compare(const void *left, const void *right) {
    const struct entry *a = left, *b = right;
    return (a->public.id > b->public.id) - (a->public.id < b->public.id);
}
static tl_status build(tl_desktop *desktop, struct snapshot *snapshot) {
    struct entry *entries = vec_data(snapshot->entries);
    for (size_t i = 0; i < vec_count(snapshot->entries); i++) {
        entries[i].public.id = previous_id(desktop->active, &entries[i]);
        if (entries[i].public.id == 0) {
            if (desktop->next_id == INT64_MAX)
                return TL_LIMIT;
            entries[i].public.id = ++desktop->next_id;
        }
    }
    if (vec_count(snapshot->entries) > 1)
        qsort(entries, vec_count(snapshot->entries), sizeof(*entries), id_compare);
    tl_status status = TL_OK;
    for (size_t i = 0; status == TL_OK && i < vec_count(snapshot->entries); i++)
        status = lexical_add_fields(snapshot->engine, entries[i].public.id, entries[i].key,
                                    entries[i].generic_name, entries[i].keywords);
    if (status == TL_OK)
        status = lexical_finish(snapshot->engine);
    if (status == TL_OK)
        status = lexical_workspace_create(snapshot->engine, &snapshot->workspace);
    return status;
}
static bool unchanged(const struct snapshot *old, const struct snapshot *next) {
    if (old == NULL || vec_count(old->entries) != vec_count(next->entries))
        return false;
    const struct entry *entries = vec_const_data(next->entries);
    for (size_t i = 0; i < vec_count(next->entries); i++)
        if (previous_id(old, &entries[i]) == 0)
            return false;
    return true;
}
static void *refresh_worker(void *context) {
    tl_desktop *desktop = context;
    for (;;) {
        pthread_mutex_lock(&desktop->wake_lock);
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += DESKTOP_REFRESH_SECONDS;
        while (!desktop->stop && !desktop->refresh) {
            if (pthread_cond_timedwait(&desktop->wake, &desktop->wake_lock, &deadline) != 0)
                break;
        }
        bool stop = desktop->stop;
        desktop->refresh = false;
        pthread_mutex_unlock(&desktop->wake_lock);
        if (stop)
            return NULL;
        struct snapshot *next = NULL;
        tl_status status = discover(&next);
        if (status == TL_OK && unchanged(desktop->active, next)) {
            snapshot_destroy(next);
            continue;
        }
        if (status == TL_OK)
            status = build(desktop, next);
        if (status != TL_OK) {
            snapshot_destroy(next);
            continue;
        }
        pthread_mutex_lock(&desktop->lock);
        struct snapshot *old = desktop->active;
        desktop->active = next;
        pthread_mutex_unlock(&desktop->lock);
        snapshot_destroy(old);
    }
}
tl_status desktop_create(tl_desktop **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_desktop *desktop = calloc(1, sizeof(*desktop));
    if (desktop == NULL)
        return TL_NOMEM;
    if (pthread_mutex_init(&desktop->lock, NULL) != 0) {
        free(desktop);
        return TL_IO;
    }
    if (pthread_mutex_init(&desktop->wake_lock, NULL) != 0) {
        pthread_mutex_destroy(&desktop->lock);
        free(desktop);
        return TL_IO;
    }
    if (pthread_cond_init(&desktop->wake, NULL) != 0) {
        pthread_mutex_destroy(&desktop->wake_lock);
        pthread_mutex_destroy(&desktop->lock);
        free(desktop);
        return TL_IO;
    }
    uint32_t nonce = 0;
    tl_status status =
        getrandom(&nonce, sizeof(nonce), 0) == (ssize_t)sizeof(nonce) ? TL_OK : TL_IO;
    desktop->next_id = DESKTOP_ID_BASE | ((uint64_t)(nonce & 0x3fffffffU) << 32);
    if (status == TL_OK)
        status = discover(&desktop->active);
    if (status == TL_OK)
        status = build(desktop, desktop->active);
    if (status == TL_OK && pthread_create(&desktop->thread, NULL, refresh_worker, desktop) != 0)
        status = TL_IO;
    if (status != TL_OK) {
        desktop_destroy(desktop);
        return status;
    }
    desktop->started = true;
    *out = desktop;
    return TL_OK;
}
void desktop_destroy(tl_desktop *desktop) {
    if (desktop == NULL)
        return;
    pthread_mutex_lock(&desktop->wake_lock);
    desktop->stop = true;
    pthread_cond_signal(&desktop->wake);
    pthread_mutex_unlock(&desktop->wake_lock);
    if (desktop->started)
        pthread_join(desktop->thread, NULL);
    snapshot_destroy(desktop->active);
    pthread_cond_destroy(&desktop->wake);
    pthread_mutex_destroy(&desktop->wake_lock);
    pthread_mutex_destroy(&desktop->lock);
    free(desktop);
}
void desktop_acquire(tl_desktop *desktop) {
    pthread_mutex_lock(&desktop->lock);
}
void desktop_release(tl_desktop *desktop) {
    pthread_mutex_unlock(&desktop->lock);
}
void desktop_refresh(tl_desktop *desktop) {
    pthread_mutex_lock(&desktop->wake_lock);
    desktop->refresh = true;
    pthread_cond_signal(&desktop->wake);
    pthread_mutex_unlock(&desktop->wake_lock);
}
tl_status desktop_query(tl_desktop *desktop, const char *query, tl_result *results, size_t capacity,
                        size_t *count) {
    if (count == NULL)
        return TL_INVALID;
    *count = 0;
    if (desktop == NULL || query == NULL || results == NULL || capacity == 0 ||
        capacity > LEXICAL_MAX_RESULTS)
        return TL_INVALID;
    if (query[0] == 0) {
        return TL_OK;
    }
    return lexical_query(desktop->active->engine, desktop->active->workspace, query, results,
                         capacity, count);
}
const tl_desktop_entry *desktop_resolve(const tl_desktop *desktop, uint64_t id) {
    if (desktop == NULL)
        return NULL;
    const struct entry *entries = vec_const_data(desktop->active->entries);
    size_t low = 0, high = vec_count(desktop->active->entries);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (entries[middle].public.id < id)
            low = middle + 1;
        else
            high = middle;
    }
    return low < vec_count(desktop->active->entries) && entries[low].public.id == id
               ? &entries[low].public
               : NULL;
}
