/* NFC/casefold, boundary mapping and malformed-byte regression tests. */
#include "test.h"
#include <string.h>
static void equivalent(const char *a, const char *b) {
    tl_tokenized *left = test_text(a), *right = test_text(b);
    tl_text x = tokenize_view(left), y = tokenize_view(right);
    CHECK(x.length == y.length);
    CHECK(memcmp(x.symbols, y.symbols, x.length * sizeof(uint32_t)) == 0);
    CHECK(x.mask == y.mask);
    tokenize_destroy(left);
    tokenize_destroy(right);
}
static void invalid_sequences(void) {
    const char *fixtures[] = {"\xff",    "\xfe", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
                              "\xe2\x82"};
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        tl_tokenized *text = test_text(fixtures[i]);
        tl_text view = tokenize_view(text);
        CHECK(view.length == strlen(fixtures[i]));
        for (size_t j = 0; j < view.length; j++)
            CHECK(view.symbols[j] == TOKENIZE_OPAQUE_BASE + (unsigned char)fixtures[i][j]);
        tokenize_destroy(text);
    }
}
void test_tokenize(void) {
    equivalent("Caf\xc3\xa9", "CAFE\xcc\x81");
    equivalent("Stra\xc3\x9f"
               "e",
               "STRASSE");
    equivalent("\xce\xa3", "\xcf\x82");
    invalid_sequences();
    tl_tokenized *text = test_text("/projectNotes.md");
    tl_text view = tokenize_view(text);
    CHECK(view.basename == 1 && view.boundaries[8] != 0);
    CHECK(view.byte_offsets[8] == 8);
    tokenize_destroy(text);
    text = test_text("e\xcc\x81X");
    view = tokenize_view(text);
    CHECK(view.length == 2 && view.byte_offsets[1] == 3 && view.boundaries[1] != 0);
    tokenize_destroy(text);
    uint32_t symbols[8];
    uint8_t boundaries[8];
    size_t offsets[8];
    CHECK(tokenize_into("a\0b", 3, symbols, boundaries, offsets, 8, &view) == TL_INVALID);
    CHECK(tokenize_into("ab", 2, symbols, boundaries, offsets, 1, &view) == TL_LIMIT);
    char *display = NULL;
    CHECK(tokenize_display_create("a\xff\n\\b", &display) == TL_OK);
    CHECK(strcmp(display, "a\xef\xbf\xbd\\x0a\\\\b") == 0);
    tokenize_display_destroy(display);
}
