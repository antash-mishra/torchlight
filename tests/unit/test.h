/* Shared test assertions and module test entry points. */
#ifndef TORCHLIGHT_TEST_H
#define TORCHLIGHT_TEST_H
#include "torchlight/tokenize.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                        \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
/** Construct a normalized fixture owned by caller; abort on unexpected error. */
static inline tl_tokenized *test_text(const char *path) {
    tl_tokenized *text = NULL;
    CHECK(tokenize_create(path, &text) == TL_OK);
    return text;
}
/** Run XDG/override checks, aborting on failure. */
void test_config(void);
/** Run isolated core utility checks, aborting on failure. */
void test_core(void);
/** Run allocation-free integer sort checks, aborting on failure. */
void test_sort(void);
/** Test bitmap filtering against a scalar mask scan. */
void test_mask(void);
/** Test worker partitions, errors and repeated dispatch. */
void test_parallel(void);
/** Run strict JSON, Unicode/base64 and component-aware scope checks. */
void test_json(void);
/** Run protocol and singleton socket lifecycle checks. */
void test_ipc(void);
/** Run rename, relocated-watch, overflow and exhaustion checks. */
void test_watch(void);
/** Run writer publication recovery, history saturation and overflow acceptance. */
void test_writer(void);
/** Run periodic scan and watcher recovery checks under instance exhaustion. */
void test_writer_fallback(void);
/** Run byte/Unicode normalization checks, aborting on failure. */
void test_tokenize(void);
/** Run prefix-channel checks, aborting on failure. */
void test_prefix(void);
/** Run pure subsequence checks, aborting on failure. */
void test_subseq(void);
/** Run trigram-channel checks, aborting on failure. */
void test_trigram(void);
/** Run typo-channel checks, aborting on failure. */
void test_typo(void);
/** Run directory-tree checks, aborting on failure. */
void test_dirtree(void);
/** Run scorer checks, aborting on failure. */
void test_fuzzy(void);
/** Run orchestrator/ordering/workspace checks, aborting on failure. */
void test_lexical(void);
/** Run resident snapshot lifetime, capacity and concurrent-publication checks. */
void test_catalog(void);
/** Run transactional catalog checks, aborting on failure. */
void test_store(void);
/** Run filesystem incarnation, descendant retirement and fallback identity checks. */
void test_identity(void);
/** Run physical crawler checks, aborting on failure. */
void test_crawl(void);
#endif
