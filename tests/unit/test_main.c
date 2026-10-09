/* Sanitizer-enabled test runner for every module. */
#include "test.h"
int main(void) {
    test_desktop();
    test_popup();
    test_windows();
    test_actions();
    test_config();
    test_core();
    test_sort();
    test_mask();
    test_parallel();
    test_json();
    test_ipc();
    test_async();
    test_tokenize();
    test_prefix();
    test_subseq();
    test_fuzzy();
    test_embed();
    test_potion();
    test_semantic();
    test_vector();
    test_rank();
    test_usage();
    test_trigram();
    test_typo();
    test_dirtree();
    test_lexical();
    test_lexical_boost();
    test_catalog();
    test_store();
    test_identity();
    test_crawl();
    test_watch();
    test_personal();
    test_writer();
    test_writer_fallback();
    test_writer_history();
    puts("All module tests passed.");
    return 0;
}
