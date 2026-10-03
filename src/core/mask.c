/* Bitmap intersections reject rows missing required mask bits in word-sized
 * batches. Scratch owns all mutable state; the index is immutable and reusable. */
#include "torchlight/mask.h"
#include <stdlib.h>
#include <string.h>

enum { MASK_BITS = 64 };
struct tl_mask_index {
    uint64_t *postings;
    size_t count, words;
};
struct tl_mask_scratch {
    const tl_mask_index *index;
    uint64_t *matches;
    size_t cursor;
};
tl_status mask_index_create(const uint64_t *masks, size_t count, tl_mask_index **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (masks == NULL && count != 0)
        return TL_INVALID;
    size_t words = count / MASK_BITS + (count % MASK_BITS != 0), bytes = 0;
    tl_status status = tl_size_multiply(words, MASK_BITS * sizeof(uint64_t), &bytes);
    if (status != TL_OK)
        return status;
    tl_mask_index *index = calloc(1, sizeof(*index));
    if (index == NULL)
        return TL_NOMEM;
    index->postings = calloc(1, bytes == 0 ? 1 : bytes);
    if (index->postings == NULL) {
        mask_index_destroy(index);
        return TL_NOMEM;
    }
    index->count = count;
    index->words = words;
    for (size_t row = 0; row < count; row++) {
        uint64_t mask = masks[row];
        for (size_t bit = 0; mask != 0; bit++, mask >>= 1) {
            if ((mask & 1) != 0)
                index->postings[bit * words + row / MASK_BITS] |= UINT64_C(1) << (row % MASK_BITS);
        }
    }
    *out = index;
    return TL_OK;
}
void mask_index_destroy(tl_mask_index *index) {
    if (index == NULL)
        return;
    free(index->postings);
    free(index);
}
tl_status mask_scratch_create(const tl_mask_index *index, tl_mask_scratch **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (index == NULL)
        return TL_INVALID;
    tl_mask_scratch *scratch = calloc(1, sizeof(*scratch));
    if (scratch == NULL)
        return TL_NOMEM;
    size_t words = index->words == 0 ? 1 : index->words;
    scratch->matches = calloc(words, sizeof(uint64_t));
    if (scratch->matches == NULL) {
        mask_scratch_destroy(scratch);
        return TL_NOMEM;
    }
    scratch->index = index;
    *out = scratch;
    return TL_OK;
}
void mask_scratch_destroy(tl_mask_scratch *scratch) {
    if (scratch == NULL)
        return;
    free(scratch->matches);
    free(scratch);
}
tl_status mask_index_query(const tl_mask_index *index, tl_mask_scratch *scratch,
                           uint64_t required) {
    if (index == NULL || scratch == NULL || scratch->index != index)
        return TL_INVALID;
    memset(scratch->matches, 0xff, index->words * sizeof(uint64_t));
    for (size_t bit = 0; required != 0; bit++, required >>= 1) {
        if ((required & 1) == 0)
            continue;
        const uint64_t *posting = index->postings + bit * index->words;
        for (size_t word = 0; word < index->words; word++)
            scratch->matches[word] &= posting[word];
    }
    size_t tail = index->count % MASK_BITS;
    if (tail != 0)
        scratch->matches[index->words - 1] &= (UINT64_C(1) << tail) - 1;
    scratch->cursor = 0;
    return TL_OK;
}
tl_status mask_index_include(tl_mask_scratch *scratch, size_t row) {
    if (scratch == NULL || row >= scratch->index->count)
        return TL_INVALID;
    scratch->matches[row / MASK_BITS] |= UINT64_C(1) << (row % MASK_BITS);
    return TL_OK;
}
size_t mask_index_count(const tl_mask_scratch *scratch) {
    if (scratch == NULL)
        return 0;
    size_t count = 0;
    for (size_t word = scratch->cursor; word < scratch->index->words; word++)
        count += (size_t)__builtin_popcountll(scratch->matches[word]);
    return count;
}
bool mask_index_next(tl_mask_scratch *scratch, size_t *out) {
    if (scratch == NULL || out == NULL)
        return false;
    while (scratch->cursor < scratch->index->words) {
        uint64_t word = scratch->matches[scratch->cursor];
        if (word == 0) {
            scratch->cursor++;
            continue;
        }
        *out = scratch->cursor * MASK_BITS + (size_t)__builtin_ctzll(word);
        scratch->matches[scratch->cursor] = word & (word - 1);
        return true;
    }
    return false;
}
