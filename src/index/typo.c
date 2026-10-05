/* Whole-token deletion lookup and bounded prefix edits over sorted token ranges.
 * Every distinct token is stored under its
 * own hash and the hash of each one-symbol deletion. A query probes the same
 * keys; any two strings within one edit (including an adjacent swap) share a
 * key, and fuzzy_edit_distance removes the false candidates. */
#include "torchlight/typo.h"
#include "torchlight/fuzzy.h"
#include "torchlight/hashmap.h"
#include "torchlight/vec.h"
#include <stdlib.h>
#include <string.h>
/* One occurrence of a token during construction. */
struct occurrence {
    uint64_t hash;
    const uint32_t *symbols;
    uint32_t length, slot;
};
struct term {
    const uint32_t *symbols;
    uint32_t length, postings_start;
};
struct deletion_key {
    uint64_t hash;
    uint32_t term;
};
struct tl_typo {
    tl_vec *occurrences;
    struct term *terms;
    uint32_t *postings;
    struct deletion_key *keys;
    size_t term_count, posting_count, key_count;
    bool finished;
};
struct tl_typo_scratch {
    const tl_typo *index;
    /* stamps[term] == epoch marks terms already verified for this query. */
    uint32_t *stamps, epoch;
};
static uint64_t hash_symbols(const uint32_t *symbols, size_t length) {
    return hashmap_hash(HASHMAP_HASH_SEED, symbols, length * sizeof(uint32_t));
}
/* Hash of symbols with the symbol at skip removed, without copying. */
static uint64_t hash_deletion(const uint32_t *symbols, size_t length, size_t skip) {
    uint64_t state = hashmap_hash(HASHMAP_HASH_SEED, symbols, skip * sizeof(uint32_t));
    return hashmap_hash(state, symbols + skip + 1, (length - skip - 1) * sizeof(uint32_t));
}
static int compare_symbols(const uint32_t *a, size_t na, const uint32_t *b, size_t nb) {
    size_t length = na < nb ? na : nb;
    for (size_t i = 0; i < length; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return na == nb ? 0 : na < nb ? -1 : 1;
}
static int compare_occurrences(const void *left, const void *right) {
    const struct occurrence *a = left, *b = right;
    int order = compare_symbols(a->symbols, a->length, b->symbols, b->length);
    if (order != 0)
        return order;
    return a->slot == b->slot ? 0 : a->slot < b->slot ? -1 : 1;
}
static int compare_deletion_keys(const void *left, const void *right) {
    const struct deletion_key *a = left, *b = right;
    if (a->hash != b->hash)
        return a->hash < b->hash ? -1 : 1;
    return a->term == b->term ? 0 : a->term < b->term ? -1 : 1;
}
tl_status typo_create(tl_typo **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_typo *index = calloc(1, sizeof(*index));
    if (index == NULL)
        return TL_NOMEM;
    tl_status status = vec_create(sizeof(struct occurrence), &index->occurrences);
    if (status != TL_OK) {
        free(index);
        return status;
    }
    *out = index;
    return TL_OK;
}
void typo_destroy(tl_typo *index) {
    if (index == NULL)
        return;
    vec_destroy(index->occurrences);
    free(index->terms);
    free(index->postings);
    free(index->keys);
    free(index);
}
static bool eligible(const uint32_t *symbols, size_t length) {
    if (length < TYPO_MIN_SYMBOLS || length > TYPO_MAX_SYMBOLS)
        return false;
    for (size_t i = 0; i < length; i++) {
        if (symbols[i] < '0' || symbols[i] > '9')
            return true;
    }
    return false; /* all digits */
}
tl_status typo_add(tl_typo *index, tl_text text, size_t slot) {
    if (index == NULL || (text.length != 0 && (text.symbols == NULL || text.boundaries == NULL)))
        return TL_INVALID;
    if (index->finished)
        return TL_STATE;
    if (slot > UINT32_MAX)
        return TL_LIMIT;
    for (size_t start = 0; start < text.length;) {
        if (tokenize_separator(text.symbols[start])) {
            start++;
            continue;
        }
        size_t end = start + 1;
        while (end < text.length && !tokenize_separator(text.symbols[end]) &&
               text.boundaries[end] == 0)
            end++;
        if (eligible(text.symbols + start, end - start)) {
            struct occurrence occurrence = {hash_symbols(text.symbols + start, end - start),
                                            text.symbols + start, (uint32_t)(end - start),
                                            (uint32_t)slot};
            tl_status status = vec_append(index->occurrences, &occurrence);
            if (status != TL_OK)
                return status;
        }
        start = end;
    }
    return TL_OK;
}
/* Group sorted occurrences into distinct terms with deduplicated postings. */
static tl_status build_terms(tl_typo *index) {
    size_t count = vec_count(index->occurrences);
    if (count > UINT32_MAX)
        return TL_LIMIT;
    struct occurrence *items = vec_data(index->occurrences);
    if (count > 1)
        qsort(items, count, sizeof(*items), compare_occurrences);
    index->terms = malloc((count == 0 ? 1 : count) * sizeof(struct term));
    index->postings = malloc((count == 0 ? 1 : count) * sizeof(uint32_t));
    if (index->terms == NULL || index->postings == NULL)
        return TL_NOMEM;
    index->term_count = index->posting_count = 0; /* only written entries are counted */
    for (size_t i = 0; i < count; i++) {
        bool new_term = i == 0 || items[i].hash != items[i - 1].hash ||
                        compare_symbols(items[i].symbols, items[i].length, items[i - 1].symbols,
                                        items[i - 1].length) != 0;
        if (new_term)
            index->terms[index->term_count++] =
                (struct term){items[i].symbols, items[i].length, (uint32_t)index->posting_count};
        else if (items[i].slot == items[i - 1].slot)
            continue; /* same token twice in one name */
        index->postings[index->posting_count++] = items[i].slot;
    }
    return TL_OK;
}
static tl_status build_keys(tl_typo *index) {
    size_t total = 0;
    for (size_t t = 0; t < index->term_count; t++)
        total += (size_t)index->terms[t].length + 1;
    if (index->term_count > UINT32_MAX)
        return TL_LIMIT;
    index->keys = malloc((total == 0 ? 1 : total) * sizeof(struct deletion_key));
    if (index->keys == NULL)
        return TL_NOMEM;
    for (size_t t = 0; t < index->term_count; t++) {
        const struct term *term = &index->terms[t];
        index->keys[index->key_count++] =
            (struct deletion_key){hash_symbols(term->symbols, term->length), (uint32_t)t};
        for (size_t skip = 0; skip < term->length; skip++)
            index->keys[index->key_count++] = (struct deletion_key){
                hash_deletion(term->symbols, term->length, skip), (uint32_t)t};
    }
    if (index->key_count > 1)
        qsort(index->keys, index->key_count, sizeof(struct deletion_key), compare_deletion_keys);
    size_t unique = 0;
    for (size_t i = 0; i < index->key_count; i++) {
        if (unique == 0 || index->keys[i].hash != index->keys[unique - 1].hash ||
            index->keys[i].term != index->keys[unique - 1].term)
            index->keys[unique++] = index->keys[i];
    }
    index->key_count = unique;
    return TL_OK;
}
/* Arrays were sized for the worst case (every occurrence distinct); give the
 * unused tail back. A failed shrink keeps the larger, still valid block. */
static void shrink_arrays(tl_typo *index) {
    struct term *terms =
        realloc(index->terms, (index->term_count == 0 ? 1 : index->term_count) * sizeof(*terms));
    if (terms != NULL)
        index->terms = terms;
    uint32_t *postings =
        realloc(index->postings,
                (index->posting_count == 0 ? 1 : index->posting_count) * sizeof(*postings));
    if (postings != NULL)
        index->postings = postings;
    struct deletion_key *keys =
        realloc(index->keys, (index->key_count == 0 ? 1 : index->key_count) * sizeof(*keys));
    if (keys != NULL)
        index->keys = keys;
}
tl_status typo_finish(tl_typo *index) {
    if (index == NULL)
        return TL_INVALID;
    if (index->finished)
        return TL_STATE;
    tl_status status = build_terms(index);
    if (status == TL_OK)
        status = build_keys(index);
    if (status == TL_OK)
        shrink_arrays(index);
    if (status != TL_OK)
        return status;
    vec_destroy(index->occurrences);
    index->occurrences = NULL;
    index->finished = true;
    return TL_OK;
}
tl_status typo_scratch_create(const tl_typo *index, tl_typo_scratch **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (index == NULL)
        return TL_INVALID;
    if (!index->finished)
        return TL_STATE;
    tl_typo_scratch *scratch = calloc(1, sizeof(*scratch));
    if (scratch == NULL)
        return TL_NOMEM;
    scratch->stamps = calloc(index->term_count == 0 ? 1 : index->term_count, sizeof(uint32_t));
    if (scratch->stamps == NULL) {
        free(scratch);
        return TL_NOMEM;
    }
    scratch->index = index;
    *out = scratch;
    return TL_OK;
}
void typo_scratch_destroy(tl_typo_scratch *scratch) {
    if (scratch == NULL)
        return;
    free(scratch->stamps);
    free(scratch);
}
/* Verify each unseen term under key hash and report its slots when the term is
 * exactly one edit away. */
static tl_status probe(const tl_typo *index, tl_typo_scratch *scratch, uint64_t hash, tl_text query,
                       tl_typo_hit hit, void *context) {
    size_t lo = 0, hi = index->key_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (index->keys[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t k = lo; k < index->key_count && index->keys[k].hash == hash; k++) {
        uint32_t t = index->keys[k].term;
        if (scratch->stamps[t] == scratch->epoch)
            continue;
        scratch->stamps[t] = scratch->epoch;
        const struct term *term = &index->terms[t];
        tl_text candidate = {.symbols = term->symbols, .length = term->length};
        size_t distance = 0;
        tl_status status = fuzzy_edit_distance(candidate, query, &distance);
        if (status != TL_OK)
            return status;
        if (distance != 1)
            continue;
        size_t end =
            t + 1 < index->term_count ? index->terms[t + 1].postings_start : index->posting_count;
        for (size_t p = term->postings_start; p < end; p++) {
            status = hit(context, index->postings[p]);
            if (status != TL_OK)
                return status;
        }
    }
    return TL_OK;
}
static void next_epoch(const tl_typo *index, tl_typo_scratch *scratch) {
    if (++scratch->epoch == 0) {
        /* Wrapped: clear stamps so no stale stamp equals a new epoch. */
        memset(scratch->stamps, 0, index->term_count * sizeof(uint32_t));
        scratch->epoch = 1;
    }
}
/* Lexicographically sorted distinct terms form an implicit trie: each child
 * is a contiguous range, found by binary search without storing trie nodes.
 * A bounded OSA row prunes branches requiring more than one edit. */
struct prefix_search {
    const tl_typo *index;
    tl_typo_scratch *scratch;
    tl_text query;
    tl_typo_hit hit;
    void *context;
    uint8_t rows[TYPO_MAX_SYMBOLS + 2][TYPO_MAX_SYMBOLS + 1];
    uint32_t symbols[TYPO_MAX_SYMBOLS + 1];
};
static tl_status report_prefix(struct prefix_search *search, size_t start, size_t end) {
    for (size_t t = start; t < end; t++) {
        const struct term *term = &search->index->terms[t];
        /* An exact prefix belongs to the stronger prefix channel. */
        if (search->scratch->stamps[t] == search->scratch->epoch ||
            (term->length >= search->query.length &&
             memcmp(term->symbols, search->query.symbols,
                    search->query.length * sizeof(uint32_t)) == 0))
            continue;
        search->scratch->stamps[t] = search->scratch->epoch;
        size_t stop = t + 1 < search->index->term_count ? search->index->terms[t + 1].postings_start
                                                        : search->index->posting_count;
        for (size_t p = term->postings_start; p < stop; p++) {
            tl_status status = search->hit(search->context, search->index->postings[p]);
            if (status != TL_OK)
                return status;
        }
    }
    return TL_OK;
}
static unsigned minimum(unsigned a, unsigned b) {
    return a < b ? a : b;
}
static unsigned prefix_row(struct prefix_search *search, size_t depth, uint32_t symbol) {
    uint8_t *row = search->rows[depth], *previous = search->rows[depth - 1];
    row[0] = (uint8_t)minimum((unsigned)depth, 2);
    unsigned best = row[0];
    for (size_t q = 1; q <= search->query.length; q++) {
        unsigned value = minimum((unsigned)previous[q] + 1, (unsigned)row[q - 1] + 1);
        value =
            minimum(value, (unsigned)previous[q - 1] + (symbol != search->query.symbols[q - 1]));
        if (depth > 1 && q > 1 && symbol == search->query.symbols[q - 2] &&
            search->symbols[depth - 2] == search->query.symbols[q - 1])
            value = minimum(value, (unsigned)search->rows[depth - 2][q - 2] + 1);
        row[q] = (uint8_t)minimum(value, 2);
        best = minimum(best, row[q]);
    }
    return best;
}
static size_t child_end(const tl_typo *index, size_t start, size_t end, size_t depth) {
    uint32_t symbol = index->terms[start].symbols[depth];
    size_t low = start + 1, high = end;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (index->terms[mid].symbols[depth] == symbol)
            low = mid + 1;
        else
            high = mid;
    }
    return low;
}
static tl_status visit_prefix(struct prefix_search *search, size_t start, size_t end,
                              size_t depth) {
    if (depth != 0 && search->rows[depth][search->query.length] <= 1)
        return report_prefix(search, start, end);
    if (depth >= search->query.length + 1)
        return TL_OK;
    while (start < end && search->index->terms[start].length == depth)
        start++;
    while (start < end) {
        size_t stop = child_end(search->index, start, end, depth);
        uint32_t symbol = search->index->terms[start].symbols[depth];
        search->symbols[depth] = symbol;
        if (prefix_row(search, depth + 1, symbol) <= 1) {
            tl_status status = visit_prefix(search, start, stop, depth + 1);
            if (status != TL_OK)
                return status;
        }
        start = stop;
    }
    return TL_OK;
}
tl_status typo_query(const tl_typo *index, tl_typo_scratch *scratch, tl_text query, tl_typo_hit hit,
                     void *context) {
    if (index == NULL || scratch == NULL || scratch->index != index || hit == NULL ||
        (query.symbols == NULL && query.length != 0))
        return TL_INVALID;
    if (!index->finished)
        return TL_STATE;
    /* Short fragments have too many neighbours; use prefix/subsequence only.
     * The query budget is independent of an indexed neighbour's length. */
    if (!eligible(query.symbols, query.length))
        return TL_OK;
    next_epoch(index, scratch);
    tl_status status = TL_OK;
    if (query.length >= TYPO_PREFIX_MIN_SYMBOLS && eligible(query.symbols, query.length)) {
        struct prefix_search search = {
            .index = index, .scratch = scratch, .query = query, .hit = hit, .context = context};
        for (size_t q = 0; q <= query.length; q++)
            search.rows[0][q] = (uint8_t)minimum((unsigned)q, 2);
        status = visit_prefix(&search, 0, index->term_count, 0);
    }
    if (status == TL_OK)
        status =
            probe(index, scratch, hash_symbols(query.symbols, query.length), query, hit, context);
    for (size_t skip = 0; skip < query.length && status == TL_OK; skip++)
        status = probe(index, scratch, hash_deletion(query.symbols, query.length, skip), query, hit,
                       context);
    return status;
}
