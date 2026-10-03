/* Sanitizer-enabled test runner for every module. */
#include "test.h"
int main(void) {
    test_config();
    test_core();
    test_sort();
    test_mask();
    test_parallel();
    test_json();
    test_ipc();
    test_tokenize();
    test_prefix();
    test_subseq();
    test_fuzzy();
    test_trigram();
    test_typo();
    test_dirtree();
    test_lexical();
    test_catalog();
    test_store();
    test_identity();
    test_crawl();
    test_watch();
    test_writer();
    test_writer_fallback();
    puts("All module tests passed.");
    return 0;
}
