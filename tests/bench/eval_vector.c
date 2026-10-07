/* Two-pass prefix shortlist against exhaustive int8 on trained embeddings.
 * Encodes a synthetic path corpus with a native Potion model, then compares
 * each query's top ten from vector_create_prefix_int8 with vector_create_int8:
 * mean and worst recall@10, top-1 agreement, score identity and scan latency.
 * This measures neighbor agreement, not human relevance.
 *
 *   eval_vector MODEL.tlm ROWS QUERIES [PREFIX SHORTLIST]
 *
 * QUERIES holds one query per line (the semantic fixture's texts); held-out
 * launcher queries generated from the corpus are added to them. Paths are
 * prepared from the full synthetic path; the service prepares the path below
 * the indexed root's parent, which keeps the same basename and two parents. */
#include "corpus.h"
#include "queries.h"
#include "torchlight/potion.h"
#include "torchlight/semantic.h"
#include "torchlight/vector.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    EVAL_LIMIT = 10,
    EVAL_SYNTHETIC_SEED = 42,
    EVAL_HELD_OUT_SEED = 2, /* the daemon benchmark's held-out query seed */
    EVAL_QUERIES_PER_KIND = 60,
    EVAL_MAX_QUERIES = 1024,
    EVAL_MIN_QUERY_BYTES = 3 /* semantic_submit leaves shorter queries lexical */
};

struct indexes {
    tl_vector *exact, *prefix;
    tl_vector_workspace *exact_workspace, *prefix_workspace;
};
struct agreement {
    double recall, worst_recall, exact_ms[EVAL_MAX_QUERIES], prefix_ms[EVAL_MAX_QUERIES];
    size_t queries, top1, score_mismatches;
};

static double now_ms(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec * 1e3 + (double)value.tv_nsec / 1e6;
}

static bool parse_size(const char *text, size_t *out) {
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != 0 || value == 0 || value > SIZE_MAX)
        return false;
    *out = (size_t)value;
    return true;
}

/* Encode a prepared path as a document vector; false for an unembeddable one. */
static bool encode_path(tl_embedder *embedder, const char *path, float *out, size_t dimensions) {
    char text[EMBED_TEXT_BYTES + 1];
    if (potion_prepare_path(path, text, sizeof(text)) != TL_OK || text[0] == 0)
        return false;
    return embedder_encode(embedder, EMBED_DOCUMENT, text, out, dimensions) == TL_OK;
}

static tl_status build(tl_embedder *embedder, const bench_corpus *corpus, size_t prefix,
                       size_t shortlist, struct indexes *out) {
    const tl_emb_model *model = embedder_model(embedder);
    tl_status status =
        vector_create_int8(1, model->dimensions, corpus->count, SIZE_MAX, &out->exact);
    if (status == TL_OK)
        status = vector_create_prefix_int8(1, model->dimensions, corpus->count, prefix, shortlist,
                                           SIZE_MAX, &out->prefix);
    float values[VECTOR_MAX_DIMENSIONS];
    for (size_t i = 0; i < corpus->count && status == TL_OK; i++) {
        if (!encode_path(embedder, corpus->paths[i], values, model->dimensions))
            continue;
        status = vector_add(out->exact, i + 1, 1, values, model->dimensions);
        if (status == TL_OK)
            status = vector_add(out->prefix, i + 1, 1, values, model->dimensions);
    }
    if (status == TL_OK)
        status = vector_finish(out->exact);
    if (status == TL_OK)
        status = vector_finish(out->prefix);
    if (status == TL_OK)
        status = vector_workspace_create(out->exact, &out->exact_workspace);
    if (status == TL_OK)
        status = vector_workspace_create(out->prefix, &out->prefix_workspace);
    return status;
}

static void destroy(struct indexes *indexes) {
    vector_workspace_destroy(indexes->exact_workspace);
    vector_workspace_destroy(indexes->prefix_workspace);
    vector_destroy(indexes->exact);
    vector_destroy(indexes->prefix);
}

/* Fixture lines first, then held-out generated queries, up to the limit. */
static tl_status load_queries(const char *file, const bench_corpus *corpus,
                              char (*texts)[LEXICAL_QUERY_BYTES + 1], size_t *count) {
    FILE *stream = fopen(file, "r");
    if (stream == NULL)
        return TL_IO;
    char line[LEXICAL_QUERY_BYTES + 2];
    while (*count < EVAL_MAX_QUERIES && fgets(line, sizeof(line), stream) != NULL) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) >= EVAL_MIN_QUERY_BYTES)
            memcpy(texts[(*count)++], line, strlen(line) + 1);
    }
    fclose(stream);
    bench_query *generated = NULL;
    size_t total = 0;
    tl_status status =
        queries_generate(corpus, EVAL_HELD_OUT_SEED, EVAL_QUERIES_PER_KIND, &generated, &total);
    for (size_t i = 0; i < total && *count < EVAL_MAX_QUERIES && status == TL_OK; i++) {
        const char *text = generated[i].text;
        if (strlen(text) >= EVAL_MIN_QUERY_BYTES && strchr(text, '/') == NULL)
            memcpy(texts[(*count)++], text, strlen(text) + 1);
    }
    free(generated);
    return status;
}

static tl_status timed_query(tl_vector *index, tl_vector_workspace *workspace, const float *query,
                             size_t dimensions, tl_vector_result *results, size_t *count,
                             double *milliseconds) {
    double start = now_ms();
    tl_status status =
        vector_query(index, workspace, 1, query, dimensions, results, EVAL_LIMIT, count);
    *milliseconds = now_ms() - start;
    return status;
}

/* Recall of the exhaustive top ten, top-1 agreement, and score identity for
 * every row both lists return. */
