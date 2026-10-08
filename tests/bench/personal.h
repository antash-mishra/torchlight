/* M5 personal ranking benchmark (ADR 0033): typing latency with boosted
 * entries attached, the cost of computing boosts from a full usage summary,
 * and a synthetic usage scenario comparing personalized with plain ranking. */
#ifndef TORCHLIGHT_BENCH_PERSONAL_H
#define TORCHLIGHT_BENCH_PERSONAL_H
#include "corpus.h"
#include "queries.h"
#include "torchlight/lexical.h"
/** Run every personal measurement on engine (built from corpus with id
 * i + 1 for corpus entry i) using workspace, typing the held-out queries for
 * latency. Leaves the workspace without boosts. Prints report lines;
 * TL_NOMEM/TL_IO or query errors. */
tl_status personal_benchmark(const bench_corpus *corpus, const tl_lexical *engine,
                             tl_lexical_workspace *workspace, const bench_query *held_out,
                             size_t held_out_count);
#endif
