/* XDG precedence, visibility and localized search with background snapshot swaps.
 * The refresh worker parses .desktop files only when a cheap change stamp of
 * the applications trees differs from that of what the last parse read:
 * parsing ~1 MB of localized entries every second costs about 1.5% of a core
 * on a stock desktop. */
#include "torchlight/desktop.h"
#include "torchlight/hashmap.h"
#include "torchlight/prefix.h"
#include "torchlight/vec.h"
#include <dirent.h>
#include <gio/gdesktopappinfo.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
/* Defaults of tl_desktop_options, in milliseconds. */
#define DESKTOP_CHECK_MS 1000
#define DESKTOP_SETTLE_MS 5000
#define DESKTOP_RESCAN_MS 600000
/* Stat fields one .desktop file contributes to a change stamp. */
#define DESKTOP_STAMP_FIELDS 8
/* Stamp contribution of a directory that could not be listed. */
#define DESKTOP_UNLISTED UINT64_C(0x756e6c6973746564)
#define DESKTOP_FIELD_BYTES 4096
#define DESKTOP_SCAN_DEPTH 16
#define DESKTOP_FILE_BYTES (64 * 1024)
/* A typed name token (Chrome in Google Chrome) beats a filename prefix.
 * Metadata-only matches retain their ordinary lexical strength. */
#define DESKTOP_NAME_PREFIX_BONUS 2000
/* Cinnamon's XDG_CURRENT_DESKTOP name. Its menu (libcinnamon-menu) also lists
 * entries restricted to GNOME, which is how GNOME Terminal (OnlyShowIn=GNOME;
 * Unity;) appears on Linux Mint, but never GNOME Settings panels. */
#define DESKTOP_CINNAMON "X-Cinnamon"
#define DESKTOP_GNOME "GNOME"
#define DESKTOP_GNOME_SETTINGS "gnome-control-center"
#define DESKTOP_SEPARATOR ":"
/* The synthetic parent of every search key: the name is the key's basename,
 * and metadata never enters filesystem folder context. */
#define DESKTOP_KEY_PREFIX "/Applications/"
/* Symbols desktop_named can normalize a query into: lexical_query's own
 * bound, since case folding can expand a byte. */
