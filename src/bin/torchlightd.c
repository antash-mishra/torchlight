/* Thin executable: parse daemon options, set the process allocator policy and
 * wire the reusable service module. */
#include "torchlight/daemon.h"
#include "torchlight/semantic.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#define DEFAULT_RESCAN_MS 30000
/* Backstop full scan of every root while inotify coverage is complete. */
#define DEFAULT_REPAIR_MS 3600000
#define MAX_INTERVAL_MS 86400000
static bool parse_number(const char *text, size_t maximum, size_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || text[0] < '0' || text[0] > '9' || *end != 0 || value > maximum)
        return false;
    *out = (size_t)value;
    return true;
}
static bool number(const char *text, size_t maximum, size_t *out) {
    return parse_number(text, maximum, out) && *out != 0;
}
/* Blocks at least this large come straight from mmap and go back to the OS
 * when freed. Engine arrays are larger; paths and tokens are far smaller. A
 * fixed value also stops glibc from raising the threshold after the first
 * large free (ADR 0039 has the measurements behind it). */
#define ALLOCATOR_MMAP_THRESHOLD (1024 * 1024)
/* Process-wide allocator policy: set here, in the executable, so the library
 * stays allocator-neutral. Best effort; other C libraries keep defaults. */
static void set_allocator_policy(void) {
#ifdef __GLIBC__
    int applied = mallopt(M_MMAP_THRESHOLD, ALLOCATOR_MMAP_THRESHOLD);
    (void)applied;
#endif
}
/* The writer calls this after freeing a full scan's batch or a retired whole
 * engine: glibc keeps freed heap in its arenas otherwise, so RSS would stay
 * at the rebuild peak. */
static void release_memory(void *context) {
    (void)context;
#ifdef __GLIBC__
    int released = malloc_trim(0);
    (void)released;
#endif
}
static tl_status parse(int argc, char **argv, tl_daemon_options *options, const char **database,
                       const char **config) {
    *options = (tl_daemon_options){.history = true,
                                   .watch_capacity = 65536,
                                   .max_entries = 500000,
                                   .max_path_bytes = 128U * 1024U * 1024U,
                                   .rescan_ms = DEFAULT_RESCAN_MS,
                                   .repair_ms = DEFAULT_REPAIR_MS,
                                   .history_days = 30,
                                   .release_memory = release_memory};
    options->semantic_deadline_ms = SEMANTIC_DEADLINE_MS;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-history") == 0) {
            options->history = false;
            continue;
        }
        if (i + 1 == argc)
            return TL_INVALID;
        const char *key = argv[i], *value = argv[++i];
        size_t parsed = 0;
        if (strcmp(key, "--db") == 0)
            *database = value;
        else if (strcmp(key, "--config") == 0)
            *config = value;
        else if (strcmp(key, "--socket") == 0)
            options->socket_path = value;
        else if (strcmp(key, "--model") == 0)
            options->model_path = value;
        else if (strcmp(key, "--semantic-deadline-ms") == 0 && number(value, 4000, &parsed))
            options->semantic_deadline_ms = (unsigned)parsed;
        else if (strcmp(key, "--rescan-ms") == 0 && number(value, 3600000, &parsed) &&
                 parsed >= 100)
            options->rescan_ms = (unsigned)parsed;
        else if (strcmp(key, "--repair-ms") == 0 && parse_number(value, MAX_INTERVAL_MS, &parsed) &&
                 (parsed == 0 || parsed >= 100))
            options->repair_ms = (unsigned)parsed;
        else if (strcmp(key, "--watch-capacity") == 0 && number(value, 1000000, &parsed))
            options->watch_capacity = parsed;
        else if (strcmp(key, "--max-entries") == 0 && number(value, 10000000, &parsed))
            options->max_entries = parsed;
        else if (strcmp(key, "--max-path-bytes") == 0 && number(value, SIZE_MAX, &parsed))
            options->max_path_bytes = parsed;
        else if (strcmp(key, "--history-days") == 0 && number(value, 36500, &parsed))
            options->history_days = (unsigned)parsed;
        else
            return TL_INVALID;
    }
    return TL_OK;
}
int main(int argc, char **argv) {
    set_allocator_policy();
    tl_daemon_options options;
    const char *database = NULL, *file = NULL;
    tl_status status = parse(argc, argv, &options, &database, &file);
    if (status != TL_OK) {
        fputs("Usage: torchlightd [--db PATH] [--config PATH] [--socket PATH]\n"
              "  [--no-history] [--history-days N] [--rescan-ms N] [--repair-ms N]\n"
              "  [--watch-capacity N] [--max-entries N] [--max-path-bytes N]\n"
              "  [--model PATH.tlm] [--semantic-deadline-ms N]\n",
              stderr);
        return 2;
    }
    tl_config *config = NULL;
    char *socket_path = NULL;
    tl_daemon *daemon = NULL;
    size_t error_line = 0;
    status = config_create("torchlight", database, file, &error_line, &config);
    options.config = config;
    if (status == TL_OK && options.socket_path == NULL) {
        status = ipc_default_path(&socket_path);
        options.socket_path = socket_path;
    }
    if (status == TL_OK)
        status = daemon_create(&options, &daemon);
    if (status == TL_OK)
        status = daemon_run(daemon);
    tl_status cleanup = daemon_destroy(daemon);
    if (cleanup != TL_OK)
        status = cleanup;
    ipc_path_destroy(socket_path);
    config_destroy(config);
    if (status != TL_OK)
        fprintf(stderr, "torchlightd: %s (config line %zu)\n", tl_status_string(status),
                error_line);
    return status == TL_OK ? 0 : 1;
}
