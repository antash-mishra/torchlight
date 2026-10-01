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
/** Run byte/Unicode normalization checks, aborting on failure. */
void test_tokenize(void);
/** Run prefix-channel checks, aborting on failure. */
void test_prefix(void);
/** Run pure subsequence checks, aborting on failure. */
void test_subseq(void);
/** Run scorer checks, aborting on failure. */
void test_fuzzy(void);
/** Run orchestrator/ordering/workspace checks, aborting on failure. */
void test_lexical(void);
/** Run transactional catalog checks, aborting on failure. */
void test_store(void);
/** Run physical crawler checks, aborting on failure. */
void test_crawl(void);
#endif
