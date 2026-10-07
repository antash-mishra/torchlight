/* Trigram posting lists keyed by exact packed symbol triples, queried by
 * relaxed overlap counting with per-query scratch. */
#include "torchlight/trigram.h"
#include "torchlight/hashmap.h"
#include "torchlight/sort.h"
#include "torchlight/vec.h"
#include <stdlib.h>
#include <string.h>
enum {
    /* Every scalar value and opaque byte symbol is below 2^21, so three
     * symbols pack exactly into 63 bits: keys never collide. */
    TRIGRAM_SYMBOL_BITS = 21,
    /* A trigram in more than 1/8 of names barely narrows candidates while its
     * posting list dominates query cost (e.g. "txt"), so queries ignore it. */
    TRIGRAM_FREQUENT_DIVISOR = 8,
    /* Small corpora never treat trigrams as frequent. */
    TRIGRAM_FREQUENT_FLOOR = 64
};
struct pair {
    uint32_t id, slot;
};
struct tl_trigram {
    tl_hashmap *ids;
    tl_vec *keys, *pairs, *local;
    uint32_t *starts, *postings;
    size_t slot_count, frequent_limit;
    bool finished;
};
/* Posting range of one informative query trigram. */
struct posting_range {
    uint32_t start, end;
};
struct tl_trigram_scratch {
    const tl_trigram *index;
    uint16_t *counts;
    uint32_t *touched;
    uint64_t keys[TRIGRAM_MAX_QUERY_SYMBOLS];
    struct posting_range ranges[TRIGRAM_MAX_QUERY_SYMBOLS];
};
struct key_lookup {
    const uint64_t *keys;
    uint64_t key;
};
static bool same_key(const void *context, uint32_t value) {
    const struct key_lookup *lookup = context;
    return lookup->keys[value] == lookup->key;
}
static uint64_t key_hash(uint64_t key) {
    return hashmap_hash(HASHMAP_HASH_SEED, &key, sizeof(key));
}
/* Write the sorted distinct trigram keys of text into keys (capacity at least
 * text.length); TL_INVALID if a symbol does not fit the packing. */
static tl_status distinct_trigrams(tl_text text, uint64_t *keys, size_t *out_count) {
    *out_count = 0;
    if (text.length < 3)
        return TL_OK;
    const uint32_t limit = UINT32_C(1) << TRIGRAM_SYMBOL_BITS;
    for (size_t i = 0; i + 2 < text.length; i++) {
        const uint32_t *s = text.symbols + i;
        if (s[0] >= limit || s[1] >= limit || s[2] >= limit)
            return TL_INVALID;
        keys[i] = (uint64_t)s[0] << (2 * TRIGRAM_SYMBOL_BITS) |
                  (uint64_t)s[1] << TRIGRAM_SYMBOL_BITS | s[2];
    }
    size_t count = text.length - 2, unique = 0;
    tl_status status = sort_u64(keys, count);
    if (status != TL_OK)
        return status;
    for (size_t i = 0; i < count; i++) {
        if (i == 0 || keys[i] != keys[unique - 1])
            keys[unique++] = keys[i];
    }
    *out_count = unique;
    return TL_OK;
}
tl_status trigram_create(tl_trigram **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_trigram *index = calloc(1, sizeof(*index));
    if (index == NULL)
        return TL_NOMEM;
    tl_status status = hashmap_create(&index->ids);
    if (status == TL_OK)
        status = vec_create(sizeof(uint64_t), &index->keys);
    if (status == TL_OK)
        status = vec_create(sizeof(struct pair), &index->pairs);
    if (status == TL_OK)
        status = vec_create(sizeof(uint64_t), &index->local);
    if (status != TL_OK) {
        trigram_destroy(index);
        return status;
    }
    *out = index;
    return TL_OK;
}
void trigram_destroy(tl_trigram *index) {
    if (index == NULL)
        return;
    hashmap_destroy(index->ids);
    vec_destroy(index->keys);
    vec_destroy(index->pairs);
    vec_destroy(index->local);
    free(index->starts);
    free(index->postings);
    free(index);
}
static tl_status trigram_id(tl_trigram *index, uint64_t key, uint32_t *out) {
    struct key_lookup lookup = {vec_const_data(index->keys), key};
    uint64_t hash = key_hash(key);
    if (hashmap_find(index->ids, hash, same_key, &lookup, out))
        return TL_OK;
    size_t id = vec_count(index->keys);
    if (id >= UINT32_MAX)
        return TL_LIMIT;
    tl_status status = vec_append(index->keys, &key);
    if (status == TL_OK)
        status = hashmap_insert(index->ids, hash, (uint32_t)id);
    *out = (uint32_t)id;
    return status;
}
tl_status trigram_add(tl_trigram *index, tl_text text, size_t slot) {
    if (index == NULL || (text.symbols == NULL && text.length != 0))
        return TL_INVALID;
    if (index->finished)
        return TL_STATE;
    if (slot >= UINT32_MAX)
        return TL_LIMIT;
    /* Strictly increasing slots keep postings sorted and free of duplicates. */
    if (slot < index->slot_count)
        return TL_INVALID;
    index->slot_count = slot + 1;
    tl_status status = vec_reserve(index->local, text.length);
    size_t count = 0;
    if (status == TL_OK)
        status = distinct_trigrams(text, vec_data(index->local), &count);
    const uint64_t *keys = vec_const_data(index->local);
    for (size_t i = 0; i < count && status == TL_OK; i++) {
        struct pair pair = {.slot = (uint32_t)slot};
        status = trigram_id(index, keys[i], &pair.id);
        if (status == TL_OK)
            status = vec_append(index->pairs, &pair);
    }
    return status;
}
/* Counting sort of (id, slot) pairs into per-id posting lists. Pairs arrive in
 * nondecreasing slot order, so every posting list is sorted by slot. */
