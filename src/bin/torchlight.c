/* Local index/query CLI; domain algorithms live in reusable modules. */
#include "torchlight/client.h"
#include "torchlight/config.h"
#include "torchlight/crawl.h"
#include "torchlight/lexical.h"
#include "torchlight/store.h"
#include "torchlight/tokenize.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct options {
    const char *command, *database, *config, *socket_path;
    const char **paths; /* positional arguments, borrowed from argv */
    size_t path_count, limit;
    bool null_output, json_output;
};
/* Owned root/allow paths; unresolved absolute spellings are not canonical. */
struct path_list {
    char **paths;
    bool *keep, *scanned;
    size_t count;
    bool has_unresolved_paths;
};
struct scan_context {
    tl_store *store;
    const char *database;
    size_t count, unreadable;
    /* crawl_run also returns TL_IO for filesystem failures, so retain the
     * callback's error separately to avoid treating SQL failures as offline roots. */
    tl_status callback_status;
};
static void usage(void) {
    fputs("Usage: torchlight index [--db PATH] [--config PATH] [ROOT...]\n"
          "       torchlight query [--db PATH] [--limit 1..1000] [--null] QUERY\n"
          "       torchlight query [--socket PATH] [--json | --null] [--limit N] QUERY\n"
          "       torchlight status|reconcile|history-clear [--socket PATH]\n"
          "       torchlight resolve [--socket PATH] [--json | --null] FILE_ID\n"
          "       torchlight record [--socket PATH] FILE_ID EVENT_ID [SEARCH_ID]\n"
          "Without ROOT, index syncs the catalog to the configured roots.\n",
          stderr);
}
static bool parse_limit(const char *value, size_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long limit = strtoul(value, &end, 10);
    if (errno != 0 || value[0] < '0' || value[0] > '9' || *end != 0 || limit == 0 ||
        limit > LEXICAL_MAX_RESULTS)
        return false;
    *out = (size_t)limit;
    return true;
}
static tl_status parse(int argc, char **argv, struct options *options) {
    if (argc < 2)
        return TL_INVALID;
    *options = (struct options){.command = argv[1], .limit = 10};
    bool index = strcmp(options->command, "index") == 0;
    if (!index && strcmp(options->command, "query") != 0 &&
        strcmp(options->command, "status") != 0 && strcmp(options->command, "resolve") != 0 &&
        strcmp(options->command, "record") != 0 && strcmp(options->command, "reconcile") != 0 &&
        strcmp(options->command, "history-clear") != 0)
        return TL_INVALID;
    options->paths = calloc((size_t)argc, sizeof(char *));
    if (options->paths == NULL)
        return TL_NOMEM;
    bool positional = false;
    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        bool has_value = i + 1 < argc;
        if (!positional && strcmp(arg, "--") == 0)
            positional = true;
        else if (!positional && strcmp(arg, "--db") == 0 && has_value)
            options->database = argv[++i];
        else if (!positional && index && strcmp(arg, "--config") == 0 && has_value)
            options->config = argv[++i];
        else if (!positional && !index && strcmp(arg, "--null") == 0)
            options->null_output = true;
        else if (!positional && !index && strcmp(arg, "--json") == 0)
            options->json_output = true;
        else if (!positional && !index && strcmp(arg, "--socket") == 0 && has_value)
            options->socket_path = argv[++i];
        else if (!positional && !index && strcmp(arg, "--limit") == 0 && has_value) {
            if (!parse_limit(argv[++i], &options->limit))
                return TL_INVALID;
        } else if (positional || arg[0] != '-')
            options->paths[options->path_count++] = arg;
        else
            return TL_INVALID;
    }
    if (options->json_output && options->null_output)
        return TL_INVALID;
    if (options->database != NULL && (options->socket_path != NULL || options->json_output ||
                                      (!index && strcmp(options->command, "query") != 0)))
        return TL_INVALID;
    if (index)
        return TL_OK;
    if (strcmp(options->command, "record") == 0)
        return options->path_count >= 2 && options->path_count <= 3 ? TL_OK : TL_INVALID;
    size_t expected =
        strcmp(options->command, "query") == 0 || strcmp(options->command, "resolve") == 0 ? 1U
                                                                                           : 0U;
    return options->path_count == expected ? TL_OK : TL_INVALID;
}
static tl_status socket_command(const struct options *options) {
    tl_ipc_request request = {.limit = options->limit};
    memcpy(request.request_id, "cli", 4);
    if (strcmp(options->command, "query") == 0)
        request.operation = IPC_QUERY;
    else if (strcmp(options->command, "status") == 0)
        request.operation = IPC_STATUS;
    else if (strcmp(options->command, "resolve") == 0)
        request.operation = IPC_RESOLVE;
    else if (strcmp(options->command, "record") == 0)
        request.operation = IPC_OPEN;
    else if (strcmp(options->command, "reconcile") == 0)
        request.operation = IPC_RECONCILE;
    else
        request.operation = IPC_HISTORY_CLEAR;
    if (request.operation == IPC_QUERY) {
        if (options->path_count != 1 || options->paths[0] == NULL)
            return TL_INVALID;
        size_t length = strlen(options->paths[0]);
        if (length > LEXICAL_QUERY_BYTES || !json_utf8(options->paths[0]))
            return TL_INVALID;
        memcpy(request.query, options->paths[0], length + 1);
    }
    if (request.operation == IPC_RESOLVE || request.operation == IPC_OPEN) {
        if (options->path_count == 0 || options->paths[0] == NULL)
            return TL_INVALID;
        const char *text = options->paths[0];
        uint64_t value = 0;
        for (size_t i = 0; text[i] != 0; i++) {
            if (text[i] < '0' || text[i] > '9' ||
                value > ((uint64_t)INT64_MAX - (uint64_t)(text[i] - '0')) / 10)
                return TL_INVALID;
            value = value * 10 + (uint64_t)(text[i] - '0');
        }
        if (value == 0)
            return TL_INVALID;
        request.file_id = value;
    }
    if (request.operation == IPC_OPEN) {
        if (options->path_count < 2 || options->paths[1] == NULL)
            return TL_INVALID;
        size_t length = strlen(options->paths[1]);
        if (length == 0 || length > IPC_HISTORY_ID_BYTES)
            return TL_INVALID;
        memcpy(request.event_id, options->paths[1], length + 1);
        if (options->path_count == 3) {
            length = strlen(options->paths[2]);
            if (length > IPC_HISTORY_ID_BYTES)
                return TL_INVALID;
            memcpy(request.search_id, options->paths[2], length + 1);
        }
    }
    char *default_socket = NULL;
    const char *socket_path = options->socket_path;
    tl_status status = TL_OK;
    if (socket_path == NULL) {
        status = ipc_default_path(&default_socket);
        socket_path = default_socket;
    }
    if (status == TL_OK)
        status = client_request(socket_path, &request, options->json_output, options->null_output,
                                stdout);
    ipc_path_destroy(default_socket);
    return status;
}
/* ---- paths ---------------------------------------------------------------- */
static void free_paths(struct path_list *list) {
    for (size_t i = 0; i < list->count; i++)
        free(list->paths[i]);
    free(list->paths);
    free(list->keep);
    free(list->scanned);
    *list = (struct path_list){0};
}
/* Retain an unresolved absolute spelling for diagnostics and a later retry.
 * It cannot establish that a differently spelled registered root was removed:
 * resolving trailing slashes, dot components or symlinks may have changed it. */
