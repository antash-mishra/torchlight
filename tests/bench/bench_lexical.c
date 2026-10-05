/* Warm lexical-engine benchmark and ranking evaluation, independent of the
 * CLI and SQLite. Reports build cost, memory, per-keystroke typing latency,
 * whole-query latency and labeled ranking quality (tuning and held-out query
 * seeds) for a synthetic or real path corpus.
 *
 *   bench_lexical --synthetic COUNT
 *   bench_lexical --paths FILE [--limit COUNT]   (NUL-separated paths) */
#include "corpus.h"
#include "queries.h"
#include "torchlight/catalog.h"
#include "torchlight/lexical.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
enum {
    BENCH_LIMIT = 10,        /* results a launcher shows */
    BENCH_CANDIDATES = 1000, /* candidate-recall depth */
    BENCH_QUERIES_PER_KIND = 200,
    BENCH_SYNTHETIC_SEED = 42,
    BENCH_TUNING_SEED = 1,  /* used while developing ranking */
    BENCH_HELD_OUT_SEED = 2 /* reported, never tuned against */
};
struct fixture {
    const char *query, *basename; /* NULL basename: expect no results */
};
/* Sanity fixtures present in every synthetic corpus (see corpus.c). */
static const struct fixture FIXTURES[] = {{"README.md", "README.md"},
                                          {"read", "README.md"},
                                          {"prjnts", "projectNotes.md"},
                                          {"projectnotes.md", "projectNotes.md"},
                                          {"documents projectnotes", "projectNotes.md"},
                                          {"finance invoice2024", "invoice2024.pdf"},
                                          {"CAFE\xcc\x81", "Caf\xc3\xa9.pdf"},
                                          {"invoce2024", "invoice2024.pdf"},
                                          {"zznomatchqq", NULL}};
