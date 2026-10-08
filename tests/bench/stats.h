/* Shared benchmark timing: a monotonic clock and latency percentiles. */
#ifndef TORCHLIGHT_BENCH_STATS_H
#define TORCHLIGHT_BENCH_STATS_H
#include "torchlight/common.h"
typedef struct {
    double *samples;
    size_t count, capacity;
} bench_latency;
/** Seconds on the monotonic clock. TL_IO when the clock is unavailable. */
tl_status bench_now(double *out);
/** Append one sample, growing the owned buffer. TL_NOMEM on failure. */
tl_status bench_record(bench_latency *latency, double milliseconds);
/** Sort the samples and print "<label>_ms n= p50= p95= p99= max="; nothing
 * for an empty set. */
void bench_report(const char *label, bench_latency *latency);
/** Free the samples and reset; NULL allowed. */
void bench_latency_free(bench_latency *latency);
#endif