static tl_status add_path(struct path_list *list, const char *path) {
    char *canonical = realpath(path, NULL);
    if (canonical == NULL && errno == ENOMEM)
        return TL_NOMEM;
    if (canonical == NULL && path[0] == '/') {
        list->has_unresolved_paths = true;
        canonical = strdup(path);
    }
    if (canonical == NULL)
        return path[0] == '/' ? TL_NOMEM : TL_INVALID;
    list->paths[list->count++] = canonical;
    return TL_OK;
}
static tl_status allocate_paths(struct path_list *list, size_t capacity) {
    size_t slots = capacity == 0 ? 1 : capacity;
    list->paths = calloc(slots, sizeof(char *));
    list->keep = calloc(slots, sizeof(bool));
    list->scanned = calloc(slots, sizeof(bool));
    return list->paths == NULL || list->keep == NULL || list->scanned == NULL ? TL_NOMEM : TL_OK;
}
/* Collect config values of key; reports unknown keys as TL_INVALID. */
static tl_status config_paths(const tl_config *config, const char *key, struct path_list *list) {
    size_t entries = config_entry_count(config);
    tl_status status = allocate_paths(list, entries);
    for (size_t i = 0; i < entries && status == TL_OK; i++) {
        const char *entry_key = NULL, *value = NULL;
        status = config_entry(config, i, &entry_key, &value);
        if (status == TL_OK && strcmp(entry_key, "root") != 0 && strcmp(entry_key, "allow") != 0) {
            fprintf(stderr, "torchlight: unknown configuration key \"%s\"\n", entry_key);
            status = TL_INVALID;
        }
        if (status == TL_OK && strcmp(entry_key, key) == 0)
            status = add_path(list, value);
    }
    return status;
}
/* Roots from the command line, else the configuration, else HOME. */
static tl_status select_roots(const struct options *options, const tl_config *config,
                              struct path_list *roots) {
    if (options->path_count != 0) {
        tl_status status = allocate_paths(roots, options->path_count);
        for (size_t i = 0; i < options->path_count && status == TL_OK; i++)
            status = add_path(roots, options->paths[i]);
        return status;
    }
    tl_status status = config_paths(config, "root", roots);
    const char *home = getenv("HOME");
    if (status == TL_OK && roots->count == 0)
        status = home == NULL || home[0] != '/' ? TL_INVALID : add_path(roots, home);
    return status;
}
/* ---- index ---------------------------------------------------------------- */
static tl_status save_entry(void *context, const tl_crawl_entry *entry) {
    struct scan_context *scan = context;
    size_t length = strlen(scan->database);
    /* Custom databases may live inside roots; also skip WAL/SHM sidecars. */
    if (strncmp(entry->path, scan->database, length) == 0 &&
        (entry->path[length] == 0 || strcmp(entry->path + length, "-wal") == 0 ||
         strcmp(entry->path + length, "-shm") == 0 ||
         strcmp(entry->path + length, ".daemon.lock") == 0))
        return TL_OK;
    tl_status status = store_put(scan->store, entry);
    scan->callback_status = status;
    if (status == TL_OK && entry->unreadable)
        scan->unreadable++;
    else if (status == TL_OK)
        scan->count++;
    return status;
}
static void warn_path(const char *message, const char *path) {
    char *display = NULL;
    if (tokenize_display_create(path, &display) == TL_OK)
        fprintf(stderr, "torchlight: %s: %s\n", message, display);
    tokenize_display_destroy(display);
}
/* Scan every kept root; an unavailable root keeps its saved entries. */
static tl_status scan_roots(tl_store *store, tl_crawl *crawler, struct path_list *roots,
                            struct scan_context *scan) {
    for (size_t i = 0; i < roots->count; i++) {
        if (!roots->keep[i])
            continue;
        tl_status status = crawl_run(crawler, roots->paths[i], save_entry, scan);
        if (scan->callback_status != TL_OK)
            return scan->callback_status;
        if (status == TL_IO) {
            warn_path("root unavailable, keeping saved entries", roots->paths[i]);
            status = store_keep(store, roots->paths[i]);
        } else if (status == TL_OK) {
            roots->scanned[i] = true;
        }
        if (status != TL_OK)
            return status;
    }
    return TL_OK;
}
static tl_status remember_root(void *context, const char *root) {
    struct path_list *registered = context;
    char **paths = realloc(registered->paths, (registered->count + 1) * sizeof(char *));
    if (paths == NULL)
        return TL_NOMEM;
    registered->paths = paths;
    registered->paths[registered->count] = strdup(root);
    return registered->paths[registered->count++] == NULL ? TL_NOMEM : TL_OK;
}
/* Unresolved spellings may refer to any unmatched registered root. Keep those
 * scopes through ancestor pruning, then retry their removal on a resolved sync. */