struct latency {
    double *samples;
    size_t count, capacity;
};
struct quality {
    size_t queries, at1, at10, at_candidates;
    double reciprocal;
};
static tl_status now(double *out) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return TL_IO;
    *out = (double)value.tv_sec + (double)value.tv_nsec / 1e9;
    return TL_OK;
}
static long resident_kib(void) {
    FILE *stream = fopen("/proc/self/statm", "r");
    long size = 0, resident = 0;
    if (stream == NULL)
        return -1;
    int fields = fscanf(stream, "%ld %ld", &size, &resident);
    fclose(stream);
    return fields == 2 ? resident * (sysconf(_SC_PAGESIZE) / 1024) : -1;
}
static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}
static tl_status record(struct latency *latency, double milliseconds) {
    if (latency->count == latency->capacity) {
        size_t capacity = latency->capacity == 0 ? 1024 : latency->capacity * 2;
        double *samples = realloc(latency->samples, capacity * sizeof(double));
        if (samples == NULL)
            return TL_NOMEM;
        latency->samples = samples;
        latency->capacity = capacity;
    }
    latency->samples[latency->count++] = milliseconds;
    return TL_OK;
}
static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return a == b ? 0 : a < b ? -1 : 1;
}
static double percentile(const struct latency *latency, size_t percent) {
    size_t index = (latency->count * percent + 99) / 100;
    return latency->samples[index == 0 ? 0 : index - 1];
}
static void report_latency(const char *label, struct latency *latency) {
    if (latency->count == 0)
        return;
    qsort(latency->samples, latency->count, sizeof(double), compare_double);
    printf("%s_ms n=%zu p50=%.3f p95=%.3f p99=%.3f max=%.3f\n", label, latency->count,
           percentile(latency, 50), percentile(latency, 95), percentile(latency, 99),
           latency->samples[latency->count - 1]);
}
static tl_status timed_query(const tl_lexical *engine, tl_lexical_workspace *workspace,
                             const char *query, tl_result *results, size_t capacity, size_t *count,
                             double *milliseconds) {
    double start = 0, end = 0;
    tl_status status = now(&start);
    if (status == TL_OK)
        status = lexical_query(engine, workspace, query, results, capacity, count);
    if (status == TL_OK)
        status = now(&end);
    *milliseconds = (end - start) * 1e3;
    return status;
}
static tl_status build_engine(const bench_corpus *corpus, tl_lexical **engine,
                              tl_lexical_workspace **workspace) {
    double start = 0, end = 0;
    long before = resident_kib();
    tl_status status = now(&start);
    if (status == TL_OK)
        status = lexical_create(engine);
    for (size_t i = 0; i < corpus->count && status == TL_OK; i++)
        status = lexical_add(*engine, (uint64_t)i + 1, corpus->paths[i], corpus->is_root[i]);
    if (status == TL_OK)
        status = lexical_finish(*engine);
    if (status == TL_OK)
        status = lexical_workspace_create(*engine, workspace);
    if (status == TL_OK)
        status = now(&end);
    if (status == TL_OK)
        printf("paths=%zu mean_path_bytes=%.1f build_s=%.3f engine_rss_kib=%ld\n", corpus->count,
               (double)corpus->bytes / (double)corpus->count, end - start, resident_kib() - before);
    return status;
}
static bool contains(const tl_result *results, size_t count, const char *basename) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(base_name(results[i].path), basename) == 0)
            return true;
    }
    return false;
}
static tl_status check_fixtures(const tl_lexical *engine, tl_lexical_workspace *workspace) {
    size_t passed = 0, total = sizeof(FIXTURES) / sizeof(FIXTURES[0]);
    for (size_t i = 0; i < total; i++) {
        tl_result results[BENCH_LIMIT];
        size_t count = 0;
        tl_status status =
            lexical_query(engine, workspace, FIXTURES[i].query, results, BENCH_LIMIT, &count);
        if (status != TL_OK)
            return status;
        bool ok = FIXTURES[i].basename == NULL ? count == 0
                                               : contains(results, count, FIXTURES[i].basename);
        if (ok)
            passed++;
        else
            printf("fixture_failed query=\"%s\"\n", FIXTURES[i].query);
    }
    printf("fixtures=%zu/%zu\n", passed, total);
    return passed == total ? TL_OK : TL_STATE;
}
/* 1-based rank of the first relevant result, or 0 when absent. */
static size_t relevant_rank(const bench_corpus *corpus, const bench_query *query,
                            const tl_result *results, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (query_relevant(corpus, query, results[i].path))
            return i + 1;
    }
    return 0;
}
static void report_quality(const char *label, const struct quality *quality) {
    double n = quality->queries == 0 ? 1.0 : (double)quality->queries;
    printf("quality %s n=%zu recall@1=%.3f recall@10=%.3f mrr@10=%.3f candidate_recall@%d=%.3f\n",
           label, quality->queries, (double)quality->at1 / n, (double)quality->at10 / n,
           quality->reciprocal / n, BENCH_CANDIDATES, (double)quality->at_candidates / n);
}
static tl_status evaluate(const bench_corpus *corpus, const tl_lexical *engine,
                          tl_lexical_workspace *workspace, const char *seed_label,
                          const bench_query *queries, size_t count, tl_result *results) {
    struct quality kinds[QUERY_KIND_COUNT] = {{0}}, all = {0};
    for (size_t q = 0; q < count; q++) {
        size_t found = 0;
        tl_status status =
            lexical_query(engine, workspace, queries[q].text, results, BENCH_CANDIDATES, &found);
        if (status != TL_OK)
            return status;
        size_t rank = relevant_rank(corpus, &queries[q], results, found);
        struct quality *targets[] = {&kinds[queries[q].kind], &all};
        for (size_t t = 0; t < 2; t++) {
            targets[t]->queries++;
            targets[t]->at1 += rank == 1;
            targets[t]->at10 += rank != 0 && rank <= BENCH_LIMIT;
            targets[t]->at_candidates += rank != 0;
            targets[t]->reciprocal += rank != 0 && rank <= BENCH_LIMIT ? 1.0 / (double)rank : 0.0;
        }
    }
    char label[64];
    for (int kind = 0; kind < QUERY_KIND_COUNT; kind++) {
        snprintf(label, sizeof(label), "%s/%s", seed_label, query_kind_name((enum query_kind)kind));
        report_quality(label, &kinds[kind]);
    }
    snprintf(label, sizeof(label), "%s/all", seed_label);
    report_quality(label, &all);
    return TL_OK;
}
/* Type every query one byte at a time (as a launcher sees it), timing each
 * keystroke; then time each whole query issued after a different one. */
