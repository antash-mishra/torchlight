/* Reusable synthetic warm-engine benchmark, independent of CLI/SQLite loading. */
#include "torchlight/lexical.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
enum { BENCH_ROUNDS = 30, BENCH_CASES = 8, BENCH_LIMIT = 10 };
struct query_case {
    const char *query, *target;
};
static tl_status now(double *out) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return TL_IO;
    *out = (double)value.tv_sec + (double)value.tv_nsec / 1e9;
    return TL_OK;
}
static tl_status build_corpus(tl_lexical *engine, size_t count, size_t *path_bytes) {
    const char *special[] = {"/corpus", "/corpus/0/projectNotes.md", "/corpus/0/README.md",
                             "/corpus/0/Caf\xc3\xa9.pdf", "/corpus/finance/invoice2024.pdf"};
    *path_bytes = 0;
    for (size_t i = 0; i < count; i++) {
        char path[128];
        if (i < sizeof(special) / sizeof(special[0])) {
            int code = snprintf(path, sizeof(path), "%s", special[i]);
            if (code < 0 || (size_t)code >= sizeof(path))
                return TL_LIMIT;
        } else {
            int code = snprintf(path, sizeof(path), "/corpus/data/group-%05zu/document-%06zu.txt",
                                i % 1000, i);
            if (code < 0 || (size_t)code >= sizeof(path))
                return TL_LIMIT;
        }
        tl_status status = lexical_add(engine, (uint64_t)i + 1, path, i == 0);
        if (status != TL_OK)
            return status;
        *path_bytes += strlen(path);
    }
    return lexical_finish(engine);
}
static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return a == b ? 0 : a < b ? -1 : 1;
}
static bool contains(const tl_result *results, size_t count, const char *target) {
    if (target == NULL)
        return count == 0;
    for (size_t i = 0; i < count; i++)
        if (strcmp(results[i].path, target) == 0)
            return true;
    return false;
}
static tl_status measure(const tl_lexical *engine, tl_lexical_workspace *workspace) {
    const struct query_case cases[BENCH_CASES] = {
        {"README.md", "/corpus/0/README.md"},
        {"read", "/corpus/0/README.md"},
        {"prjnts", "/corpus/0/projectNotes.md"},
        {"pn", "/corpus/0/projectNotes.md"},
        {"r", "/corpus/0/README.md"},
        {"finance invoice", "/corpus/finance/invoice2024.pdf"},
        {"CAFE\xcc\x81", "/corpus/0/Caf\xc3\xa9.pdf"},
        {"zznomatch", NULL}};
    double samples[BENCH_CASES * BENCH_ROUNDS];
    size_t correct = 0;
    for (size_t round = 0; round < BENCH_ROUNDS; round++) {
        for (size_t i = 0; i < BENCH_CASES; i++) {
            tl_result results[BENCH_LIMIT];
            size_t count = 0;
            double start = 0, end = 0;
            tl_status status = now(&start);
            if (status == TL_OK)
                status =
                    lexical_query(engine, workspace, cases[i].query, results, BENCH_LIMIT, &count);
            if (status == TL_OK)
                status = now(&end);
            if (status != TL_OK)
                return status;
            samples[round * BENCH_CASES + i] = (end - start) * 1e3;
            if (contains(results, count, cases[i].target))
                correct++;
        }
    }
    size_t count = BENCH_CASES * BENCH_ROUNDS;
    qsort(samples, count, sizeof(double), compare_double);
    printf("warm_ms p50=%.3f p95=%.3f p99=%.3f labeled_checks=%zu/%zu\n", samples[count / 2],
           samples[(count * 95 + 99) / 100 - 1], samples[(count * 99 + 99) / 100 - 1], correct,
           count);
    return correct == count ? TL_OK : TL_STATE;
}
static tl_status benchmark(size_t count) {
    tl_lexical *engine = NULL;
    tl_lexical_workspace *workspace = NULL;
    double start = 0, built = 0, first_start = 0, first_end = 0;
    size_t path_bytes = 0;
    tl_status status = now(&start);
    if (status == TL_OK)
        status = lexical_create(&engine);
    if (status == TL_OK)
        status = build_corpus(engine, count, &path_bytes);
    if (status == TL_OK)
        status = lexical_workspace_create(engine, &workspace);
    if (status == TL_OK)
        status = now(&built);
    tl_result results[BENCH_LIMIT];
    size_t result_count = 0;
    if (status == TL_OK)
        status = now(&first_start);
    if (status == TL_OK)
        status = lexical_query(engine, workspace, "prjnts", results, BENCH_LIMIT, &result_count);
    if (status == TL_OK)
        status = now(&first_end);
    if (status == TL_OK) {
        printf("paths=%zu mean_path_bytes=%.1f build_s=%.3f first_ms=%.3f\n", count,
               (double)path_bytes / (double)count, built - start, (first_end - first_start) * 1e3);
        status = measure(engine, workspace);
    }
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        printf("peak_rss_kib=%ld\n", usage.ru_maxrss);
    else if (status == TL_OK)
        status = TL_IO;
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
    return status;
}
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    char *end = NULL;
    errno = 0;
    unsigned long count = strtoul(argv[1], &end, 10);
    if (errno != 0 || argv[1][0] < '0' || argv[1][0] > '9' || *end != 0 || count < 5 ||
        count > 500000)
        return 2;
    tl_status status = benchmark((size_t)count);
    if (status != TL_OK)
        fprintf(stderr, "benchmark: %s\n", tl_status_string(status));
    return status == TL_OK ? 0 : 1;
}
