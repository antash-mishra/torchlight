/* Flatpak CLI adapter: bounded background metadata reads and exact source checks. */
#include "torchlight/software_backend.h"
#include "torchlight/hashmap.h"
#include "torchlight/json.h"
#include "torchlight/vec.h"
#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define INSTALLATIONS_MAX 64
#define SOURCE_CONFIG_BYTES (1024U * 1024U)
struct installation {
    char id[SOFTWARE_PART_BYTES];
    char *path;
};
static uint64_t stat_hash(uint64_t hash, const char *path) {
    struct stat info;
    if (stat(path, &info) != 0)
        return hashmap_hash(hash, "absent", 6);
    uint64_t fields[] = {(uint64_t)info.st_dev,          (uint64_t)info.st_ino,
                         (uint64_t)info.st_size,         (uint64_t)info.st_mtim.tv_sec,
                         (uint64_t)info.st_mtim.tv_nsec, (uint64_t)info.st_ctim.tv_sec,
                         (uint64_t)info.st_ctim.tv_nsec};
    return hashmap_hash(hash, fields, sizeof(fields));
}
static tl_status add_installation(tl_vec *list, const char *id, const char *path) {
    if (id == NULL || path == NULL || path[0] != '/' || strlen(id) >= SOFTWARE_PART_BYTES)
        return TL_INVALID;
    tl_software_ref check = {
        .remote = "check", .app_id = "org.example.Check", .arch = "x86_64", .branch = "stable"};
    memcpy(check.installation, id, strlen(id) + 1);
    if (!software_ref_valid(&check))
        return TL_INVALID;
    if (vec_count(list) >= INSTALLATIONS_MAX)
        return TL_LIMIT;
    struct installation item = {.path = g_strdup(path)};
    memcpy(item.id, id, strlen(id) + 1);
    tl_status status = vec_append(list, &item);
    if (status != TL_OK)
        g_free(item.path);
    return status;
}
static char *config_directory(void) {
    const char *override = getenv("FLATPAK_CONFIG_DIR");
    return g_build_filename(override == NULL ? "/etc/flatpak" : override, "installations.d", NULL);
}
static tl_status named_installations(tl_vec *list) {
    char *directory = config_directory();
    GDir *dir = g_dir_open(directory, 0, NULL);
    if (dir == NULL) {
        g_free(directory);
        return TL_OK;
    }
    const char *name;
    tl_status status = TL_OK;
    while (status == TL_OK && (name = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(name, ".conf"))
            continue;
        char *path = g_build_filename(directory, name, NULL);
        struct stat info;
        GKeyFile *file = g_key_file_new();
        if (stat(path, &info) != 0 || info.st_size > SOURCE_CONFIG_BYTES ||
            !g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
            status = TL_IO;
        g_free(path);
        gsize count = 0;
        char **groups = g_key_file_get_groups(file, &count);
        for (gsize i = 0; i < count && status == TL_OK; i++) {
            size_t length = strlen(groups[i]);
            if (!g_str_has_prefix(groups[i], "Installation \"") || length < 16 ||
                groups[i][length - 1] != '"')
                continue;
            char *id = g_strdup_printf("system:%.*s", (int)(length - 15), groups[i] + 14);
            char *root = g_key_file_get_string(file, groups[i], "Path", NULL);
            status = add_installation(list, id, root);
            g_free(root);
            g_free(id);
        }
        g_strfreev(groups);
        g_key_file_unref(file);
    }
    g_dir_close(dir);
    g_free(directory);
    return status;
}
static void installations_destroy(tl_vec *list) {
    struct installation *items = vec_data(list);
    for (size_t i = 0; i < vec_count(list); i++)
        g_free(items[i].path);
    vec_destroy(list);
}
static tl_status installations(tl_vec **out) {
    tl_status status = vec_create(sizeof(struct installation), out);
    const char *user = getenv("FLATPAK_USER_DIR");
    const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char *root = user != NULL   ? g_strdup(user)
                 : data != NULL ? g_build_filename(data, "flatpak", NULL)
                 : home != NULL ? g_build_filename(home, ".local/share/flatpak", NULL)
                                : NULL;
    if (status == TL_OK)
        status = add_installation(*out, "user", root);
    g_free(root);
    const char *system = getenv("FLATPAK_SYSTEM_DIR");
    if (status == TL_OK)
        status = add_installation(*out, "system", system == NULL ? "/var/lib/flatpak" : system);
    if (status == TL_OK)
        status = named_installations(*out);
    if (status != TL_OK) {
        installations_destroy(*out);
        *out = NULL;
    }
    return status;
}
static char *scope_option(const char *id) {
    return strcmp(id, "user") == 0     ? g_strdup("--user")
           : strcmp(id, "system") == 0 ? g_strdup("--system")
                                       : g_strdup_printf("--installation=%s", id + 7);
}
static tl_status source_revision(const struct installation *scope, const char *remote,
                                 uint64_t *out) {
    char *path = g_build_filename(scope->path, "repo/config", NULL);
    struct stat info;
    GKeyFile *file = g_key_file_new();
    tl_status status = TL_STATE;
    if (stat(path, &info) == 0 && info.st_size <= SOURCE_CONFIG_BYTES &&
        g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL)) {
        char *group = g_strdup_printf("remote \"%s\"", remote);
        char *url = g_key_file_get_string(file, group, "url", NULL);
        bool disabled = g_key_file_get_boolean(file, group, "xa.disable", NULL) != 0;
        if (url != NULL && url[0] != 0 && !disabled) {
            gsize length = 0;
            char *text = g_key_file_to_data(file, &length, NULL);
            uint64_t hash = stat_hash(HASHMAP_HASH_SEED, path);
            /* Config replacement, trust-key changes and reinstallations invalidate actions. */
            char *key = g_strdup_printf("%s/repo/%s.trustedkeys.gpg", scope->path, remote);
            hash = stat_hash(hash, key);
            *out = hashmap_hash(hash, text, length);
            g_free(key);
            g_free(text);
            status = TL_OK;
        }
        g_free(url);
        g_free(group);
    }
    g_key_file_unref(file);
    g_free(path);
    return status;
}
static bool copy_field(char *out, size_t capacity, const char *text) {
    size_t length = strlen(text);
    if (length >= capacity)
        return false;
    memcpy(out, text, length + 1);
    return true;
}
static bool parse_ref(char *text, tl_software_ref *ref) {
    if (strncmp(text, "app/", 4) != 0)
        return false;
    char *app = text + 4, *arch = strchr(app, '/');
    if (arch == NULL)
        return false;
    *arch++ = 0;
    char *branch = strchr(arch, '/');
    if (branch == NULL)
        return false;
    *branch++ = 0;
    return copy_field(ref->app_id, sizeof(ref->app_id), app) &&
           copy_field(ref->arch, sizeof(ref->arch), arch) &&
           copy_field(ref->branch, sizeof(ref->branch), branch) && software_ref_valid(ref);
}
static int compare_string(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static tl_status installed_lines(char *text, tl_vec **out) {
    tl_status status = vec_create(sizeof(char *), out);
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line != NULL && status == TL_OK;
         line = strtok_r(NULL, "\n", &save)) {
        if (vec_count(*out) >= SOFTWARE_MAX_ENTRIES)
            return TL_LIMIT;
        char *ref = strchr(line, '\t');
        if (ref == NULL)
            return TL_INVALID;
        ref++;
        /* list omits the kind prefix on Flatpak 1.14; normalize both forms. */
        if (strncmp(ref, "app/", 4) == 0)
            memmove(ref, ref + 4, strlen(ref + 4) + 1);
        status = vec_append(*out, &line);
    }
    if (status == TL_OK && vec_count(*out) > 1)
        qsort(vec_data(*out), vec_count(*out), sizeof(char *), compare_string);
    return status;
}
static tl_status add_lines(tl_software *index, char *text, const struct installation *scope,
                           const char *remote, uint64_t revision, const tl_vec *installed,
                           tl_process_budget *budget) {
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        if (budget->cancel != NULL && atomic_load(budget->cancel))
            return TL_CANCELLED;
        if (process_now() >= budget->deadline_ms)
            return TL_LIMIT;
        char *name = strchr(line, '\t');
        if (name == NULL)
            return TL_INVALID;
        *name++ = 0;
        char *summary = strchr(name, '\t');
        if (summary != NULL)
            *summary++ = 0;
        else
            summary = "";
        tl_software_entry entry = {.name = name, .summary = summary, .source_revision = revision};
        if (!copy_field(entry.ref.installation, sizeof(entry.ref.installation), scope->id) ||
            !copy_field(entry.ref.remote, sizeof(entry.ref.remote), remote))
            return TL_LIMIT;
        if (!parse_ref(line, &entry.ref))
            return TL_INVALID;
        char *key = g_strdup_printf("%s\t%s/%s/%s", remote, entry.ref.app_id, entry.ref.arch,
                                    entry.ref.branch);
        entry.installed = vec_count(installed) != 0 &&
                          bsearch(&key, vec_const_data(installed), vec_count(installed),
                                  sizeof(char *), compare_string) != NULL;
        g_free(key);
        if (entry.name[0] == 0)
            entry.name = entry.ref.app_id;
        tl_status status = software_add(index, &entry);
        if (status != TL_OK)
            return status;
    }
    return TL_OK;
}
static tl_status read_remote(tl_software *index, const struct installation *scope,
                             const char *option, const char *remote, const tl_vec *installed,
                             bool download, tl_process_budget *budget) {
    uint64_t revision = 0;
    tl_status status = source_revision(scope, remote, &revision);
    if (status == TL_STATE)
        status = TL_IO;
    char *text = NULL;
    if (status == TL_OK && download) {
        const char *update[] = {"flatpak", option, "update", "--appstream", remote, NULL};
        status = process_capture(update, budget, &text);
        process_output_destroy(text);
        text = NULL;
    }
    if (status == TL_OK) {
        const char *args[] = {"flatpak",  option,  "remote-ls",
                              "--cached", "--app", "--columns=ref:full,name:full,description:full",
                              remote,     NULL};
        status = process_capture(args, budget, &text);
    }
    if (status == TL_OK)
        status = add_lines(index, text, scope, remote, revision, installed, budget);
    process_output_destroy(text);
    return status;
}
static tl_status read_scope(tl_software *index, const struct installation *scope, bool download,
                            tl_process_budget *budget, size_t *sources) {
    char *option = scope_option(scope->id), *remotes = NULL, *installed = NULL;
    tl_vec *lines = NULL;
    const char *list[] = {"flatpak", option, "remotes", "--columns=name:full,options:full", NULL};
    tl_status status = process_capture(list, budget, &remotes);
    const char *apps[] = {"flatpak", option, "list", "--app", "--columns=origin:full,ref:full",
                          NULL};
    if (status == TL_OK)
        status = process_capture(apps, budget, &installed);
    if (status == TL_OK)
        status = installed_lines(installed, &lines);
    char *save = NULL;
    for (char *line = status == TL_OK ? strtok_r(remotes, "\n", &save) : NULL;
         line != NULL && status == TL_OK; line = strtok_r(NULL, "\n", &save)) {
        char *flags = strchr(line, '\t');
        if (flags != NULL)
            *flags++ = 0;
        if (flags != NULL &&
            (strstr(flags, "disabled") != NULL || strstr(flags, "no-enumerate") != NULL))
            continue;
        status = read_remote(index, scope, option, line, lines, download, budget);
        if (status == TL_OK)
            (*sources)++;
    }
    vec_destroy(lines);
    process_output_destroy(installed);
    process_output_destroy(remotes);
    g_free(option);
    return status;
}
static tl_status build(void *context, bool download, tl_process_budget *budget, tl_software **out) {
    (void)context;
    *out = NULL;
    tl_vec *scopes = NULL;
    tl_software *index = NULL;
    tl_status status = installations(&scopes);
    if (status == TL_OK)
        status = software_create(&index);
    struct installation *items = vec_data(scopes);
    size_t sources = 0;
    for (size_t i = 0; i < vec_count(scopes) && status == TL_OK; i++)
        status = read_scope(index, &items[i], download, budget, &sources);
    if (status == TL_OK && sources == 0)
        status = TL_STATE;
    if (status == TL_OK)
        status = software_finish(index);
    installations_destroy(scopes);
    if (status == TL_OK)
        *out = index;
    else
        software_destroy(index);
    return status;
}
static uint64_t stamp(void *context) {
    (void)context;
    tl_vec *scopes = NULL;
    if (installations(&scopes) != TL_OK)
        return 0;
    uint64_t hash = HASHMAP_HASH_SEED;
    struct installation *items = vec_data(scopes);
    for (size_t i = 0; i < vec_count(scopes); i++) {
        hash = hashmap_hash(hash, items[i].id, strlen(items[i].id));
        char *config = g_build_filename(items[i].path, "repo/config", NULL);
        char *changed = g_build_filename(items[i].path, ".changed", NULL);
        hash = stat_hash(stat_hash(hash, config), changed);
        g_free(config);
        g_free(changed);
    }
    installations_destroy(scopes);
    return hash;
}
tl_software_backend software_backend_flatpak(void) {
    return (tl_software_backend){.build = build, .stamp = stamp};
}
tl_status software_backend_validate(const tl_software_ref *ref, uint64_t revision,
                                    tl_process_budget *budget) {
    if (!software_ref_valid(ref) || budget == NULL)
        return TL_INVALID;
    tl_vec *scopes = NULL;
    tl_status status = installations(&scopes);
    struct installation *items = vec_data(scopes);
    const struct installation *scope = NULL;
    for (size_t i = 0; i < vec_count(scopes); i++)
        if (strcmp(items[i].id, ref->installation) == 0)
            scope = &items[i];
    uint64_t current = 0;
    if (status == TL_OK)
        status = scope == NULL ? TL_STATE : source_revision(scope, ref->remote, &current);
    if (status == TL_OK && current != revision)
        status = TL_STATE;
    if (status == TL_OK) {
        char *option = scope_option(ref->installation);
        char *reference = g_strdup_printf("app/%s/%s/%s", ref->app_id, ref->arch, ref->branch);
        const char *args[] = {"flatpak",    option,      "remote-info", "--cached",
                              "--show-ref", ref->remote, reference,     NULL};
        char *text = NULL;
        status = process_capture(args, budget, &text);
        if (status == TL_OK && strcmp(g_strchomp(text), reference) != 0)
            status = TL_STATE;
        process_output_destroy(text);
        g_free(reference);
        g_free(option);
    }
    installations_destroy(scopes);
    return status;
}