static tl_status measure_latency(const tl_lexical *engine, tl_lexical_workspace *workspace,
                                 const bench_query *queries, size_t count) {
    struct latency typing = {0}, whole = {0};
    tl_result results[BENCH_LIMIT];
    tl_status status = TL_OK;
    for (size_t q = 0; q < count && status == TL_OK; q++) {
        char prefix[LEXICAL_QUERY_BYTES + 1];
        size_t length = strlen(queries[q].text);
        for (size_t typed = 1; typed <= length && status == TL_OK; typed++) {
            memcpy(prefix, queries[q].text, typed);
            prefix[typed] = 0;
            size_t found = 0;
            double milliseconds = 0;
            status =
                timed_query(engine, workspace, prefix, results, BENCH_LIMIT, &found, &milliseconds);
            if (status == TL_OK)
                status = record(&typing, milliseconds);
        }
    }
    for (size_t q = 0; q < count && status == TL_OK; q++) {
        size_t found = 0;
        double milliseconds = 0;
        status = timed_query(engine, workspace, queries[q].text, results, BENCH_LIMIT, &found,
                             &milliseconds);
        if (status == TL_OK)
            status = record(&whole, milliseconds);
    }
    if (status == TL_OK) {
        report_latency("typing", &typing);
        report_latency("whole_query", &whole);
    }
    free(typing.samples);
    free(whole.samples);
    return status;
}
/* Fresh query membership/evidence caches over an already resident engine.
 * Workspace allocation and worker startup happen outside the measured query. */
static tl_status measure_cold(const tl_lexical *engine, const bench_query *queries, size_t count) {
    enum { COLD_QUERY_STRIDE = 20 };
    struct latency latency = {0};
    tl_status status = TL_OK;
    for (size_t q = 0; q < count && status == TL_OK; q += COLD_QUERY_STRIDE) {
        tl_lexical_workspace *workspace = NULL;
        status = lexical_workspace_create(engine, &workspace);
        tl_result results[BENCH_LIMIT];
        size_t found = 0;
        double milliseconds = 0;
        if (status == TL_OK)
            status = timed_query(engine, workspace, queries[q].text, results, BENCH_LIMIT, &found,
                                 &milliseconds);
        lexical_workspace_destroy(workspace);
        if (status == TL_OK)
            status = record(&latency, milliseconds);
    }
    if (status == TL_OK)
        report_latency("cold_workspace_query", &latency);
    free(latency.samples);
    return status;
}
/* Include lease acquisition/release around the same held-out whole queries.
 * Construction and publication remain outside the timed query path. */
