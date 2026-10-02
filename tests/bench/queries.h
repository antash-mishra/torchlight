/* Labeled benchmark queries derived from corpus entries. Each query targets
 * one entry ("known item"); generation is deterministic for a seed, so one
 * seed can be used while tuning and another kept held out for reporting. */
#ifndef TORCHLIGHT_BENCH_QUERIES_H
#define TORCHLIGHT_BENCH_QUERIES_H
#include "corpus.h"
#include "torchlight/lexical.h"
enum query_kind {
    QUERY_EXACT,        /* the whole basename */
    QUERY_PREFIX,       /* the first ~60% of the stem */
    QUERY_ABBREVIATION, /* first letter and consonants of each stem word (prjnts) */
    QUERY_TYPO,         /* the stem with one edit in its longest word */
    QUERY_PARTIAL_TYPO, /* an incomplete stem prefix with one edit (projc) */
    QUERY_PARENT,       /* "<parent directory> <stem prefix>" */
    QUERY_KIND_COUNT
};
typedef struct {
    char text[LEXICAL_QUERY_BYTES + 1];
    enum query_kind kind;
    size_t target;
} bench_query;
/** Generate up to per_kind queries of each kind from random ASCII-named
 * entries of corpus. *out is owned by the caller (free()). TL_INVALID/TL_NOMEM. */
tl_status queries_generate(const bench_corpus *corpus, uint64_t seed, size_t per_kind,
                           bench_query **out, size_t *count);
/** Short stable name of kind for reports. */
const char *query_kind_name(enum query_kind kind);
/** Whether path satisfies query: same basename as the target (duplicates such
 * as README.md count), and for QUERY_PARENT also the same parent name. */
bool query_relevant(const bench_corpus *corpus, const bench_query *query, const char *path);
#endif
