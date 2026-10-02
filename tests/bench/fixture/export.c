/* Export the existing deterministic corpus and held-out queries for IPC timing. */
#include "../corpus.h"
#include "../queries.h"
#include "torchlight/json.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static tl_status export_paths(const bench_corpus *corpus, const char *path) {
    FILE *stream = fopen(path, "wb");
    if (stream == NULL)
        return TL_IO;
    tl_status status = TL_OK;
    for (size_t i = 0; i < corpus->count && status == TL_OK; i++) {
        size_t length = strlen(corpus->paths[i]) + 1;
        if (fwrite(corpus->paths[i], 1, length, stream) != length)
            status = TL_IO;
    }
    if (fclose(stream) != 0)
        status = TL_IO;
    return status;
}
static tl_status export_queries(const bench_corpus *corpus, const char *path) {
    bench_query *queries = NULL;
    size_t count = 0;
    tl_status status = queries_generate(corpus, 2, 200, &queries, &count);
    FILE *stream = status == TL_OK ? fopen(path, "w") : NULL;
    if (stream == NULL && status == TL_OK)
        status = TL_IO;
    for (size_t i = 0; i < count && status == TL_OK; i++) {
        char line[4096];
        tl_json_buffer buffer;
        json_buffer_init(&buffer, line, sizeof(line));
        json_raw(&buffer, "{\"query\":");
        json_quote(&buffer, queries[i].text);
        json_raw(&buffer, ",\"kind\":");
        json_quote(&buffer, query_kind_name(queries[i].kind));
        json_raw(&buffer, "}\n");
        status = buffer.status;
        if (status == TL_OK && fputs(line, stream) < 0)
            status = TL_IO;
    }
    if (stream != NULL && fclose(stream) != 0)
        status = TL_IO;
    free(queries);
    return status;
}
int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    char *end = NULL;
    errno = 0;
    unsigned long long count = strtoull(argv[1], &end, 10);
    if (errno != 0 || argv[1][0] < '0' || argv[1][0] > '9' || *end != 0 || count < 16 ||
        count > SIZE_MAX)
        return 2;
    bench_corpus corpus = {0};
    tl_status status = corpus_synthetic((size_t)count, 42, &corpus);
    if (status == TL_OK)
        status = export_paths(&corpus, argv[2]);
    if (status == TL_OK)
        status = export_queries(&corpus, argv[3]);
    corpus_free(&corpus);
    return status == TL_OK ? 0 : 1;
}