static tl_status measure_catalog(tl_lexical **engine, const bench_query *queries, size_t count) {
    tl_catalog *catalog = NULL;
    tl_catalog_snapshot *snapshot = NULL;
    struct latency latency = {0};
    tl_status status = catalog_create(2, &catalog);
    if (status == TL_OK)
        status = catalog_snapshot_create(engine, 1, 1, &snapshot);
    if (status == TL_OK)
        status = catalog_publish(catalog, &snapshot);
    for (size_t q = 0; q < count && status == TL_OK; q++) {
        double start = 0, end = 0;
        size_t found = 0;
        tl_result results[BENCH_LIMIT];
        tl_catalog_reader *reader = NULL;
        status = now(&start);
        if (status == TL_OK)
            status = catalog_acquire(catalog, &reader);
        if (status == TL_OK)
            status = catalog_query(reader, queries[q].text, results, BENCH_LIMIT, &found);
        catalog_release(reader);
        if (status == TL_OK)
            status = now(&end);
        if (status == TL_OK)
            status = record(&latency, (end - start) * 1e3);
    }
    if (status == TL_OK)
        report_latency("catalog_query", &latency);
    free(latency.samples);
    catalog_snapshot_destroy(snapshot);
    tl_status destroyed = catalog_destroy(catalog);
    return destroyed == TL_OK ? status : destroyed;
}
static tl_status run(const bench_corpus *corpus, bool synthetic) {
    tl_lexical *engine = NULL;
    tl_lexical_workspace *workspace = NULL;
    bench_query *tuning = NULL, *held_out = NULL;
    size_t tuning_count = 0, held_out_count = 0;
    tl_result *results = malloc(BENCH_CANDIDATES * sizeof(tl_result));
    tl_status status = results == NULL ? TL_NOMEM : build_engine(corpus, &engine, &workspace);
    if (status == TL_OK)
        status = queries_generate(corpus, BENCH_TUNING_SEED, BENCH_QUERIES_PER_KIND, &tuning,
                                  &tuning_count);
    if (status == TL_OK)
        status = queries_generate(corpus, BENCH_HELD_OUT_SEED, BENCH_QUERIES_PER_KIND, &held_out,
                                  &held_out_count);
    if (status == TL_OK)
        status = measure_latency(engine, workspace, held_out, held_out_count);
    if (status == TL_OK)
        status = measure_cold(engine, held_out, held_out_count);
    if (status == TL_OK)
        status = evaluate(corpus, engine, workspace, "tuning", tuning, tuning_count, results);
    if (status == TL_OK)
        status = evaluate(corpus, engine, workspace, "held_out", held_out, held_out_count, results);
    if (status == TL_OK && synthetic)
        status = check_fixtures(engine, workspace);
    if (status == TL_OK)
        printf("steady_rss_kib=%ld\n", resident_kib());
    lexical_workspace_destroy(workspace);
    workspace = NULL;
    if (status == TL_OK)
        status = measure_catalog(&engine, held_out, held_out_count);
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        printf("peak_rss_kib=%ld\n", usage.ru_maxrss);
    free(results);
    free(tuning);
    free(held_out);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
    return status;
}
static bool parse_count(const char *text, size_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || text[0] < '0' || text[0] > '9' || *end != 0 || value == 0)
        return false;
    *out = (size_t)value;
    return true;
}
int main(int argc, char **argv) {
    bench_corpus corpus = {0};
    size_t count = 0, limit = 0;
    tl_status status = TL_INVALID;
    bool synthetic = argc == 3 && strcmp(argv[1], "--synthetic") == 0;
    if (synthetic && parse_count(argv[2], &count))
        status = corpus_synthetic(count, BENCH_SYNTHETIC_SEED, &corpus);
    else if ((argc == 3 ||
              (argc == 5 && strcmp(argv[3], "--limit") == 0 && parse_count(argv[4], &limit))) &&
             strcmp(argv[1], "--paths") == 0)
        status = corpus_load(argv[2], limit, &corpus);
    if (status == TL_INVALID) {
        fputs("usage: bench_lexical --synthetic COUNT | --paths FILE [--limit COUNT]\n", stderr);
        return 2;
    }
    if (status == TL_OK)
        status = run(&corpus, synthetic);
    corpus_free(&corpus);
    if (status != TL_OK)
        fprintf(stderr, "benchmark: %s\n", tl_status_string(status));
    return status == TL_OK ? 0 : 1;
}