#define DESKTOP_QUERY_SYMBOLS (LEXICAL_QUERY_BYTES * 4)
struct entry {
    tl_desktop_entry public;
    char *key, *generic_name, *keywords;
    /* The normalized name (the key's basename), for desktop_named. */
    tl_tokenized *name_text;
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
    pthread_cond_t wake; /* CLOCK_MONOTONIC deadlines */
    pthread_t thread;
    bool started, stop, refresh;
    struct snapshot *active;
    uint64_t next_id, desktop_gen;
    tl_desktop_options options;
    /* Refresh worker only (desktop_create before it starts): the stamp of
     * what the last parse read, and when the settle parse (zero for none)
     * and the periodic parse are due, in monotonic milliseconds. */
    uint64_t stamp, settle_due, rescan_due;
    atomic_uint_least64_t parses;
    /* desktop_named's normalized query; used only by the lease holder. */
    uint32_t query_symbols[DESKTOP_QUERY_SYMBOLS];
    uint8_t query_boundaries[DESKTOP_QUERY_SYMBOLS];
    size_t query_offsets[DESKTOP_QUERY_SYMBOLS];
};
static void entry_free(struct entry *entry) {
    g_free((void *)entry->public.desktop_id);
    g_free((void *)entry->public.filename);
    g_free((void *)entry->public.name);
    g_free((void *)entry->public.icon);
    g_free((void *)entry->public.wm_class);
    g_free(entry->key);
    g_free(entry->generic_name);
    g_free(entry->keywords);
    tokenize_destroy(entry->name_text);
}
static void snapshot_destroy(struct snapshot *snapshot) {
    if (snapshot == NULL)
        return;
    struct entry *entries = vec_data(snapshot->entries);
    for (size_t i = 0; i < vec_count(snapshot->entries); i++)
        entry_free(&entries[i]);
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
    char *key =
        g_strconcat(DESKTOP_KEY_PREFIX, g_app_info_get_display_name(G_APP_INFO(info)), NULL);
    for (size_t i = strlen(DESKTOP_KEY_PREFIX); key[i] != 0; i++)
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
/* ---- visibility -------------------------------------------------------------- */
/* Whether list (NULL-terminated, NULL for an absent key) holds exactly the
 * length bytes at name. */
static bool list_names(const char *const *list, const char *name, size_t length) {
    for (size_t i = 0; list != NULL && list[i] != NULL; i++)
        if (strncmp(list[i], name, length) == 0 && list[i][length] == 0)
            return true;
    return false;
}
/* Step through colon-separated desktops: false at the end, else *name and
 * *length give the next desktop and *cursor moves past it and its colon. */
static bool next_desktop(const char **cursor, const char **name, size_t *length) {
    if (**cursor == 0)
        return false;
    *name = *cursor;
    *length = strcspn(*cursor, DESKTOP_SEPARATOR);
    *cursor += *length;
    if (**cursor != 0)
        (*cursor)++;
    return true;
}
/* Whether the colon-separated desktops include wanted. */
static bool desktops_include(const char *desktops, const char *wanted) {
    const char *const names[] = {wanted, NULL};
    const char *cursor = desktops, *name = NULL;
    size_t length = 0;
    while (next_desktop(&cursor, &name, &length))
        if (list_names(names, name, length))
            return true;
    return false;
}
bool desktop_shown_in(const char *const *only_show_in, const char *const *not_show_in,
                      const char *exec, const char *desktops) {
    if (desktops == NULL)
        desktops = "";
    if (desktops_include(desktops, DESKTOP_CINNAMON)) {
        if (exec != NULL &&
            strncmp(exec, DESKTOP_GNOME_SETTINGS, strlen(DESKTOP_GNOME_SETTINGS)) == 0)
            return false;
        if (list_names(only_show_in, DESKTOP_GNOME, strlen(DESKTOP_GNOME)))
            return true;
    }
    /* g_desktop_app_info_get_show_in, without its once-per-process cache of
     * the environment, so a test can judge several desktops. */
    const char *cursor = desktops, *name = NULL;
    size_t length = 0;
    while (next_desktop(&cursor, &name, &length)) {
        if (list_names(only_show_in, name, length))
            return true;
        if (list_names(not_show_in, name, length))
            return false;
    }
    return only_show_in == NULL;
}
/* Whether this session's menu lists info (desktop_shown_in); the popup's
 * launch check applies the same test to the entry it launches. */
static bool menu_lists(GDesktopAppInfo *info) {
    if (g_desktop_app_info_get_is_hidden(info) || g_desktop_app_info_get_nodisplay(info))
        return false;
    char **only_show_in = g_desktop_app_info_get_string_list(info, "OnlyShowIn", NULL);
    char **not_show_in = g_desktop_app_info_get_string_list(info, "NotShowIn", NULL);
    char *exec = g_desktop_app_info_get_string(info, "Exec");
    bool shown =
        desktop_shown_in((const char *const *)only_show_in, (const char *const *)not_show_in, exec,
                         g_getenv("XDG_CURRENT_DESKTOP"));
    g_free(exec);
    g_strfreev(not_show_in);
    g_strfreev(only_show_in);
    return shown;
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
/* Copy what search and launch need of a listed entry, including its
 * normalized name; TL_NOMEM when that cannot be allocated. entry owns what
 * was copied either way (entry_free). */
static tl_status fill_entry(struct entry *entry, GDesktopAppInfo *info, const char *filename,
                            const char *id, uint64_t revision) {
    entry->public.desktop_id = g_strdup(id);
    entry->public.filename = g_strdup(filename);
    entry->public.name = g_strdup(g_app_info_get_display_name(G_APP_INFO(info)));
    GIcon *icon = g_app_info_get_icon(G_APP_INFO(info));
    entry->public.icon =
        icon == NULL ? g_strdup("application-x-executable-symbolic") : g_icon_to_string(icon);
    if (entry->public.icon == NULL || strlen(entry->public.icon) >= DESKTOP_FIELD_BYTES) {
        g_free((void *)entry->public.icon);
        entry->public.icon = g_strdup("application-x-executable-symbolic");
    }
    entry->public.settings = settings_category(info);
    const char *wm_class = g_desktop_app_info_get_startup_wm_class(info);
    if (wm_class != NULL && wm_class[0] != 0 && strlen(wm_class) < DESKTOP_WM_CLASS_BYTES)
        entry->public.wm_class = g_strdup(wm_class);
    entry->key = search_key(info);
    entry->generic_name = generic_field(info);
    entry->keywords = keyword_field(info);
    entry->public.generic_name = entry->generic_name;
    entry->public.keywords = entry->keywords;
    entry->fingerprint = revision;
    entry->public.revision = revision;
    tl_status status = tokenize_create(entry->key + strlen(DESKTOP_KEY_PREFIX), &entry->name_text);
    /* A name the tokenizer rejects is still searched; it just never counts as named. */
    return status == TL_NOMEM ? status : TL_OK;
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
    if (!menu_lists(info) || name == NULL || strlen(name) >= DESKTOP_FIELD_BYTES) {
        g_object_unref(info);
        return TL_OK;
    }
    struct entry entry = {0};
    status = fill_entry(&entry, info, filename, id, revision);
    g_object_unref(info);
    if (status == TL_OK)
        status = vec_append(snapshot->entries, &entry);
    if (status != TL_OK)
        entry_free(&entry);
    return status;
}
/* ---- applications trees ---------------------------------------------------- */
/* One walk of the applications trees. Discovery and change stamps share it,
 * so a stamp covers exactly the files a discovery reads. */
struct walk {
    /* A non-directory entry named *.desktop: its filename and its path below
     * the applications directory (the desktop id before '/' becomes '-'). */
    tl_status (*entry)(void *context, const char *filename, const char *relative);
    /* A directory that could not be listed (missing or unreadable); optional. */
    void (*unlisted)(void *context, const char *directory);
    void *context;
};
/* Whether a listed name is a directory to enter: a real directory, never a
 * symlink to one. d_type answers without a system call on most filesystems. */
static bool enter_directory(const char *filename, unsigned char type) {
    if (type != DT_UNKNOWN)
        return type == DT_DIR;
    struct stat info;
    return lstat(filename, &info) == 0 && S_ISDIR(info.st_mode);
}
static tl_status walk_directory(const struct walk *walk, const char *root, const char *relative,
                                unsigned depth) {
    if (depth > DESKTOP_SCAN_DEPTH)
        return TL_LIMIT;
    char *directory = g_build_filename(root, relative, NULL);
    DIR *dir = opendir(directory);
    if (dir == NULL && walk->unlisted != NULL)
        walk->unlisted(walk->context, directory);
    g_free(directory);
    if (dir == NULL)
        return TL_OK;
    tl_status status = TL_OK;
    const struct dirent *item;
    while (status == TL_OK && (item = readdir(dir)) != NULL) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
            continue;
        char *child = g_build_filename(relative, item->d_name, NULL);
        char *filename = g_build_filename(root, child, NULL);
        if (enter_directory(filename, item->d_type))
            status = walk_directory(walk, root, child, depth + 1);
        else if (g_str_has_suffix(item->d_name, ".desktop"))
            status = walk->entry(walk->context, filename, child);
        g_free(filename);
        g_free(child);
    }
    int closed = closedir(dir);
    (void)closed; /* a read-only directory stream has nothing left to lose */
    return status;
}
/* The user's applications tree, then each system one, in XDG precedence. */
static tl_status walk_trees(const struct walk *walk) {
    char *root = g_build_filename(g_get_user_data_dir(), "applications", NULL);
    tl_status status = walk_directory(walk, root, "", 0);
    g_free(root);
    const char *const *system = g_get_system_data_dirs();
    for (size_t i = 0; status == TL_OK && system[i] != NULL; i++) {
        root = g_build_filename(system[i], "applications", NULL);
        status = walk_directory(walk, root, "", 0);
        g_free(root);
    }
    return status;
}
/* ---- change stamps ----------------------------------------------------------- */
/* Fold one observation into a stamp. Adding per-item hashes keeps the stamp
 * independent of the order directories list their entries in. */
static void stamp_add(uint64_t *stamp, const char *path, const uint64_t *fields, size_t count) {
    uint64_t hash = hashmap_hash(HASHMAP_HASH_SEED, path, strlen(path) + 1);
    *stamp += hashmap_hash(hash, fields, count * sizeof(*fields));
}
/* What add_entry reads of a file. stat follows symlinks, as add_entry does,
 * so a retargeted or rewritten target changes the stamp; ctime catches an
 * edit that kept size and restored mtime. A missing target stamps as zeros. */
static void stamp_file(uint64_t *stamp, const char *filename) {
    uint64_t fields[DESKTOP_STAMP_FIELDS] = {0};
    struct stat info;
    if (stat(filename, &info) == 0) {
        fields[0] = (uint64_t)info.st_dev;
        fields[1] = (uint64_t)info.st_ino;
        fields[2] = (uint64_t)info.st_mode;
        fields[3] = (uint64_t)info.st_size;
        fields[4] = (uint64_t)info.st_mtim.tv_sec;
        fields[5] = (uint64_t)info.st_mtim.tv_nsec;
        fields[6] = (uint64_t)info.st_ctim.tv_sec;
        fields[7] = (uint64_t)info.st_ctim.tv_nsec;
    }
    stamp_add(stamp, filename, fields, DESKTOP_STAMP_FIELDS);
}
/* An unlistable directory stamps as such, so it changes the stamp once it
 * can be listed (a missing user applications directory created later). */
static void stamp_unlisted(uint64_t *stamp, const char *directory) {
    const uint64_t marker = DESKTOP_UNLISTED;
    stamp_add(stamp, directory, &marker, 1);
}
static tl_status stamp_entry(void *context, const char *filename, const char *relative) {
    (void)relative;
    stamp_file(context, filename);
    return TL_OK;
}
static void stamp_directory(void *context, const char *directory) {
    stamp_unlisted(context, directory);
}
/* A fingerprint of everything a discovery reads except PATH: which .desktop
 * files exist where, the stat identity, size and change times of each, and
 * which directories could not be listed. About one stat per entry. */
static uint64_t applications_stamp(void) {
    uint64_t stamp = 0;
    struct walk walk = {.entry = stamp_entry, .unlisted = stamp_directory, .context = &stamp};
    /* A walk that stops (a tree deeper than discovery accepts) still yields
     * the same stamp until that tree changes; discovery fails on it anyway. */
    tl_status status = walk_trees(&walk);
    (void)status;
    return stamp;
}
/* ---- discovery ------------------------------------------------------------- */
/* A discovery stamps exactly what it reads, so a change it may have half
 * seen (a directory unreadable for a moment, a file replaced mid-walk)
 * differs from the next check's stamp even after a revert. */
struct discovery {
    struct snapshot *snapshot;
    uint64_t stamp;
};
static tl_status discover_entry(void *context, const char *filename, const char *relative) {
    struct discovery *discovery = context;
    stamp_file(&discovery->stamp, filename);
    char *id = g_strdup(relative);
    for (size_t i = 0; id[i] != 0; i++)
        if (id[i] == '/')
            id[i] = '-';
    tl_status status = add_entry(discovery->snapshot, filename, id);
    g_free(id);
    return status;
}
static void discover_unlisted(void *context, const char *directory) {
    struct discovery *discovery = context;
    stamp_unlisted(&discovery->stamp, directory);
}
static int entry_compare(const void *left, const void *right) {
    const struct entry *a = left, *b = right;
    return strcmp(a->public.desktop_id, b->public.desktop_id);
}
/* Read and parse every .desktop file into a new snapshot (not yet built).
 * *stamp receives the stamp of what was read, only on success. */
static tl_status discover(tl_desktop *desktop, struct snapshot **out, uint64_t *stamp) {
    tl_status status = snapshot_create(out);
    struct discovery discovery = {*out, 0};
    struct walk walk = {
        .entry = discover_entry, .unlisted = discover_unlisted, .context = &discovery};
    if (status == TL_OK)
        status = walk_trees(&walk);
    if (status == TL_OK && vec_count((*out)->entries) > 1)
        qsort(vec_data((*out)->entries), vec_count((*out)->entries), sizeof(struct entry),
              entry_compare);
    if (status == TL_OK)
        *stamp = discovery.stamp;
    atomic_fetch_add(&desktop->parses, 1);
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
/* ---- refresh worker ---------------------------------------------------------- */
static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
/* Sleep until the next check, a refresh request or stop. Returns false on
 * stop; *requested reports whether desktop_refresh asked for a parse. */
static bool wait_check(tl_desktop *desktop, bool *requested) {
    struct timespec deadline = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) == 0) {
        deadline.tv_sec += (time_t)(desktop->options.check_ms / 1000);
        deadline.tv_nsec += (long)(desktop->options.check_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }
    pthread_mutex_lock(&desktop->wake_lock);
    while (!desktop->stop && !desktop->refresh)
        if (pthread_cond_timedwait(&desktop->wake, &desktop->wake_lock, &deadline) != 0)
            break;
    bool stop = desktop->stop;
    *requested = desktop->refresh;
    desktop->refresh = false;
    pthread_mutex_unlock(&desktop->wake_lock);
    return !stop;
}
/* Whether this check parses: on request, when the trees differ from what the
 * last parse read, once more settle_ms after the last change (an install may
 * add a TryExec binary just after its entry) and every rescan_ms. *checked
 * receives this check's stamp. */
static bool parse_due(tl_desktop *desktop, bool requested, uint64_t *checked) {
    uint64_t now = monotonic_ms();
    *checked = applications_stamp();
    bool changed = *checked != desktop->stamp;
    bool settled = !changed && desktop->settle_due != 0 && now >= desktop->settle_due;
    if (changed)
        desktop->settle_due = now + desktop->options.settle_ms;
    else if (settled)
        desktop->settle_due = 0;
    if (!requested && !changed && !settled && now < desktop->rescan_due)
        return false;
    desktop->rescan_due = now + desktop->options.rescan_ms;
    return true;
}
/* Parse every entry and publish the result unless nothing changed. The
 * stamp becomes that of what the parse read; a failed refresh keeps the
 * active snapshot and records the check's stamp instead, so a tree discovery
 * cannot read is not parsed again until it changes. */
static void refresh_snapshot(tl_desktop *desktop, uint64_t checked) {
    struct snapshot *next = NULL;
    uint64_t read = checked;
    tl_status status = discover(desktop, &next, &read);
    desktop->stamp = status == TL_OK ? read : checked;
    if (status == TL_OK && unchanged(desktop->active, next)) {
        snapshot_destroy(next);
        return;
    }
    if (status == TL_OK)
        status = build(desktop, next);
    if (status != TL_OK) {
        snapshot_destroy(next);
        return;
    }
    pthread_mutex_lock(&desktop->lock);
    struct snapshot *old = desktop->active;
    desktop->active = next;
    desktop->desktop_gen++;
    pthread_mutex_unlock(&desktop->lock);
    snapshot_destroy(old);
}
static void *refresh_worker(void *context) {
    tl_desktop *desktop = context;
    tl_thread_name("tl-desktop");
    bool requested = false;
    uint64_t checked = 0;
    while (wait_check(desktop, &requested))
        if (parse_due(desktop, requested, &checked))
            refresh_snapshot(desktop, checked);
    return NULL;
}
/* Options with defaults for zero fields. */
static tl_desktop_options with_defaults(const tl_desktop_options *options) {
    tl_desktop_options out = {0};
    if (options != NULL)
        out = *options;
    if (out.check_ms == 0)
        out.check_ms = DESKTOP_CHECK_MS;
    if (out.settle_ms == 0)
        out.settle_ms = DESKTOP_SETTLE_MS;
    if (out.rescan_ms == 0)
        out.rescan_ms = DESKTOP_RESCAN_MS;
    return out;
}
/* A condition variable timed on CLOCK_MONOTONIC, immune to clock changes. */
static tl_status init_wake(pthread_cond_t *wake) {
    pthread_condattr_t attributes;
    if (pthread_condattr_init(&attributes) != 0)
        return TL_IO;
    int code = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (code == 0)
        code = pthread_cond_init(wake, &attributes);
    pthread_condattr_destroy(&attributes);
    return code == 0 ? TL_OK : TL_IO;
}
tl_status desktop_create(const tl_desktop_options *options, tl_desktop **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_desktop *desktop = calloc(1, sizeof(*desktop));
    if (desktop == NULL)
        return TL_NOMEM;
    desktop->desktop_gen = 1;
    desktop->options = with_defaults(options);
    atomic_init(&desktop->parses, 0);
    if (pthread_mutex_init(&desktop->lock, NULL) != 0) {
        free(desktop);
        return TL_IO;
    }
    if (pthread_mutex_init(&desktop->wake_lock, NULL) != 0) {
        pthread_mutex_destroy(&desktop->lock);
        free(desktop);
        return TL_IO;
    }
    if (init_wake(&desktop->wake) != TL_OK) {
        pthread_mutex_destroy(&desktop->wake_lock);
        pthread_mutex_destroy(&desktop->lock);
        free(desktop);
        return TL_IO;
    }
    uint32_t nonce = 0;
    tl_status status =
        getrandom(&nonce, sizeof(nonce), 0) == (ssize_t)sizeof(nonce) ? TL_OK : TL_IO;
    desktop->next_id = DESKTOP_ID_BASE | ((uint64_t)(nonce & 0x3fffffffU) << 32);
    if (status == TL_OK) {
        desktop->rescan_due = monotonic_ms() + desktop->options.rescan_ms;
        status = discover(desktop, &desktop->active, &desktop->stamp);
    }
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
uint64_t desktop_parses(tl_desktop *desktop) {
    return desktop == NULL ? 0 : atomic_load(&desktop->parses);
}
tl_status desktop_query(tl_desktop *desktop, const char *query, tl_result *results, size_t capacity,
                        size_t *count) {
    return desktop_query_boosted(desktop, query, NULL, 0, results, capacity, count);
}
tl_status desktop_query_boosted(tl_desktop *desktop, const char *query,
                                const tl_lexical_boost *boosts, size_t boost_count,
                                tl_result *results, size_t capacity, size_t *count) {
    if (count == NULL)
        return TL_INVALID;
    *count = 0;
    if (desktop == NULL || query == NULL || results == NULL || capacity == 0 ||
        capacity > LEXICAL_MAX_RESULTS)
        return TL_INVALID;
    /* Always replace the workspace's boosts, so a query never ranks with
     * boosts attached for an earlier one. */
    tl_status status = lexical_workspace_boost(desktop->active->workspace, boosts, boost_count);
    if (status != TL_OK || query[0] == 0)
        return status;
    return lexical_query(desktop->active->engine, desktop->active->workspace, query, results,
                         capacity, count);
}
/* The separators lexical_query splits a query's words at. */
static bool query_space(uint32_t symbol) {
    return symbol == ' ' || symbol == '\t' || symbol == '\n' || symbol == '\r';
}
/* Whether every word of the normalized query starts name, a word of it or
 * its initials (prefix_score's keys of a name without parent folders). */
static bool names_entry(const tl_tokenized *name, tl_text query) {
    if (name == NULL)
        return false;
    size_t words = 0;
    for (size_t start = 0; start < query.length;) {
        if (query_space(query.symbols[start])) {
            start++;
            continue;
        }
        size_t end = start;
        while (end < query.length && !query_space(query.symbols[end]))
            end++;
        tl_text word = {.symbols = query.symbols + start,
                        .boundaries = query.boundaries + start,
                        .length = end - start};
        int best = 0;
        bool complete = false;
        if (prefix_score(tokenize_view(name), word, &best, &complete) != TL_OK ||
            best < PREFIX_INITIALS_SCORE)
            return false;
        words++;
        start = end;
    }
    return words != 0;
}
static const struct entry *entry_by_id(const tl_desktop *desktop, uint64_t id) {
    const struct entry *entries = vec_const_data(desktop->active->entries);
    size_t low = 0, high = vec_count(desktop->active->entries);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (entries[middle].public.id < id)
            low = middle + 1;
        else
            high = middle;
    }
    return low < vec_count(desktop->active->entries) && entries[low].public.id == id ? &entries[low]
                                                                                     : NULL;
}
tl_status desktop_named(tl_desktop *desktop, const char *query, const tl_result *results,
                        size_t count, bool *named) {
    if (count == 0)
        return TL_OK;
    if (named == NULL)
        return TL_INVALID;
    memset(named, 0, count * sizeof(*named));
    if (desktop == NULL || query == NULL || results == NULL)
        return TL_INVALID;
    size_t length = strnlen(query, LEXICAL_QUERY_BYTES + 1);
    if (length > LEXICAL_QUERY_BYTES)
        return TL_LIMIT;
    tl_text text = {0};
    tl_status status =
        tokenize_into(query, length, desktop->query_symbols, desktop->query_boundaries,
                      desktop->query_offsets, DESKTOP_QUERY_SYMBOLS, &text);
    if (status != TL_OK)
        return status;
    for (size_t i = 0; i < count; i++) {
        const struct entry *entry = entry_by_id(desktop, results[i].id);
        named[i] = entry != NULL && names_entry(entry->name_text, text);
    }
    return TL_OK;
}
const tl_desktop_entry *desktop_resolve(const tl_desktop *desktop, uint64_t id) {
    if (desktop == NULL)
        return NULL;
    const struct entry *entry = entry_by_id(desktop, id);
    return entry == NULL ? NULL : &entry->public;
}
uint64_t desktop_gen(const tl_desktop *desktop) {
    return desktop == NULL ? 0 : desktop->desktop_gen;
}
size_t desktop_count(const tl_desktop *desktop) {
    return desktop == NULL ? 0 : vec_count(desktop->active->entries);
}
const tl_desktop_entry *desktop_entry(const tl_desktop *desktop, size_t position) {
    if (desktop == NULL || position >= desktop_count(desktop))
        return NULL;
    const struct entry *entries = vec_const_data(desktop->active->entries);
    return &entries[position].public;
}
