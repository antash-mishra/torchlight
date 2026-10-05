/* Synthetic float scan/fusion costs, excluding inference, catalog and IPC. */
#include "torchlight/rank.h"
#include "torchlight/vector.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

enum {
    VECTOR_BENCH_DIMENSIONS = 256,
    VECTOR_BENCH_QUERIES = 64,
    VECTOR_BENCH_LIMIT = 10,
    VECTOR_BENCH_FUSION_ROWS = 1000,
    VECTOR_BENCH_COORDINATE_RANGE = 2001,
    VECTOR_BENCH_ROW_STEP = 7919
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
    if (stream == NULL)
        return -1;
    long pages = 0, resident = 0;
    int fields = fscanf(stream, "%ld %ld", &pages, &resident);
    fclose(stream);
    long page_size = sysconf(_SC_PAGESIZE);
    return fields == 2 && page_size > 0 ? resident * (page_size / 1024) : -1;
}

static void embedding(uint64_t id, float *out) {
    uint64_t state = id ^ UINT64_C(0x9e3779b97f4a7c15);
    for (size_t i = 0; i < VECTOR_BENCH_DIMENSIONS; i++) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        uint64_t random = state * UINT64_C(0x2545f4914f6cdd1d);
        out[i] = (float)((int)(random % VECTOR_BENCH_COORDINATE_RANGE) -
                         VECTOR_BENCH_COORDINATE_RANGE / 2);
    }
}

static int compare_samples(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void report(const char *label, double *samples) {
    qsort(samples, VECTOR_BENCH_QUERIES, sizeof(*samples), compare_samples);
    const size_t count = VECTOR_BENCH_QUERIES;
    printf("%s_ms n=%zu p50=%.3f p95=%.3f p99=%.3f max=%.3f\n", label, count,
           samples[(count * 50 + 99) / 100 - 1], samples[(count * 95 + 99) / 100 - 1],
           samples[(count * 99 + 99) / 100 - 1], samples[count - 1]);
}

static tl_status build(size_t rows, tl_vector **index, tl_vector_workspace **workspace) {
    double start = 0, end = 0;
    tl_status status = now(&start);
    if (status == TL_OK)
        status = vector_create(1, VECTOR_BENCH_DIMENSIONS, rows, SIZE_MAX, index);
    for (size_t i = 0; i < rows && status == TL_OK; i++) {
        float values[VECTOR_BENCH_DIMENSIONS];
        embedding(i + 1, values);
        status = vector_add(*index, i + 1, 1, values, VECTOR_BENCH_DIMENSIONS);
    }
    if (status == TL_OK)
        status = vector_finish(*index);
    if (status == TL_OK)
        status = vector_workspace_create(*index, workspace);
    if (status == TL_OK)
        status = now(&end);
    if (status == TL_OK)
        printf("rows=%zu dimensions=%d index_bytes=%zu build_s=%.3f resident_kib=%ld\n", rows,
               VECTOR_BENCH_DIMENSIONS, vector_bytes(*index), end - start, resident_kib());
    return status;
}

static tl_status timed_query(const tl_vector *index, tl_vector_workspace *workspace, uint64_t id,
                             double *milliseconds) {
    float query[VECTOR_BENCH_DIMENSIONS];
    embedding(id, query);
    tl_vector_result results[VECTOR_BENCH_LIMIT];
    size_t count = 0;
    double start = 0, end = 0;
    tl_status status = now(&start);
    if (status == TL_OK)
        status = vector_query(index, workspace, 1, query, VECTOR_BENCH_DIMENSIONS, results,
                              VECTOR_BENCH_LIMIT, &count);
    if (status == TL_OK)
        status = now(&end);
    if (status == TL_OK && (count == 0 || results[0].id != id))
        status = TL_STATE;
    *milliseconds = (end - start) * 1e3;
    return status;
}

static tl_status scan(const tl_vector *index, tl_vector_workspace *workspace) {
    double first = 0, samples[VECTOR_BENCH_QUERIES];
    tl_status status = timed_query(index, workspace, 1, &first);
    if (status == TL_OK)
        printf("first_scan_ms=%.3f\n", first);
    struct rusage before, after;
    if (status == TL_OK && getrusage(RUSAGE_SELF, &before) != 0)
        status = TL_IO;
    for (size_t i = 0; i < VECTOR_BENCH_QUERIES && status == TL_OK; i++) {
        uint64_t id = (i * VECTOR_BENCH_ROW_STEP) % vector_count(index) + 1;
        status = timed_query(index, workspace, id, &samples[i]);
    }
    if (status == TL_OK && getrusage(RUSAGE_SELF, &after) != 0)
        status = TL_IO;
    if (status == TL_OK) {
        report("float_scan", samples);
        printf("self_match_checks=%d/%d minor_faults=%ld major_faults=%ld peak_rss_kib=%ld\n",
               VECTOR_BENCH_QUERIES, VECTOR_BENCH_QUERIES, after.ru_minflt - before.ru_minflt,
               after.ru_majflt - before.ru_majflt, after.ru_maxrss);
    }
    return status;
}

static tl_status fusion(void) {
    tl_rank *ranker = NULL;
    tl_status status = rank_create(VECTOR_BENCH_FUSION_ROWS * 2, RANK_DEFAULT_RRF_K, &ranker);
    tl_rank_candidate lexical[VECTOR_BENCH_FUSION_ROWS], semantic[VECTOR_BENCH_FUSION_ROWS];
    tl_rank_result results[VECTOR_BENCH_LIMIT];
    for (size_t i = 0; i < VECTOR_BENCH_FUSION_ROWS; i++) {
        lexical[i] = (tl_rank_candidate){i + 1, "/synthetic/shared", RANK_REGULAR};
        semantic[i] =
            (tl_rank_candidate){VECTOR_BENCH_FUSION_ROWS - i, "/synthetic/shared", RANK_REGULAR};
    }
    double samples[VECTOR_BENCH_QUERIES];
    for (size_t i = 0; i < VECTOR_BENCH_QUERIES && status == TL_OK; i++) {
        double start = 0, end = 0;
        size_t count = 0;
        status = now(&start);
        if (status == TL_OK)
            status = rank_fuse(ranker, lexical, VECTOR_BENCH_FUSION_ROWS, semantic,
                               VECTOR_BENCH_FUSION_ROWS, results, VECTOR_BENCH_LIMIT, &count);
        if (status == TL_OK)
            status = now(&end);
        if (status == TL_OK && (count != VECTOR_BENCH_LIMIT || results[0].id != 1))
            status = TL_STATE;
        samples[i] = (end - start) * 1e3;
    }
    if (status == TL_OK)
        report("rrf_2x1000", samples);
    rank_destroy(ranker);
    return status;
}

int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] < '0' || argv[1][0] > '9') {
        fprintf(stderr, "usage: bench_vector ROWS (256 dimensions, synthetic floats)\n");
        return 1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(argv[1], &end, 10);
    if (errno != 0 || *end != 0 || value == 0 || value > SIZE_MAX)
        return 1;
    puts("synthetic_float_reference model=none inference=excluded ipc=excluded");
    tl_vector *index = NULL;
    tl_vector_workspace *workspace = NULL;
    tl_status status = build((size_t)value, &index, &workspace);
    if (status == TL_OK)
        status = scan(index, workspace);
    if (status == TL_OK)
        status = fusion();
    vector_workspace_destroy(workspace);
    vector_destroy(index);
    if (status != TL_OK)
        fprintf(stderr, "vector benchmark: %s\n", tl_status_string(status));
    return status == TL_OK ? 0 : 1;
}