static tl_status build_postings(tl_trigram *index) {
    size_t ids = vec_count(index->keys), pairs_count = vec_count(index->pairs);
    if (pairs_count > UINT32_MAX)
        return TL_LIMIT;
    index->starts = calloc(ids + 1, sizeof(uint32_t));
    index->postings = malloc((pairs_count == 0 ? 1 : pairs_count) * sizeof(uint32_t));
    uint32_t *cursor = malloc((ids == 0 ? 1 : ids) * sizeof(uint32_t));
    if (index->starts == NULL || index->postings == NULL || cursor == NULL) {
        free(cursor);
        return TL_NOMEM;
    }
    const struct pair *pairs = vec_const_data(index->pairs);
    for (size_t i = 0; i < pairs_count; i++)
        index->starts[pairs[i].id + 1]++;
    for (size_t id = 0; id < ids; id++) {
        index->starts[id + 1] += index->starts[id];
        cursor[id] = index->starts[id];
    }
    for (size_t i = 0; i < pairs_count; i++)
        index->postings[cursor[pairs[i].id]++] = pairs[i].slot;
    free(cursor);
    return TL_OK;
}
tl_status trigram_finish(tl_trigram *index) {
    if (index == NULL)
        return TL_INVALID;
    if (index->finished)
        return TL_STATE;
    tl_status status = build_postings(index);
    if (status != TL_OK)
        return status;
    vec_destroy(index->pairs);
    vec_destroy(index->local);
    index->pairs = index->local = NULL;
    vec_shrink(index->keys);
    size_t limit = index->slot_count / TRIGRAM_FREQUENT_DIVISOR;
    index->frequent_limit = limit > TRIGRAM_FREQUENT_FLOOR ? limit : TRIGRAM_FREQUENT_FLOOR;
    index->finished = true;
    return TL_OK;
}
tl_status trigram_scratch_create(const tl_trigram *index, tl_trigram_scratch **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (index == NULL)
        return TL_INVALID;
    if (!index->finished)
        return TL_STATE;
    size_t slots = index->slot_count == 0 ? 1 : index->slot_count;
    tl_trigram_scratch *scratch = calloc(1, sizeof(*scratch));
    if (scratch == NULL)
        return TL_NOMEM;
    scratch->counts = calloc(slots, sizeof(uint16_t));
    scratch->touched = malloc(slots * sizeof(uint32_t));
    if (scratch->counts == NULL || scratch->touched == NULL) {
        trigram_scratch_destroy(scratch);
        return TL_NOMEM;
    }
    scratch->index = index;
    *out = scratch;
    return TL_OK;
}
void trigram_scratch_destroy(tl_trigram_scratch *scratch) {
    if (scratch == NULL)
        return;
    free(scratch->counts);
    free(scratch->touched);
    free(scratch);
}
static bool find_key(const tl_trigram *index, uint64_t key, uint32_t *id) {
    struct key_lookup lookup = {vec_const_data(index->keys), key};
    return hashmap_find(index->ids, key_hash(key), same_key, &lookup, id);
}
/* Frequent in decider: more postings than its limit; absent keys never are. */
static bool frequent(const tl_trigram *decider, uint64_t key) {
    uint32_t id = 0;
    return find_key(decider, key, &id) &&
           decider->starts[id + 1] - decider->starts[id] > decider->frequent_limit;
}
/* Collect the posting ranges of the query's informative trigrams, shortest
 * first. Returns the informative total: absent trigrams count (as empty
 * lists), frequent ones (per decider) do not. */
