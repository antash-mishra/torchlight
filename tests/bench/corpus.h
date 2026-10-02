/* Benchmark corpora: a deterministic home-directory-like synthetic corpus, or
 * a NUL-separated list of real absolute paths (e.g. from `find / -print0`). */
#ifndef TORCHLIGHT_BENCH_CORPUS_H
#define TORCHLIGHT_BENCH_CORPUS_H
#include "torchlight/common.h"
#include <stdbool.h>
typedef struct {
    char **paths;  /* paths[i] points into buffer; NUL-terminated */
    bool *is_root; /* entries the engine should treat as indexed roots */
    char *buffer;
    size_t count, bytes;
} bench_corpus;
/** Fill out with count synthetic paths generated from seed. The first entry is
 * the root "/home/user"; a few fixed fixtures (projectNotes.md, README.md,
 * Café.pdf, finance/invoice2024.pdf) are always present. Owned by out until
 * corpus_free. TL_INVALID for count < 16, TL_NOMEM. */
tl_status corpus_synthetic(size_t count, uint64_t seed, bench_corpus *out);
/** Load up to limit (0 = all) NUL-separated absolute paths from file; other
 * records are skipped. The shortest path is marked as the root. TL_IO when the
 * file cannot be read or holds no usable path, TL_NOMEM. */
tl_status corpus_load(const char *file, size_t limit, bench_corpus *out);
/** Free a corpus filled by corpus_synthetic/corpus_load; NULL allowed. */
void corpus_free(bench_corpus *corpus);
/** Deterministic splitmix64 step shared by the benchmark generators. */
uint64_t corpus_random(uint64_t *state);
#endif
