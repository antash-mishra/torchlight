/* Local M1 index/query CLI; domain algorithms live in reusable modules. */
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
    const char *command, *database, *argument;
    size_t limit;
    bool null_output;
};
static void usage(void) {
    fputs("Usage: torchlight index [--db PATH] ROOT\n"
          "       torchlight query [--db PATH] [--limit 1..1000] [--null] QUERY\n",
          stderr);
}
static tl_status parse(int argc, char **argv, struct options *options) {
    if (argc < 3)
        return TL_INVALID;
    *options = (struct options){.command = argv[1], .limit = 10};
    if (strcmp(options->command, "index") != 0 && strcmp(options->command, "query") != 0)
        return TL_INVALID;
    bool positional = false;
    for (int i = 2; i < argc; i++) {
        if (!positional && strcmp(argv[i], "--") == 0) {
            positional = true;
            continue;
        }
        if (!positional && strcmp(argv[i], "--db") == 0 && i + 1 < argc)
            options->database = argv[++i];
        else if (!positional && strcmp(argv[i], "--null") == 0)
            options->null_output = true;
        else if (!positional && strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            char *end = NULL;
            const char *value = argv[++i];
            errno = 0;
            unsigned long limit = strtoul(value, &end, 10);
            if (errno != 0 || value[0] < '0' || value[0] > '9' || *end != 0 || limit == 0 ||
                limit > LEXICAL_MAX_RESULTS)
                return TL_INVALID;
            options->limit = (size_t)limit;
        } else if (options->argument == NULL && (positional || argv[i][0] != '-'))
            options->argument = argv[i];
        else
            return TL_INVALID;
    }
    return options->argument == NULL ? TL_INVALID : TL_OK;
}
struct scan_context {
    tl_store *store;
    const char *database;
    size_t count, unreadable;
};
static tl_status save_entry(void *context, const tl_crawl_entry *entry) {
    struct scan_context *scan = context;
    size_t length = strlen(scan->database);
    /* Custom databases may live inside roots; also skip WAL/SHM sidecars. */
    if (strncmp(entry->path, scan->database, length) == 0 &&
        (entry->path[length] == 0 || strcmp(entry->path + length, "-wal") == 0 ||
         strcmp(entry->path + length, "-shm") == 0))
        return TL_OK;
    tl_status status = store_put(scan->store, entry);
    if (status == TL_OK && entry->unreadable)
        scan->unreadable++;
    else if (status == TL_OK)
        scan->count++;
    return status;
}
static tl_status index_root(tl_store *store, const struct options *options,
                            const char *state_directory) {
    char *root = realpath(options->argument, NULL), *database = realpath(options->database, NULL);
    tl_crawl *crawler = NULL;
    tl_status status =
        root == NULL || database == NULL ? TL_IO : crawl_create(state_directory, &crawler);
    if (status != TL_OK)
        goto cleanup;
    struct scan_context scan = {store, database, 0, 0};
    status = store_begin(store);
    if (status != TL_OK)
        goto cleanup;
    status = crawl_run(crawler, root, save_entry, &scan);
    if (status == TL_OK)
        status = store_prune(store, root);
    if (status == TL_OK)
        status = store_commit(store);
    if (status != TL_OK) {
        tl_status rollback = store_rollback(store);
        if (rollback != TL_OK)
            status = rollback;
    } else if (fprintf(stderr, "Indexed %zu entries.\n", scan.count) < 0 ||
               (scan.unreadable != 0 &&
                fprintf(stderr, "Kept saved entries for %zu unreadable paths.\n", scan.unreadable) <
                    0))
        status = TL_IO;
cleanup:
    crawl_destroy(crawler);
    free(database);
    free(root);
    return status;
}
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
            lexical_query(engine, workspace, options->argument, results, options->limit, &count);
    if (status == TL_OK)
        status = print_results(results, count, options->null_output);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
    return status;
}
int main(int argc, char **argv) {
    struct options options = {0};
    tl_status status = parse(argc, argv, &options);
    if (status != TL_OK) {
        usage();
        return 2;
    }
    tl_config *config = NULL;
    tl_store *store = NULL;
    status = config_create("torchlight", options.database, &config);
    if (status != TL_OK)
        goto cleanup;
    options.database = config_database(config);
    status = store_create(options.database, &store);
    if (status != TL_OK)
        goto cleanup;
    status = strcmp(options.command, "index") == 0
                 ? index_root(store, &options, config_state_directory(config))
                 : query_catalog(store, &options);
cleanup:
    store_destroy(store);
    config_destroy(config);
    if (status != TL_OK) {
        fprintf(stderr, "torchlight: %s\n", tl_status_string(status));
        return 1;
    }
    return 0;
}
