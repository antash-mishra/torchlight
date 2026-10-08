/* Capped decayed open counts and sorted query-to-open pairs (see usage.h).
 * Items live in a dense array; evicting one moves the last item into its
 * index and renumbers that item's pairs, so indices change only together
 * with the version. Pairs stay sorted by normalized query (ties in any
 * order), so the stored queries starting with a typed query form one range
 * found by binary search. Weights decay exponentially, so a weight measured
 * at one time can be added to another by decaying it to the later time. */
#include "torchlight/usage.h"
#include "torchlight/lexical.h"
#include "torchlight/tokenize.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Frecency boosts are recomputed at most this often; a minute changes decayed
 * weights negligibly, and changes force a refresh anyway. */
#define USAGE_REFRESH_SECONDS 60
/* Weight at which a boost reaches half its maximum: about two recent opens
 * for frecency, one recent open from a matching query for query history. */
#define USAGE_FRECENCY_HALF_WEIGHT 2.0
#define USAGE_QUERY_HALF_WEIGHT 1.0
/* Case folding can expand a query of LEXICAL_QUERY_BYTES bytes. */
#define USAGE_SCRATCH_SYMBOLS (LEXICAL_QUERY_BYTES * 4)
_Static_assert(USAGE_FRECENCY_BOOST_MAX + USAGE_QUERY_BOOST_MAX <= LEXICAL_BOOST_MAX,
               "usage boosts fit the lexical boost cap");
_Static_assert(USAGE_MAX_ITEMS <= LEXICAL_MAX_BOOSTED, "every item can be boosted");

/* A decayed open count as measured at time at. */
struct weight {
    double value;
    int64_t at;
};
struct item {
    uint64_t file_id;
    char desktop_id[USAGE_DESKTOP_ID_BYTES];
    struct weight weight;
    int64_t last_open;
};
struct pair {
    uint32_t symbols[USAGE_QUERY_SYMBOLS];
    size_t length, item;
    struct weight weight;
    int64_t last_open;
};
struct tl_usage {
    struct item items[USAGE_MAX_ITEMS];
    struct pair pairs[USAGE_MAX_PAIRS];
    size_t item_count, pair_count;
    /* Cached frecency boosts, the per-query output, and per-query sums of
     * matching pair weights with marks for the items they touched. */
    int frecency[USAGE_MAX_ITEMS], boosts[USAGE_MAX_ITEMS];
    double sums[USAGE_MAX_ITEMS];
    bool marked[USAGE_MAX_ITEMS];
    size_t touched[USAGE_MAX_ITEMS];
    int64_t retention, refreshed;
    bool stale;
    uint64_t version;
    uint32_t symbols[USAGE_SCRATCH_SYMBOLS];
    uint8_t boundaries[USAGE_SCRATCH_SYMBOLS];
    size_t offsets[USAGE_SCRATCH_SYMBOLS];
};

tl_status usage_create(int64_t retention_seconds, tl_usage **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (retention_seconds <= 0)
        return TL_INVALID;
    tl_usage *usage = calloc(1, sizeof(*usage));
    if (usage == NULL)
        return TL_NOMEM;
    usage->retention = retention_seconds;
    usage->stale = true;
    usage->version = 1;
    *out = usage;
    return TL_OK;
}
void usage_destroy(tl_usage *usage) {
    free(usage);
}
/* ---- weights ------------------------------------------------------------------ */
static double decayed(struct weight weight, int64_t now) {
    if (now <= weight.at)
        return weight.value;
    return weight.value * exp2(-(double)(now - weight.at) / USAGE_HALF_LIFE_SECONDS);
}
/* Add a weight measured at another time, keeping the later reference time. */
static void add_weight(struct weight *into, struct weight added) {
    if (added.at >= into->at) {
        into->value = decayed(*into, added.at) + added.value;
        into->at = added.at;
    } else {
        into->value += decayed(added, into->at);
    }
}
/* Saturating map of a weight to 0..maximum, half of it at half_weight. */
static int boost_from(double value, int maximum, double half_weight) {
    if (value <= 0)
        return 0;
    return (int)lround(maximum * value / (value + half_weight));
}
/* ---- query normalization ------------------------------------------------------ */
static bool whitespace(uint32_t symbol) {
    return symbol == ' ' || symbol == '\t' || symbol == '\n' || symbol == '\r';
}
/* Normalize like the lexical engine (NFC, case folding, opaque invalid
 * bytes), trim and collapse whitespace, keeping USAGE_QUERY_SYMBOLS symbols.
 * *complete reports whether the whole query fit. False when the query is too
 * long or cannot be tokenized. */
