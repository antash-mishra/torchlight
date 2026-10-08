/* Benchmark timing helpers shared by the lexical and personal benchmarks. */
#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
tl_status bench_now(double *out) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return TL_IO;
    *out = (double)value.tv_sec + (double)value.tv_nsec / 1e9;
    return TL_OK;
}
tl_status bench_record(bench_latency *latency, double milliseconds) {
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
static double percentile(const bench_latency *latency, size_t percent) {
    size_t index = (latency->count * percent + 99) / 100;
    return latency->samples[index == 0 ? 0 : index - 1];
}
void bench_report(const char *label, bench_latency *latency) {
    if (latency->count == 0)
        return;
    qsort(latency->samples, latency->count, sizeof(double), compare_double);
    printf("%s_ms n=%zu p50=%.3f p95=%.3f p99=%.3f max=%.3f\n", label, latency->count,
           percentile(latency, 50), percentile(latency, 95), percentile(latency, 99),
           latency->samples[latency->count - 1]);
}
void bench_latency_free(bench_latency *latency) {
    if (latency == NULL)
        return;
    free(latency->samples);
    *latency = (bench_latency){0};
}
