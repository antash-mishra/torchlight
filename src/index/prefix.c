/* Binary-search prefix ranges; keys retain basename priority without copying. */
#include "torchlight/prefix.h"
#include "torchlight/vec.h"
#include <stdlib.h>
enum {
    PREFIX_BASENAME_SCORE = 6000,
    PREFIX_TOKEN_SCORE = 5000,
    PREFIX_INITIALS_SCORE = 4500,
    PREFIX_PARENT_SCORE = 1000
};
struct prefix_key {
    const uint32_t *symbols;
    size_t length, slot;
    int score;
    bool owned;
};
struct tl_prefix {
    tl_vec *keys;
    size_t slot_count;
    bool finished;
};
static int compare_symbols(const uint32_t *a, size_t na, const uint32_t *b, size_t nb) {
    size_t count = na < nb ? na : nb;
    for (size_t i = 0; i < count; i++) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return na == nb ? 0 : na < nb ? -1 : 1;
}
static int compare_keys(const void *left, const void *right) {
    const struct prefix_key *a = left, *b = right;
    int order = compare_symbols(a->symbols, a->length, b->symbols, b->length);
    if (order != 0)
        return order;
    return a->slot == b->slot ? 0 : a->slot < b->slot ? -1 : 1;
}
tl_status prefix_create(tl_prefix **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_prefix *index = calloc(1, sizeof(*index));
    if (index == NULL)
        return TL_NOMEM;
    tl_status status = vec_create(sizeof(struct prefix_key), &index->keys);
    if (status != TL_OK) {
        free(index);
        return status;
    }
    *out = index;
    return TL_OK;
}
void prefix_destroy(tl_prefix *index) {
    if (index == NULL)
        return;
    const struct prefix_key *keys = vec_const_data(index->keys);
    for (size_t i = 0; i < vec_count(index->keys); i++) {
        if (keys[i].owned)
            free((void *)keys[i].symbols);
    }
    vec_destroy(index->keys);
    free(index);
}
static tl_status add_key(tl_prefix *index, tl_text text, size_t start, size_t end, size_t slot,
                         int score) {
    if (start == end)
        return TL_OK;
    struct prefix_key key = {
        .symbols = text.symbols + start, .length = end - start, .slot = slot, .score = score};
    return vec_append(index->keys, &key);
}
static tl_status add_initials(tl_prefix *index, tl_text text, size_t slot) {
    size_t count = 0;
    for (size_t i = text.basename; i < text.length; i++) {
        if (!tokenize_separator(text.symbols[i]) && (i == text.basename || text.boundaries[i] != 0))
            count++;
    }
    if (count == 0)
        return TL_OK;
    size_t bytes = 0;
    tl_status status = tl_size_multiply(count, sizeof(uint32_t), &bytes);
    if (status != TL_OK)
        return status;
    uint32_t *initials = malloc(bytes);
    if (initials == NULL)
        return TL_NOMEM;
    count = 0;
    for (size_t i = text.basename; i < text.length; i++) {
        if (!tokenize_separator(text.symbols[i]) && (i == text.basename || text.boundaries[i] != 0))
            initials[count++] = text.symbols[i];
    }
    struct prefix_key key = {.symbols = initials,
                             .length = count,
                             .slot = slot,
                             .score = PREFIX_INITIALS_SCORE,
                             .owned = true};
    status = vec_append(index->keys, &key);
    if (status != TL_OK)
        free(initials);
    return status;
}
tl_status prefix_add(tl_prefix *index, tl_text text, size_t slot) {
    if (index == NULL || text.symbols == NULL || text.boundaries == NULL ||
        text.basename > text.length)
        return TL_INVALID;
    if (index->finished)
        return TL_STATE;
    if (slot == SIZE_MAX)
        return TL_LIMIT;
    if (index->slot_count <= slot)
        index->slot_count = slot + 1;
    tl_status status =
        add_key(index, text, text.basename, text.length, slot, PREFIX_BASENAME_SCORE);
    if (status != TL_OK)
        return status;
    for (size_t start = 0; start < text.length;) {
        if (tokenize_separator(text.symbols[start])) {
            start++;
            continue;
        }
        size_t end = start + 1;
        while (end < text.length && !tokenize_separator(text.symbols[end]) &&
               text.boundaries[end] == 0)
            end++;
        status = add_key(index, text, start, end, slot,
                         start >= text.basename ? PREFIX_TOKEN_SCORE : PREFIX_PARENT_SCORE);
        if (status != TL_OK)
            return status;
        start = end;
    }
    return add_initials(index, text, slot);
}
tl_status prefix_finish(tl_prefix *index) {
    if (index == NULL)
        return TL_INVALID;
    if (index->finished)
        return TL_STATE;
    if (vec_count(index->keys) > 1)
        qsort(vec_data(index->keys), vec_count(index->keys), sizeof(struct prefix_key),
              compare_keys);
    index->finished = true;
    return TL_OK;
}
tl_status prefix_query(const tl_prefix *index, tl_text query, int *scores, size_t scores_count) {
    if (index == NULL || scores == NULL || query.symbols == NULL || query.length == 0)
        return TL_INVALID;
    if (!index->finished)
        return TL_STATE;
    if (scores_count < index->slot_count)
        return TL_LIMIT;
    const struct prefix_key *keys = vec_const_data(index->keys);
    size_t lo = 0, hi = vec_count(index->keys);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (compare_symbols(keys[mid].symbols, keys[mid].length, query.symbols, query.length) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t i = lo; i < vec_count(index->keys); i++) {
        if (keys[i].length < query.length ||
            compare_symbols(keys[i].symbols, query.length, query.symbols, query.length) != 0)
            break;
        if (scores[keys[i].slot] < keys[i].score)
            scores[keys[i].slot] = keys[i].score;
    }
    return TL_OK;
}
