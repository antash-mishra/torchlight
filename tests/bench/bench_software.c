/* Available-software build cost and warm resident-query latency at its entry cap. */
#include "stats.h"
#include "torchlight/software.h"
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#define SOFTWARE_BENCH_ROUNDS 100
static tl_status populate(tl_software *index) {
    static const char *const kinds[] = {"Photo Viewer", "Code Editor", "Music Player",
                                        "Video Studio"};
    for (size_t i = 0; i < SOFTWARE_MAX_ENTRIES; i++) {
        tl_software_entry entry = {.ref = {.installation = "user",
                                           .remote = "flathub",
                                           .arch = "x86_64",
                                           .branch = "stable"},
                                   .source_revision = 1,
                                   .summary = "Desktop productivity application"};
        char name[SOFTWARE_NAME_BYTES];
        int written = snprintf(name, sizeof(name), "%s %05zu", kinds[i % 4], i);
        if (written < 0 || (size_t)written >= sizeof(name))
            return TL_LIMIT;
        written =
            snprintf(entry.ref.app_id, sizeof(entry.ref.app_id), "org.torchlight.App%05zu", i);
        if (written < 0 || (size_t)written >= sizeof(entry.ref.app_id))
            return TL_LIMIT;
        entry.name = name;
        tl_status status = software_add(index, &entry);
        if (status != TL_OK)
            return status;
    }
    return software_finish(index);
}
int main(void) {
    tl_software *index = NULL;
    tl_software_reader *reader = NULL;
    bench_latency latency = {0};
    double start = 0, end = 0;
    tl_status status = bench_now(&start);
    if (status == TL_OK)
        status = software_create(&index);
    if (status == TL_OK)
        status = populate(index);
    if (status == TL_OK)
        status = software_reader_create(index, &reader);
    if (status == TL_OK)
        status = bench_now(&end);
    printf("software_entries=%zu build_seconds=%.3f\n", software_count(index), end - start);
    static const char *const queries[] = {
        "p",      "photo", "photo view", "code",
        "editor", "music", "video stu",  "org.torchlight.App00023"};
    bench_latency per_query[sizeof(queries) / sizeof(queries[0])] = {0};
    const tl_software_entry *hits[10] = {0};
    size_t count = 0;
    for (size_t round = 0; round <= SOFTWARE_BENCH_ROUNDS && status == TL_OK; round++) {
        for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]) && status == TL_OK; i++) {
            status = bench_now(&start);
            if (status == TL_OK)
                status = software_query(reader, queries[i], NULL, hits, 10, &count);
            if (status == TL_OK && count == 0)
                status = TL_STATE;
            if (status == TL_OK)
                status = bench_now(&end);
            if (status == TL_OK && round != 0)
                status = bench_record(&latency, (end - start) * 1000);
            if (status == TL_OK && round != 0)
                status = bench_record(&per_query[i], (end - start) * 1000);
        }
    }
    bench_report("software_warm_query", &latency);
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        bench_report(queries[i], &per_query[i]);
        bench_latency_free(&per_query[i]);
    }
    bench_latency suggestions = {0};
    for (size_t round = 0; round <= SOFTWARE_BENCH_ROUNDS && status == TL_OK; round++) {
        for (size_t i = 1; i + 1 < sizeof(queries) / sizeof(queries[0]) && status == TL_OK; i++) {
            bool exact = false;
            status = bench_now(&start);
            if (status == TL_OK)
                status = software_suggest(reader, queries[i], NULL, hits, SOFTWARE_SUGGESTIONS,
                                          &count, &exact);
            if (status == TL_OK && count == 0)
                status = TL_STATE;
            if (status == TL_OK)
                status = bench_now(&end);
            if (status == TL_OK && round != 0)
                status = bench_record(&suggestions, (end - start) * 1000);
        }
    }
    bench_report("software_automatic_suggestions", &suggestions);
    bench_latency_free(&suggestions);
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        printf("peak_rss_kib=%ld\n", usage.ru_maxrss);
    bench_latency_free(&latency);
    software_reader_destroy(reader);
    software_destroy(index);
    if (status != TL_OK)
        fprintf(stderr, "software benchmark: %s\n", tl_status_string(status));
    return status == TL_OK ? 0 : 1;
}
