/* Resolve generic XDG data defaults and parse a "key = value" configuration
 * file without Torchlight domain dependencies. */
#include "torchlight/config.h"
#include "torchlight/vec.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
struct config_entry {
    char *key, *value;
};
struct tl_config {
    char *database, *state_directory, *file;
    tl_vec *entries;
};
/* Concatenate count strings into a new allocation with checked arithmetic. */
static tl_status join(const char *const *parts, size_t count, char **out) {
    size_t capacity = 1;
    for (size_t i = 0; i < count; i++) {
        size_t length = strlen(parts[i]);
        if (capacity > SIZE_MAX - length)
            return TL_LIMIT;
        capacity += length;
    }
    char *path = malloc(capacity);
    if (path == NULL)
        return TL_NOMEM;
    size_t offset = 0;
    for (size_t i = 0; i < count; i++) {
        size_t length = strlen(parts[i]);
        memcpy(path + offset, parts[i], length);
        offset += length;
    }
    path[offset] = 0;
    *out = path;
    return TL_OK;
}
static tl_status make_parents(char *path) {
    for (char *cursor = path + 1; *cursor != 0; cursor++) {
        if (*cursor != '/')
            continue;
        *cursor = 0;
        int code = mkdir(path, 0700);
        *cursor = '/';
        if (code != 0 && errno != EEXIST)
            return TL_IO;
    }
    return TL_OK;
}
/* Base directory from an absolute environment variable, else HOME + fallback. */
static tl_status xdg_base(const char *variable, const char *fallback, const char **base,
                          const char **middle) {
    *base = getenv(variable);
    *middle = "/";
    if (*base == NULL || (*base)[0] != '/') {
        *base = getenv("HOME");
        *middle = fallback;
    }
    return *base == NULL || (*base)[0] != '/' ? TL_INVALID : TL_OK;
}
static tl_status default_database(const char *application, char **out) {
    const char *base = NULL, *middle = NULL;
    tl_status status = xdg_base("XDG_DATA_HOME", "/.local/share/", &base, &middle);
    const char *parts[] = {base, middle, application, "/catalog.db"};
    if (status == TL_OK)
        status = join(parts, 4, out);
    if (status == TL_OK)
        status = make_parents(*out);
    if (status != TL_OK) {
        free(*out);
        *out = NULL;
    }
    return status;
}
static tl_status select_database(tl_config *config, const char *application,
                                 const char *database_override) {
    if (database_override != NULL) {
        config->database = realpath(database_override, NULL);
        if (config->database != NULL)
            return TL_OK;
        if (errno == ENOMEM)
            return TL_NOMEM;
        char *copy = strdup(database_override);
        if (copy == NULL)
            return TL_NOMEM;
        char *slash = strrchr(copy, '/');
        const char *name = slash == NULL ? copy : slash + 1;
        char *parent = NULL;
        if (slash == NULL)
            parent = realpath(".", NULL);
        else {
            *slash = 0;
            parent = realpath(copy[0] == 0 ? "/" : copy, NULL);
        }
        const char *parts[] = {parent, parent != NULL && strcmp(parent, "/") == 0 ? "" : "/", name};
        tl_status status = parent == NULL ? TL_IO
                           : name[0] == 0 ? TL_INVALID
                                          : join(parts, 3, &config->database);
        free(parent);
        free(copy);
        return status;
    }
    tl_status status = default_database(application, &config->database);
    if (status != TL_OK)
        return status;
    char *slash = strrchr(config->database, '/');
    *slash = 0;
    config->state_directory = realpath(config->database, NULL);
    *slash = '/';
    if (config->state_directory == NULL)
        return TL_IO;
    free(config->database);
    config->database = NULL;
    const char *parts[] = {config->state_directory, "/catalog.db"};
    status = join(parts, 2, &config->database);
    if (status == TL_OK) {
        char *canonical = realpath(config->database, NULL);
        if (canonical != NULL) {
            free(config->database);
            config->database = canonical;
        } else if (errno == ENOMEM)
            status = TL_NOMEM;
    }
    return status;
}
/* Pick the file to read: the override (must exist), or the default if any. */
static tl_status select_file(tl_config *config, const char *application,
                             const char *file_override) {
    if (file_override != NULL) {
        config->file = strdup(file_override);
        return config->file == NULL ? TL_NOMEM : TL_OK;
    }
    const char *base = NULL, *middle = NULL;
    if (xdg_base("XDG_CONFIG_HOME", "/.config/", &base, &middle) != TL_OK)
        return TL_OK; /* no usable location: run without a file */
    const char *parts[] = {base, middle, application, "/config"};
    tl_status status = join(parts, 4, &config->file);
    struct stat info;
    if (status == TL_OK && stat(config->file, &info) != 0) {
        free(config->file);
        config->file = NULL;
        status = errno == ENOENT || errno == ENOTDIR ? TL_OK : TL_IO;
    }
    return status;
}
static char *trim(char *text) {
    while (*text == ' ' || *text == '\t')
        text++;
    size_t length = strlen(text);
    while (length > 0 && strchr(" \t\r\n", text[length - 1]) != NULL)
        text[--length] = 0;
    return text;
}
static bool valid_key(const char *key) {
    if (key[0] == 0)
        return false;
    for (const char *c = key; *c != 0; c++) {
        if ((*c < 'a' || *c > 'z') && *c != '_')
            return false;
    }
    return true;
}
static tl_status add_entry(tl_config *config, const char *key, const char *value) {
    struct config_entry entry = {strdup(key), NULL};
    tl_status status = TL_OK;
    if (strncmp(value, "~/", 2) == 0) {
        const char *home = getenv("HOME");
        const char *parts[] = {home == NULL ? "" : home, value + 1};
        status = home == NULL || home[0] != '/' ? TL_INVALID : join(parts, 2, &entry.value);
    } else {
        entry.value = strdup(value);
    }
    if (status == TL_OK && (entry.key == NULL || entry.value == NULL))
        status = TL_NOMEM;
    if (status == TL_OK)
        status = vec_append(config->entries, &entry);
    if (status != TL_OK) {
        free(entry.key);
        free(entry.value);
    }
    return status;
}
/* Parse one line in place; blank and comment lines add nothing. */
static tl_status parse_line(tl_config *config, char *line, size_t length) {
    if (strlen(line) != length)
        return TL_INVALID; /* embedded NUL */
    char *text = trim(line);
    if (text[0] == 0 || text[0] == '#')
        return TL_OK;
    char *equals = strchr(text, '=');
    if (equals == NULL)
        return TL_INVALID;
    *equals = 0;
    char *key = trim(text), *value = trim(equals + 1);
    if (!valid_key(key) || value[0] == 0)
        return TL_INVALID;
    return add_entry(config, key, value);
}
static tl_status parse_file(tl_config *config, size_t *error_line) {
    FILE *stream = fopen(config->file, "r");
    if (stream == NULL)
        return TL_IO;
    char *line = NULL;
    size_t capacity = 0, number = 0;
    ssize_t length = 0;
    tl_status status = TL_OK;
    while (status == TL_OK && (length = getline(&line, &capacity, stream)) >= 0) {
        number++;
        status = parse_line(config, line, (size_t)length);
    }
    if (status == TL_OK && ferror(stream))
        status = TL_IO;
    if (status == TL_INVALID && error_line != NULL)
        *error_line = number;
    free(line);
    fclose(stream);
    return status;
}
tl_status config_create(const char *application, const char *database_override,
                        const char *file_override, size_t *error_line, tl_config **out) {
    if (error_line != NULL)
        *error_line = 0;
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (application == NULL || application[0] == 0 || strchr(application, '/') != NULL ||
        strcmp(application, ".") == 0 || strcmp(application, "..") == 0 ||
        (database_override != NULL && database_override[0] == 0) ||
        (file_override != NULL && file_override[0] == 0))
        return TL_INVALID;
    tl_config *config = calloc(1, sizeof(*config));
    if (config == NULL)
        return TL_NOMEM;
    tl_status status = vec_create(sizeof(struct config_entry), &config->entries);
    if (status == TL_OK)
        status = select_database(config, application, database_override);
    if (status == TL_OK)
        status = select_file(config, application, file_override);
    if (status == TL_OK && config->file != NULL)
        status = parse_file(config, error_line);
    if (status != TL_OK) {
        config_destroy(config);
        return status;
    }
    *out = config;
    return TL_OK;
}
void config_destroy(tl_config *config) {
    if (config == NULL)
        return;
    struct config_entry *entries = vec_data(config->entries);
    for (size_t i = 0; i < vec_count(config->entries); i++) {
        free(entries[i].key);
        free(entries[i].value);
    }
    vec_destroy(config->entries);
    free(config->database);
    free(config->state_directory);
    free(config->file);
    free(config);
}
const char *config_database(const tl_config *config) {
    return config == NULL ? NULL : config->database;
}
const char *config_state_directory(const tl_config *config) {
    return config == NULL ? NULL : config->state_directory;
}
const char *config_file(const tl_config *config) {
    return config == NULL ? NULL : config->file;
}
size_t config_entry_count(const tl_config *config) {
    return config == NULL ? 0 : vec_count(config->entries);
}
tl_status config_entry(const tl_config *config, size_t index, const char **key,
                       const char **value) {
    if (config == NULL || key == NULL || value == NULL || index >= vec_count(config->entries))
        return TL_INVALID;
    const struct config_entry *entry =
        (const struct config_entry *)vec_const_data(config->entries) + index;
    *key = entry->key;
    *value = entry->value;
    return TL_OK;
}
