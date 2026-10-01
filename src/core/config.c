/* Resolve generic XDG data defaults without Torchlight domain dependencies. */
#include "torchlight/config.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
struct tl_config {
    char *database, *state_directory;
};
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
static tl_status default_database(const char *application, char **out) {
    const char *base = getenv("XDG_DATA_HOME"), *middle = "/";
    if (base == NULL || base[0] != '/') {
        base = getenv("HOME");
        middle = "/.local/share/";
    }
    if (base == NULL || base[0] != '/')
        return TL_INVALID;
    const char *suffix = "/catalog.db";
    size_t capacity = strlen(base),
           parts[] = {strlen(middle), strlen(application), strlen(suffix), 1};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (capacity > SIZE_MAX - parts[i])
            return TL_LIMIT;
        capacity += parts[i];
    }
    char *path = malloc(capacity);
    if (path == NULL)
        return TL_NOMEM;
    size_t offset = 0;
    const char *strings[] = {base, middle, application, suffix};
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
        size_t length = strlen(strings[i]);
        memcpy(path + offset, strings[i], length);
        offset += length;
    }
    path[offset] = 0;
    tl_status status = make_parents(path);
    if (status != TL_OK) {
        free(path);
        return status;
    }
    *out = path;
    return TL_OK;
}
tl_status config_create(const char *application, const char *database_override, tl_config **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (application == NULL || application[0] == 0 || strchr(application, '/') != NULL ||
        strcmp(application, ".") == 0 || strcmp(application, "..") == 0 ||
        (database_override != NULL && database_override[0] == 0))
        return TL_INVALID;
    tl_config *config = calloc(1, sizeof(*config));
    if (config == NULL)
        return TL_NOMEM;
    tl_status status = TL_OK;
    if (database_override != NULL) {
        config->database = strdup(database_override);
        if (config->database == NULL)
            status = TL_NOMEM;
    } else {
        status = default_database(application, &config->database);
        if (status == TL_OK) {
            char *slash = strrchr(config->database, '/');
            if (slash == NULL) {
                status = TL_INVALID;
                goto cleanup;
            }
            *slash = 0;
            config->state_directory = realpath(config->database, NULL);
            *slash = '/';
            if (config->state_directory == NULL)
                status = TL_IO;
        }
    }
    if (status != TL_OK)
        goto cleanup;
    *out = config;
    return TL_OK;
cleanup:
    config_destroy(config);
    return status;
}
void config_destroy(tl_config *config) {
    if (config == NULL)
        return;
    free(config->database);
    free(config->state_directory);
    free(config);
}
const char *config_database(const tl_config *config) {
    return config == NULL ? NULL : config->database;
}
const char *config_state_directory(const tl_config *config) {
    return config == NULL ? NULL : config->state_directory;
}
