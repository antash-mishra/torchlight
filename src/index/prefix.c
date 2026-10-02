/* Binary-search prefix ranges over sorted keys. Basename/token keys borrow the
 * caller's normalized symbols; initials live in one index-owned arena. */
#include "torchlight/prefix.h"
#include "torchlight/vec.h"
#include <stdlib.h>
/* A key. Initials are recorded as pending arena offsets until finish, because
 * the arena may still move while it grows; only then do they get pointers. */
struct prefix_key {
    const uint32_t *symbols;
    uint32_t length, slot;
    int score;
};
struct pending_initials {
    uint32_t offset, length, slot;
};
struct tl_prefix {
    tl_vec *keys, *initials, *pending;
    struct prefix_key *sorted;
    size_t sorted_count;
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
    if (status == TL_OK)
        status = vec_create(sizeof(uint32_t), &index->initials);
    if (status == TL_OK)
        status = vec_create(sizeof(struct pending_initials), &index->pending);
    if (status != TL_OK) {
        prefix_destroy(index);
        return status;
    }
    *out = index;
    return TL_OK;
}
void prefix_destroy(tl_prefix *index) {
    if (index == NULL)
        return;
    vec_destroy(index->keys);
    vec_destroy(index->initials);
    vec_destroy(index->pending);
    free(index->sorted);
    free(index);
}
static tl_status add_key(tl_prefix *index, tl_text text, size_t start, size_t end, size_t slot,
                         int score) {
    if (start == end)
        return TL_OK;
    struct prefix_key key = {.symbols = text.symbols + start,
                             .length = (uint32_t)(end - start),
                             .slot = (uint32_t)slot,
                             .score = score};
    return vec_append(index->keys, &key);
}
static bool initial_at(tl_text text, size_t i) {
    return !tokenize_separator(text.symbols[i]) && (i == text.basename || text.boundaries[i] != 0);
}
static tl_status add_initials(tl_prefix *index, tl_text text, size_t slot) {
    size_t offset = vec_count(index->initials);
    if (offset > UINT32_MAX)
        return TL_LIMIT;
    for (size_t i = text.basename; i < text.length; i++) {
        if (!initial_at(text, i))
            continue;
        tl_status status = vec_append(index->initials, &text.symbols[i]);
        if (status != TL_OK)
            return status;
    }
    size_t length = vec_count(index->initials) - offset;
    if (length == 0)
        return TL_OK;
    struct pending_initials pending = {(uint32_t)offset, (uint32_t)length, (uint32_t)slot};
    return vec_append(index->pending, &pending);
}
tl_status prefix_add(tl_prefix *index, tl_text text, size_t slot) {
    if (index == NULL || text.symbols == NULL || text.boundaries == NULL ||
        text.basename > text.length)
        return TL_INVALID;
    if (index->finished)
        return TL_STATE;
    if (slot > UINT32_MAX || text.length > UINT32_MAX)
        return TL_LIMIT;
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
/* Combine borrowed keys and arena-resolved initials into one sorted table. */
tl_status prefix_finish(tl_prefix *index) {
    if (index == NULL)
        return TL_INVALID;
    if (index->finished)
        return TL_STATE;
    vec_shrink(index->initials);
    size_t borrowed = vec_count(index->keys), owned = vec_count(index->pending);
    size_t count = borrowed + owned, bytes = 0;
    tl_status status = tl_size_multiply(count == 0 ? 1 : count, sizeof(struct prefix_key), &bytes);
    if (status != TL_OK)
        return status;
    index->sorted = malloc(bytes);
    if (index->sorted == NULL)
        return TL_NOMEM;
    const struct prefix_key *keys = vec_const_data(index->keys);
    for (size_t i = 0; i < borrowed; i++)
        index->sorted[i] = keys[i];
    const struct pending_initials *pending = vec_const_data(index->pending);
    const uint32_t *arena = vec_const_data(index->initials);
    for (size_t i = 0; i < owned; i++)
        index->sorted[borrowed + i] = (struct prefix_key){.symbols = arena + pending[i].offset,
                                                          .length = pending[i].length,
                                                          .slot = pending[i].slot,
                                                          .score = PREFIX_INITIALS_SCORE};
    vec_destroy(index->keys);
    vec_destroy(index->pending);
    index->keys = index->pending = NULL;
    if (count > 1)
        qsort(index->sorted, count, sizeof(struct prefix_key), compare_keys);
    index->sorted_count = count;
    index->finished = true;
    return TL_OK;
}
/* Compare only a key's first query.length symbols with the query, so every
 * key starting with the query compares equal and keys form ordered ranges. */
static int compare_prefix(const struct prefix_key *key, tl_text query) {
    size_t length = key->length < query.length ? key->length : query.length;
    int order = compare_symbols(key->symbols, length, query.symbols, length);
    if (order != 0)
        return order;
    return key->length < query.length ? -1 : 0;
}
/* First key whose prefix compares >= query (want_greater false) or > query. */
static size_t bound(const tl_prefix *index, tl_text query, bool want_greater) {
    size_t lo = 0, hi = index->sorted_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int order = compare_prefix(&index->sorted[mid], query);
        if (order < 0 || (want_greater && order == 0))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
tl_status prefix_query(const tl_prefix *index, tl_text query, tl_prefix_hit hit, void *context) {
    if (index == NULL || hit == NULL || query.symbols == NULL || query.length == 0)
        return TL_INVALID;
    if (!index->finished)
        return TL_STATE;
    /* Two binary searches bound the matching range, so expanding even a huge
     * range costs no further symbol comparisons. */
    size_t end = bound(index, query, true);
    for (size_t i = bound(index, query, false); i < end; i++) {
        tl_status status = hit(context, index->sorted[i].slot, index->sorted[i].score);
        if (status != TL_OK)
            return status;
    }
    return TL_OK;
}