static void compare(const tl_vector_result *exact, size_t exact_count,
                    const tl_vector_result *prefix, size_t prefix_count, struct agreement *out) {
    size_t shared = 0;
    for (size_t i = 0; i < exact_count; i++)
        for (size_t j = 0; j < prefix_count; j++) {
            if (exact[i].id != prefix[j].id)
                continue;
            shared++;
            out->score_mismatches += exact[i].cosine != prefix[j].cosine;
        }
    double recall = exact_count == 0 ? 1.0 : (double)shared / (double)exact_count;
    out->recall += recall;
    out->worst_recall = recall < out->worst_recall ? recall : out->worst_recall;
    out->top1 += exact_count == 0 || (prefix_count != 0 && exact[0].id == prefix[0].id);
}

static tl_status evaluate(tl_embedder *embedder, struct indexes *indexes,
                          const char (*texts)[LEXICAL_QUERY_BYTES + 1], size_t count,
                          struct agreement *out) {
    size_t dimensions = embedder_model(embedder)->dimensions;
    out->worst_recall = 1.0;
    for (size_t i = 0; i < count; i++) {
        float query[VECTOR_MAX_DIMENSIONS];
        if (embedder_encode(embedder, EMBED_QUERY, texts[i], query, dimensions) != TL_OK)
            continue;
        tl_vector_result exact[EVAL_LIMIT], prefix[EVAL_LIMIT];
        size_t exact_count = 0, prefix_count = 0, sample = out->queries;
        tl_status status = timed_query(indexes->exact, indexes->exact_workspace, query, dimensions,
                                       exact, &exact_count, &out->exact_ms[sample]);
        if (status == TL_OK)
            status = timed_query(indexes->prefix, indexes->prefix_workspace, query, dimensions,
                                 prefix, &prefix_count, &out->prefix_ms[sample]);
        if (status != TL_OK)
            return status;
        compare(exact, exact_count, prefix, prefix_count, out);
        out->queries++;
    }
    return out->queries == 0 ? TL_STATE : TL_OK;
}

static int compare_samples(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void report_latency(const char *label, double *samples, size_t count) {
    qsort(samples, count, sizeof(*samples), compare_samples);
    printf("%s_ms n=%zu p50=%.3f p95=%.3f p99=%.3f max=%.3f\n", label, count,
           samples[(count * 50 + 99) / 100 - 1], samples[(count * 95 + 99) / 100 - 1],
           samples[(count * 99 + 99) / 100 - 1], samples[count - 1]);
}

static void report(const struct indexes *indexes, struct agreement *agreement) {
    printf("embedded_rows=%zu exact_bytes=%zu prefix_bytes=%zu queries=%zu\n",
           vector_count(indexes->exact), vector_bytes(indexes->exact),
           vector_bytes(indexes->prefix), agreement->queries);
    printf("mean_recall10=%.4f worst_recall10=%.2f top1_agreement=%.4f score_mismatches=%zu\n",
           agreement->recall / (double)agreement->queries, agreement->worst_recall,
           (double)agreement->top1 / (double)agreement->queries, agreement->score_mismatches);
    report_latency("exhaustive_int8", agreement->exact_ms, agreement->queries);
    report_latency("prefix_shortlist", agreement->prefix_ms, agreement->queries);
}

static tl_status run(tl_embedder *embedder, const bench_corpus *corpus, const char *queries,
                     size_t prefix, size_t shortlist) {
    char(*texts)[LEXICAL_QUERY_BYTES + 1] = calloc(EVAL_MAX_QUERIES, sizeof(*texts));
    struct agreement *agreement = calloc(1, sizeof(*agreement));
    struct indexes indexes = {0};
    size_t count = 0;
    tl_status status = texts == NULL || agreement == NULL
                           ? TL_NOMEM
                           : load_queries(queries, corpus, texts, &count);
    double start = now_ms();
    if (status == TL_OK)
        status = build(embedder, corpus, prefix, shortlist, &indexes);
    if (status == TL_OK)
        printf("build_s=%.1f\n", (now_ms() - start) / 1e3);
    if (status == TL_OK)
        status = evaluate(embedder, &indexes, (const char(*)[LEXICAL_QUERY_BYTES + 1]) texts, count,
                          agreement);
    if (status == TL_OK)
        report(&indexes, agreement);
    destroy(&indexes);
    free(agreement);
    free(texts);
    return status;
}

int main(int argc, char **argv) {
    size_t rows = 0, prefix = SEMANTIC_PREFIX_DIMENSIONS, shortlist = SEMANTIC_SHORTLIST;
    if ((argc != 4 && argc != 6) || !parse_size(argv[2], &rows) ||
        (argc == 6 && (!parse_size(argv[4], &prefix) || !parse_size(argv[5], &shortlist)))) {
        fprintf(stderr, "usage: eval_vector MODEL.tlm ROWS QUERIES [PREFIX SHORTLIST]\n");
        return 1;
    }
    bench_corpus corpus = {0};
    tl_embedder *embedder = NULL;
    tl_status status = corpus_synthetic(rows, EVAL_SYNTHETIC_SEED, &corpus);
    if (status == TL_OK)
        status = potion_load(argv[1], POTION_MODEL_BYTES, &embedder);
    if (status == TL_OK) {
        printf("trained_prefix_shortlist model=%s rows=%zu prefix=%zu shortlist=%zu limit=%d\n",
               embedder_model(embedder)->model_id, corpus.count, prefix, shortlist, EVAL_LIMIT);
        status = run(embedder, &corpus, argv[3], prefix, shortlist);
    }
    embedder_destroy(embedder);
    corpus_free(&corpus);
    if (status != TL_OK)
        fprintf(stderr, "vector evaluation: %s\n", tl_status_string(status));
    return status == TL_OK ? 0 : 1;
}