static size_t informative_ranges(const tl_trigram *index, const tl_trigram *decider,
                                 tl_trigram_scratch *scratch, size_t keys, size_t *range_count) {
    size_t informative = 0;
    *range_count = 0;
    for (size_t k = 0; k < keys; k++) {
        uint32_t id = 0;
        if (frequent(decider, scratch->keys[k]))
            continue;
        informative++;
        if (!find_key(index, scratch->keys[k], &id))
            continue;
        struct posting_range range = {index->starts[id], index->starts[id + 1]};
        /* Insertion sort by length: a query has few trigrams. */
        size_t at = (*range_count)++;
        while (at > 0 && scratch->ranges[at - 1].end - scratch->ranges[at - 1].start >
                             range.end - range.start) {
            scratch->ranges[at] = scratch->ranges[at - 1];
            at--;
        }
        scratch->ranges[at] = range;
    }
    return informative;
}
static bool posted(const tl_trigram *index, struct posting_range range, uint32_t slot) {
    uint32_t low = range.start, high = range.end;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (index->postings[middle] < slot)
            low = middle + 1;
        else
            high = middle;
    }
    return low < range.end && index->postings[low] == slot;
}
/* Count, per candidate slot, how many informative trigrams it contains. A
 * slot sharing at least `needed` of the present lists appears in one of the
 * (present - needed + 1) shortest lists, so only those are scanned to find
 * candidates; the longer lists are probed per candidate by binary search on
 * their sorted postings. Counts of reported slots are exact. */
static void count_overlap(const tl_trigram *index, tl_trigram_scratch *scratch, size_t ranges,
                          size_t needed, size_t *touched_count) {
    if (needed == 0 || ranges < needed)
        return;
    size_t scanned = ranges - needed + 1;
    for (size_t r = 0; r < scanned; r++) {
        for (uint32_t p = scratch->ranges[r].start; p < scratch->ranges[r].end; p++) {
            uint32_t slot = index->postings[p];
            if (scratch->counts[slot]++ == 0)
                scratch->touched[(*touched_count)++] = slot;
        }
    }
    for (size_t r = scanned; r < ranges; r++) {
        struct posting_range range = scratch->ranges[r];
        size_t length = range.end - range.start, probes = 1;
        for (size_t span = length; span > 1; span /= 2)
            probes++;
        /* Probe when candidates are few; otherwise one pass that only counts
         * slots already found is cheaper than repeated binary searches. */
        if (*touched_count > length / probes) {
            for (uint32_t p = range.start; p < range.end; p++)
                if (scratch->counts[index->postings[p]] != 0)
                    scratch->counts[index->postings[p]]++;
            continue;
        }
        for (size_t i = 0; i < *touched_count; i++) {
            uint32_t slot = scratch->touched[i];
            if (posted(index, range, slot))
                scratch->counts[slot]++;
        }
    }
}
tl_status trigram_query(const tl_trigram *index, const tl_trigram *reference,
                        tl_trigram_scratch *scratch, tl_text query, tl_trigram_hit hit,
                        void *context) {
    if (index == NULL || scratch == NULL || scratch->index != index || hit == NULL ||
        (query.symbols == NULL && query.length != 0))
        return TL_INVALID;
    if (!index->finished || (reference != NULL && !reference->finished))
        return TL_STATE;
    if (query.length > TRIGRAM_MAX_QUERY_SYMBOLS)
        return TL_LIMIT;
    size_t keys = 0, touched = 0, ranges = 0;
    tl_status status = distinct_trigrams(query, scratch->keys, &keys);
    if (status != TL_OK || keys < TRIGRAM_MIN_QUERY_TRIGRAMS)
        return status;
    size_t total =
        informative_ranges(index, reference == NULL ? index : reference, scratch, keys, &ranges);
    size_t needed = (total + 1) / 2;
    count_overlap(index, scratch, ranges, needed, &touched);
    for (size_t i = 0; i < touched && status == TL_OK; i++) {
        uint32_t slot = scratch->touched[i];
        if (scratch->counts[slot] >= needed)
            status = hit(context, slot, scratch->counts[slot], total);
    }
    /* Reset every touched counter, even after a callback error, so the
     * scratch stays reusable. */
    for (size_t i = 0; i < touched; i++)
        scratch->counts[scratch->touched[i]] = 0;
    return status;
}
