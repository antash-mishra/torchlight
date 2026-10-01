/* Sanitizer-enabled test runner for the first M1 increment. */
#include "test.h"
int main(void) {
    test_config();
    test_core();
    test_tokenize();
    test_prefix();
    test_subseq();
    test_fuzzy();
    test_lexical();
    test_store();
    test_crawl();
    puts("All module tests passed.");
    return 0;
}
