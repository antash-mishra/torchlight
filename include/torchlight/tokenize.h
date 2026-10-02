/* Versioned byte-preserving normalization and boundary metadata. */
#ifndef TORCHLIGHT_TOKENIZE_H
#define TORCHLIGHT_TOKENIZE_H
#include "torchlight/common.h"
#include <stdbool.h>
/* Increment this contract when normalization or boundary rules change. */
#define TOKENIZE_VERSION "utf8proc-nfc-casefold-opaque-v2"
#define TOKENIZE_OPAQUE_BASE UINT32_C(0x110000)
typedef struct tl_tokenized tl_tokenized;
typedef struct {
    const uint32_t *symbols;
    const uint8_t *boundaries;
    const size_t *byte_offsets;
    size_t length, basename;
    uint64_t mask;
} tl_text;
/** Decode length non-NUL bytes into caller buffers with capacity elements.
 * byte_offsets maps each symbol to the start of its original grapheme cluster.
 * boundaries marks word starts: after a separator, lower->upper case, the last
 * capital of an acronym before lowercase (HTML|Parser), and letter<->digit.
 * out borrows buffers. TL_INVALID rejects NUL/NULL; TL_LIMIT rejects insufficient capacity (case
 * folding may expand). Valid UTF-8 clusters are NFC-normalized and Unicode case-folded; each
 * invalid byte maps to a distinct opaque symbol. */
tl_status tokenize_into(const char *bytes, size_t length, uint32_t *symbols, uint8_t *boundaries,
                        size_t *byte_offsets, size_t capacity, tl_text *out);
/** Create owned normalized text from a NUL-terminated raw path; out is NULL on
 * invalid input/overflow/allocation failure. Original path is not retained. */
tl_status tokenize_create(const char *path, tl_tokenized **out);
/** Destroy owned normalized text; NULL is allowed. */
void tokenize_destroy(tl_tokenized *text);
/** Borrow view until text destruction; NULL text returns an empty view. */
tl_text tokenize_view(const tl_tokenized *text);
/** Return a conservative mask bit for any scalar/opaque symbol; cannot fail. */
uint64_t tokenize_symbol_mask(uint32_t symbol);
/** Test a separator used by token matching; no errors or ownership transfer. */
bool tokenize_separator(uint32_t symbol);
/** Create a valid UTF-8 display from raw bytes, replacing invalid bytes with
 * U+FFFD and escaping ASCII controls/backslashes. out is owned and NULL on
 * invalid/allocation/overflow errors; release with tokenize_display_destroy. */
tl_status tokenize_display_create(const char *path, char **out);
/** Free an owned display string; NULL allowed. */
void tokenize_display_destroy(char *display);
#endif