static tl_status sync_registered_roots(tl_store *store, const struct path_list *roots,
                                       size_t *forgotten, size_t *deferred) {
    struct path_list registered = {0};
    tl_status status = store_roots(store, remember_root, &registered);
    for (size_t i = 0; i < registered.count && status == TL_OK; i++) {
        bool configured = false;
        for (size_t j = 0; j < roots->count && !configured; j++)
            configured = strcmp(registered.paths[i], roots->paths[j]) == 0;
        if (!configured) {
            if (roots->has_unresolved_paths) {
                status = store_keep(store, registered.paths[i]);
                *deferred += status == TL_OK;
            } else {
                status = store_forget_root(store, registered.paths[i]);
                *forgotten += status == TL_OK;
            }
        }
    }
    free_paths(&registered);
    return status;
}
static tl_status refresh(tl_store *store, tl_crawl *crawler, struct path_list *roots,
                         bool sync_config, struct scan_context *scan) {
    size_t forgotten = 0, deferred = 0, scanned = 0;
    tl_status status = store_begin(store);
    if (status == TL_OK)
        status = scan_roots(store, crawler, roots, scan);
    for (size_t i = 0; i < roots->count; i++)
        scanned += roots->scanned[i];
    /* With no root available nothing was learned: fail and change nothing. */
    if (status == TL_OK && scanned == 0)
        status = TL_IO;
    if (status == TL_OK && sync_config)
        status = sync_registered_roots(store, roots, &forgotten, &deferred);
    for (size_t i = 0; i < roots->count && status == TL_OK; i++) {
        if (roots->scanned[i])
            status = store_prune(store, roots->paths[i]);
    }
    if (status == TL_OK)
        status = store_commit(store);
    if (status != TL_OK) {
        tl_status rollback = store_rollback(store);
        return rollback == TL_OK || rollback == TL_STATE ? status : rollback;
    }
    fprintf(stderr, "Indexed %zu entries.\n", scan->count);
    if (scan->unreadable != 0)
        fprintf(stderr, "Kept saved entries for %zu unreadable paths.\n", scan->unreadable);
    if (forgotten != 0)
        fprintf(stderr, "Forgot %zu roots no longer configured.\n", forgotten);
    if (deferred != 0)
        fprintf(stderr, "Deferred removal of %zu roots until configured paths resolve.\n",
                deferred);
    return TL_OK;
}
static tl_status index_roots(tl_store *store, const struct options *options,
                             const tl_config *config) {
    struct path_list roots = {0}, allow = {0};
    tl_crawl *crawler = NULL;
    char *database = realpath(config_database(config), NULL);
    tl_status status = database == NULL ? TL_IO : config_paths(config, "allow", &allow);
    if (status == TL_OK)
        status = select_roots(options, config, &roots);
    if (status == TL_OK)
        status = crawl_create(config_state_directory(config), (const char *const *)allow.paths,
                              allow.count, &crawler);
    if (status == TL_OK) {
        crawl_select_roots(crawler, (const char *const *)roots.paths, roots.count, roots.keep);
        struct scan_context scan = {.store = store, .database = database};
        status = refresh(store, crawler, &roots, options->path_count == 0, &scan);
    }
    crawl_destroy(crawler);
    free_paths(&roots);
    free_paths(&allow);
    free(database);
    return status;
}
/* ---- query ---------------------------------------------------------------- */
static tl_status load_entry(void *context, const tl_store_entry *entry) {
    return lexical_add(context, entry->id, entry->path, entry->is_root);
}
static tl_status print_results(const tl_result *results, size_t count, bool null_output) {
    for (size_t i = 0; i < count; i++) {
        if (null_output) {
            size_t length = strlen(results[i].path) + 1;
            if (fwrite(results[i].path, 1, length, stdout) != length)
                return TL_IO;
        } else {
            char *display = NULL;
            tl_status status = tokenize_display_create(results[i].path, &display);
            if (status != TL_OK)
                return status;
            int code = puts(display);
            tokenize_display_destroy(display);
            if (code == EOF)
                return TL_IO;
        }
    }
    return fflush(stdout) == 0 ? TL_OK : TL_IO;
}
static tl_status query_catalog(tl_store *store, const struct options *options) {
    tl_lexical *engine = NULL;
    tl_lexical_workspace *workspace = NULL;
    tl_result results[LEXICAL_MAX_RESULTS];
    tl_status status = lexical_create(&engine);
    if (status == TL_OK)
        status = store_load(store, load_entry, engine);
    if (status == TL_OK)
        status = lexical_finish(engine);
    if (status == TL_OK)
        status = lexical_workspace_create(engine, &workspace);
    size_t count = 0;
    if (status == TL_OK)
        status =
            lexical_query(engine, workspace, options->paths[0], results, options->limit, &count);
    if (status == TL_OK)
        status = print_results(results, count, options->null_output);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
    return status;
}
static tl_status open_config(const struct options *options, tl_config **config) {
    size_t line = 0;
    tl_status status =
        config_create("torchlight", options->database, options->config, &line, config);
    if (status == TL_INVALID && line != 0)
        fprintf(stderr, "torchlight: invalid configuration line %zu\n", line);
    return status;
}
int main(int argc, char **argv) {
    struct options options = {0};
    tl_status status = parse(argc, argv, &options);
    if (status != TL_OK) {
        free(options.paths);
        usage();
        return 2;
    }
    tl_config *config = NULL;
    tl_store *store = NULL;
    bool socket_mode = strcmp(options.command, "index") != 0 && options.database == NULL;
    int database_lock = -1;
    if (socket_mode)
        status = socket_command(&options);
    else
        status = open_config(&options, &config);
    if (!socket_mode && status == TL_OK && strcmp(options.command, "index") == 0)
        status = ipc_database_lock(config_database(config), &database_lock);
    if (!socket_mode && status == TL_OK)
        status = store_create(config_database(config), &store);
    if (!socket_mode && status == TL_OK)
        status = strcmp(options.command, "index") == 0 ? index_roots(store, &options, config)
                                                       : query_catalog(store, &options);
    store_destroy(store);
    ipc_unlock(database_lock);
    config_destroy(config);
    free(options.paths);
    if (status != TL_OK) {
        fprintf(stderr, "torchlight: %s\n", tl_status_string(status));
        return 1;
    }
    return 0;
}