static bool normalize(tl_usage *usage, const char *query, uint32_t *out, size_t *length,
                      bool *complete) {
    *length = 0;
    *complete = true;
    size_t bytes = strnlen(query, LEXICAL_QUERY_BYTES + 1);
    tl_text text = {0};
    if (bytes > LEXICAL_QUERY_BYTES ||
        tokenize_into(query, bytes, usage->symbols, usage->boundaries, usage->offsets,
                      USAGE_SCRATCH_SYMBOLS, &text) != TL_OK)
        return false;
    bool separate = false;
    for (size_t i = 0; i < text.length; i++) {
        uint32_t symbol = text.symbols[i];
        if (whitespace(symbol)) {
            separate = *length != 0;
            continue;
        }
        size_t needed = separate ? 2 : 1;
        if (*length + needed > USAGE_QUERY_SYMBOLS) {
            *complete = false;
            break;
        }
        if (separate)
            out[(*length)++] = ' ';
        out[(*length)++] = symbol;
        separate = false;
    }
    return true;
}
/* ---- items -------------------------------------------------------------------- */
static bool valid_target(tl_usage_target target) {
    if (target.file_id != 0)
        return target.desktop_id == NULL;
    return target.desktop_id != NULL && target.desktop_id[0] != 0;
}
static bool same_target(const struct item *item, tl_usage_target target) {
    if (target.file_id != 0)
        return item->file_id == target.file_id;
    return item->file_id == 0 && target.desktop_id != NULL &&
           strcmp(item->desktop_id, target.desktop_id) == 0;
}
static size_t find_item(const tl_usage *usage, tl_usage_target target) {
    for (size_t i = 0; i < usage->item_count; i++)
        if (same_target(&usage->items[i], target))
            return i;
    return SIZE_MAX;
}
/* Drop item index and its pairs; the last item moves into index. */
static void remove_item(tl_usage *usage, size_t index) {
    size_t last = usage->item_count - 1, kept = 0;
    for (size_t p = 0; p < usage->pair_count; p++) {
        struct pair *pair = &usage->pairs[p];
        if (pair->item == index)
            continue;
        if (pair->item == last)
            pair->item = index;
        if (kept != p)
            usage->pairs[kept] = *pair;
        kept++;
    }
    usage->pair_count = kept;
    usage->items[index] = usage->items[last];
    usage->item_count--;
    usage->version++;
    usage->stale = true;
}
static size_t weakest_item(const tl_usage *usage, int64_t now) {
    size_t weakest = 0;
    for (size_t i = 1; i < usage->item_count; i++)
        if (decayed(usage->items[i].weight, now) < decayed(usage->items[weakest].weight, now))
            weakest = i;
    return weakest;
}
/* Add weight to target's item, creating it (evicting the weakest when full). */
static size_t add_to_item(tl_usage *usage, tl_usage_target target, struct weight weight,
                          int64_t last_open) {
    size_t index = find_item(usage, target);
    if (index == SIZE_MAX) {
        if (usage->item_count == USAGE_MAX_ITEMS)
            remove_item(usage, weakest_item(usage, last_open));
        index = usage->item_count++;
        struct item *item = &usage->items[index];
        *item = (struct item){.file_id = target.file_id, .last_open = last_open};
        if (target.file_id == 0)
            memcpy(item->desktop_id, target.desktop_id, strlen(target.desktop_id) + 1);
        usage->version++;
    }
    struct item *item = &usage->items[index];
    add_weight(&item->weight, weight);
    if (last_open > item->last_open)
        item->last_open = last_open;
    usage->stale = true;
    return index;
}
/* ---- pairs -------------------------------------------------------------------- */
/* Lexicographic order of a pair's query against symbols[0..length). */
static int compare_query(const struct pair *pair, const uint32_t *symbols, size_t length) {
    size_t shared = pair->length < length ? pair->length : length;
    for (size_t i = 0; i < shared; i++)
        if (pair->symbols[i] != symbols[i])
            return pair->symbols[i] < symbols[i] ? -1 : 1;
    return pair->length < length ? -1 : pair->length > length ? 1 : 0;
}
/* First pair whose query is not below symbols; prefixed queries follow it. */
static size_t lower_bound(const tl_usage *usage, const uint32_t *symbols, size_t length) {
    size_t low = 0, high = usage->pair_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (compare_query(&usage->pairs[middle], symbols, length) < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}
static bool starts_with(const struct pair *pair, const uint32_t *symbols, size_t length) {
    return pair->length >= length && memcmp(pair->symbols, symbols, length * sizeof(uint32_t)) == 0;
}
static void remove_weakest_pair(tl_usage *usage, int64_t now) {
    size_t weakest = 0;
    for (size_t p = 1; p < usage->pair_count; p++)
        if (decayed(usage->pairs[p].weight, now) < decayed(usage->pairs[weakest].weight, now))
            weakest = p;
    memmove(&usage->pairs[weakest], &usage->pairs[weakest + 1],
            (usage->pair_count - weakest - 1) * sizeof(struct pair));
    usage->pair_count--;
}
/* Add weight to the (query, item) pair, inserting it in order when new. */
static void add_to_pair(tl_usage *usage, const uint32_t *symbols, size_t length, size_t item,
                        struct weight weight, int64_t last_open) {
    size_t position = lower_bound(usage, symbols, length);
    for (size_t p = position;
         p < usage->pair_count && compare_query(&usage->pairs[p], symbols, length) == 0; p++) {
        if (usage->pairs[p].item != item)
            continue;
        add_weight(&usage->pairs[p].weight, weight);
        if (last_open > usage->pairs[p].last_open)
            usage->pairs[p].last_open = last_open;
        return;
    }
    if (usage->pair_count == USAGE_MAX_PAIRS) {
        remove_weakest_pair(usage, last_open);
        position = lower_bound(usage, symbols, length);
    }
    memmove(&usage->pairs[position + 1], &usage->pairs[position],
            (usage->pair_count - position) * sizeof(struct pair));
    struct pair *pair = &usage->pairs[position];
    *pair = (struct pair){.length = length, .item = item, .weight = weight, .last_open = last_open};
    memcpy(pair->symbols, symbols, length * sizeof(uint32_t));
    usage->pair_count++;
}
/* ---- public operations ---------------------------------------------------------- */
tl_status usage_record(tl_usage *usage, tl_usage_target target, const char *query,
                       int64_t timestamp) {
    if (usage == NULL || !valid_target(target) || timestamp < 0)
        return TL_INVALID;
    if (target.file_id == 0 &&
        strnlen(target.desktop_id, USAGE_DESKTOP_ID_BYTES) == USAGE_DESKTOP_ID_BYTES)
        return TL_LIMIT;
    struct weight open = {1.0, timestamp};
    size_t item = add_to_item(usage, target, open, timestamp);
    uint32_t symbols[USAGE_QUERY_SYMBOLS];
    size_t length = 0;
    bool complete = true;
    /* A query too long to keep whole is stored by its leading symbols: every
     * typed prefix short enough to match still finds it. */
    if (query != NULL && normalize(usage, query, symbols, &length, &complete) && length != 0)
        add_to_pair(usage, symbols, length, item, open, timestamp);
    return TL_OK;
}
void usage_clear(tl_usage *usage) {
    if (usage == NULL)
        return;
    usage->item_count = usage->pair_count = 0;
    usage->version++;
    usage->stale = true;
}
tl_status usage_merge(tl_usage *into, const tl_usage *from) {
    if (into == NULL || from == NULL || into == from)
        return TL_INVALID;
    for (size_t i = 0; i < from->item_count; i++) {
        const struct item *item = &from->items[i];
        add_to_item(into, usage_target(from, i), item->weight, item->last_open);
    }
    /* Items are found again by key: a full table may have evicted some. */
    for (size_t p = 0; p < from->pair_count; p++) {
        const struct pair *pair = &from->pairs[p];
        size_t item = find_item(into, usage_target(from, pair->item));
        if (item != SIZE_MAX)
            add_to_pair(into, pair->symbols, pair->length, item, pair->weight, pair->last_open);
    }
    into->version++;
    into->stale = true;
    return TL_OK;
}
uint64_t usage_version(const tl_usage *usage) {
    return usage == NULL ? 0 : usage->version;
}
size_t usage_count(const tl_usage *usage) {
    return usage == NULL ? 0 : usage->item_count;
}
tl_usage_target usage_target(const tl_usage *usage, size_t index) {
    if (usage == NULL || index >= usage->item_count)
        return (tl_usage_target){0};
    const struct item *item = &usage->items[index];
    return (tl_usage_target){item->file_id, item->file_id == 0 ? item->desktop_id : NULL};
}
/* Evict entries past retention and recompute frecency boosts, unless the
 * cached ones are recent and nothing changed. */
static void refresh(tl_usage *usage, int64_t now) {
    if (!usage->stale && now >= usage->refreshed && now - usage->refreshed < USAGE_REFRESH_SECONDS)
        return;
    int64_t cutoff = now - usage->retention;
    size_t kept = 0;
    for (size_t p = 0; p < usage->pair_count; p++)
        if (usage->pairs[p].last_open >= cutoff)
            usage->pairs[kept++] = usage->pairs[p];
    usage->pair_count = kept;
    /* Downward, so the item remove_item moves in was already checked. */
    for (size_t i = usage->item_count; i-- > 0;)
        if (usage->items[i].last_open < cutoff)
            remove_item(usage, i);
    for (size_t i = 0; i < usage->item_count; i++)
        usage->frecency[i] = boost_from(decayed(usage->items[i].weight, now),
                                        USAGE_FRECENCY_BOOST_MAX, USAGE_FRECENCY_HALF_WEIGHT);
    usage->refreshed = now;
    usage->stale = false;
}
/* Add query-to-open boosts from stored queries starting with query. */
static void add_query_boosts(tl_usage *usage, const char *query, int64_t now) {
    uint32_t symbols[USAGE_QUERY_SYMBOLS];
    size_t length = 0, touched = 0;
    bool complete = true;
    if (!normalize(usage, query, symbols, &length, &complete) || !complete || length == 0)
        return;
    int64_t cutoff = now - usage->retention;
    for (size_t p = lower_bound(usage, symbols, length);
         p < usage->pair_count && starts_with(&usage->pairs[p], symbols, length); p++) {
        const struct pair *pair = &usage->pairs[p];
        if (pair->last_open < cutoff)
            continue;
        if (!usage->marked[pair->item]) {
            usage->marked[pair->item] = true;
            usage->touched[touched++] = pair->item;
        }
        usage->sums[pair->item] += decayed(pair->weight, now);
    }
    for (size_t t = 0; t < touched; t++) {
        size_t item = usage->touched[t];
        usage->boosts[item] +=
            boost_from(usage->sums[item], USAGE_QUERY_BOOST_MAX, USAGE_QUERY_HALF_WEIGHT);
        usage->sums[item] = 0;
        usage->marked[item] = false;
    }
}
tl_status usage_boosts(tl_usage *usage, const char *query, int64_t now, const int **boosts) {
    if (usage == NULL || query == NULL || boosts == NULL)
        return TL_INVALID;
    refresh(usage, now);
    memcpy(usage->boosts, usage->frecency, usage->item_count * sizeof(int));
    add_query_boosts(usage, query, now);
    *boosts = usage->boosts;
    return TL_OK;
}
