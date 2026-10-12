/* Persisted-state map: committed rows as path fingerprints and signatures in
 * id order, a hash index from path fingerprint to record, and per-scan seen
 * marks, so scans compare against the catalog without SQL. */
#include "torchlight/ledger.h"
#include "torchlight/hashmap.h"
#include <stdlib.h>
#include <string.h>
#define LEDGER_WORD_BITS 64U
/* Compaction rewrites every record and the index, so it waits until removed
 * records are both numerous and at least half of the storage. */
#define LEDGER_COMPACT_MIN 1024U
/* One committed row. key indexes it in paths; check confirms a key match. */
struct record {
    uint64_t id, key, check, signature;
};
/* records ascend by id. dead and seen hold one bit per record: removed rows
 * stay in place (so ids stay sorted) until compaction drops them. paths is an
 * external-hash map (4 bytes per slot) whose hashes are the records' keys.
 * indexed: paths lists every live record. A refill (ledger_reset until
 * ledger_seal) only appends records, and the seal indexes them all at once at
 * their final size instead of growing the index row by row. */
struct tl_ledger {
    tl_vec *records;     /* struct record */
    tl_vec *dead, *seen; /* uint64_t words */
    tl_hashmap *paths;
    size_t live, dead_count;
    bool complete, failed, indexed;
};
struct path_key {
    const tl_ledger *ledger;
    uint64_t check;
    uint32_t index; /* for removal: the one record to match */
};
static const struct record *record_at(const tl_ledger *ledger, size_t index) {
    return (const struct record *)vec_const_data(ledger->records) + index;
}
static uint64_t record_key(const void *context, uint32_t value) {
    return record_at(context, value)->key;
}
static bool check_equal(const void *context, uint32_t value) {
    const struct path_key *key = context;
    return record_at(key->ledger, value)->check == key->check;
}
static bool index_equal(const void *context, uint32_t value) {
    return ((const struct path_key *)context)->index == value;
}
static bool bit(const tl_vec *words, size_t index) {
    const uint64_t *data = vec_const_data(words);
    return (data[index / LEDGER_WORD_BITS] >> (index % LEDGER_WORD_BITS) & 1U) != 0;
}
static void set_bit(tl_vec *words, size_t index, bool value) {
    uint64_t *word = (uint64_t *)vec_data(words) + index / LEDGER_WORD_BITS;
    uint64_t mask = UINT64_C(1) << (index % LEDGER_WORD_BITS);
    *word = value ? *word | mask : *word & ~mask;
}
/* Grow both bit vectors (with clear bits) to cover count records. */
static tl_status cover_bits(tl_ledger *ledger, size_t count) {
    size_t words = (count + LEDGER_WORD_BITS - 1) / LEDGER_WORD_BITS;
    const uint64_t zero = 0;
    tl_status status = TL_OK;
    while (status == TL_OK && vec_count(ledger->dead) < words)
        status = vec_append(ledger->dead, &zero);
    while (status == TL_OK && vec_count(ledger->seen) < words)
        status = vec_append(ledger->seen, &zero);
    return status;
}
/* One pass over the path yields both halves of a 128-bit hash: the index key
 * and an independent check. A collision would need both to match. */
static void fingerprint(const char *path, uint64_t *key, uint64_t *check) {
    tl_hash_pair pair = hashmap_hash_pair(path, strlen(path));
    *key = pair.first;
    *check = pair.second;
}
/* Binary search for id among records; true and *index on a hit. */
static bool find_id(const tl_ledger *ledger, uint64_t id, size_t *index) {
    size_t low = 0, high = vec_count(ledger->records);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (record_at(ledger, middle)->id < id)
            low = middle + 1;
        else
            high = middle;
    }
    if (low == vec_count(ledger->records) || record_at(ledger, low)->id != id)
        return false;
    *index = low;
    return true;
}
static tl_status new_index(const tl_ledger *ledger, tl_hashmap **out) {
    return hashmap_create_external(record_key, ledger, out);
}
tl_status ledger_create(tl_ledger **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_ledger *ledger = calloc(1, sizeof(*ledger));
    if (ledger == NULL)
        return TL_NOMEM;
    tl_status status = vec_create(sizeof(struct record), &ledger->records);
    if (status == TL_OK)
        status = vec_create(sizeof(uint64_t), &ledger->dead);
    if (status == TL_OK)
        status = vec_create(sizeof(uint64_t), &ledger->seen);
    if (status == TL_OK)
        status = new_index(ledger, &ledger->paths);
    if (status != TL_OK) {
        ledger_destroy(ledger);
        return status;
    }
    *out = ledger;
    return TL_OK;
}
void ledger_destroy(tl_ledger *ledger) {
    if (ledger == NULL)
        return;
    vec_destroy(ledger->records);
    vec_destroy(ledger->dead);
    vec_destroy(ledger->seen);
    hashmap_destroy(ledger->paths);
    free(ledger);
}
/* A failure leaves records or the index out of step with the catalog. */
static tl_status fail(tl_ledger *ledger, tl_status status) {
    ledger->failed = true;
    ledger->complete = false;
    return status;
}
/* Replace the index with one listing every live record, sized for all of
 * them. On failure the old index and the indexed flag stay as they were. */
static tl_status index_records(tl_ledger *ledger) {
    tl_hashmap *paths = NULL;
    tl_status status = new_index(ledger, &paths);
    if (status == TL_OK)
        status = hashmap_reserve(paths, ledger->live);
    for (size_t i = 0; i < vec_count(ledger->records) && status == TL_OK; i++)
        if (!bit(ledger->dead, i))
            status = hashmap_insert(paths, record_at(ledger, i)->key, (uint32_t)i);
    if (status != TL_OK) {
        hashmap_destroy(paths);
        return status;
    }
    hashmap_destroy(ledger->paths);
    ledger->paths = paths;
    ledger->indexed = true;
    return TL_OK;
}
void ledger_reset(tl_ledger *ledger) {
    if (ledger == NULL)
        return;
    tl_hashmap *empty = NULL;
    if (new_index(ledger, &empty) != TL_OK) {
        /* Records and index stay consistent with each other, but no longer
         * describe the catalog the caller is about to load. */
        fail(ledger, TL_NOMEM);
        return;
    }
    hashmap_destroy(ledger->paths);
    ledger->paths = empty;
    vec_clear(ledger->records);
    vec_clear(ledger->dead);
    vec_clear(ledger->seen);
    ledger->live = ledger->dead_count = 0;
    ledger->complete = ledger->failed = ledger->indexed = false;
}
static void unlist(tl_ledger *ledger, size_t index) {
    if (!ledger->indexed)
        return;
    struct path_key match = {ledger, 0, (uint32_t)index};
    bool removed =
        hashmap_remove(ledger->paths, record_at(ledger, index)->key, index_equal, &match);
    (void)removed; /* every live record is indexed */
}
static tl_status list(tl_ledger *ledger, size_t index) {
    return ledger->indexed
               ? hashmap_insert(ledger->paths, record_at(ledger, index)->key, (uint32_t)index)
               : TL_OK;
}
/* Point an existing record (live or dead) at a new path and signature. */
static tl_status replace(tl_ledger *ledger, size_t index, uint64_t key, uint64_t check,
                         uint64_t signature) {
    if (bit(ledger->dead, index)) {
        set_bit(ledger->dead, index, false);
        ledger->dead_count--;
        ledger->live++;
    } else {
        unlist(ledger, index);
    }
    struct record *record = (struct record *)vec_data(ledger->records) + index;
    record->key = key;
    record->check = check;
    record->signature = signature;
    return list(ledger, index);
}
static tl_status append(tl_ledger *ledger, uint64_t id, uint64_t key, uint64_t check,
                        uint64_t signature) {
    size_t index = vec_count(ledger->records);
    if (index >= UINT32_MAX)
        return TL_LIMIT; /* the index stores uint32 positions */
    struct record record = {id, key, check, signature};
    tl_status status = cover_bits(ledger, index + 1);
    if (status == TL_OK)
        status = vec_append(ledger->records, &record);
    if (status == TL_OK)
        status = list(ledger, index);
    if (status == TL_OK)
        ledger->live++;
    else if (vec_count(ledger->records) > index)
        vec_truncate(ledger->records, index);
    return status;
}
tl_status ledger_put(tl_ledger *ledger, uint64_t id, const char *path, uint64_t signature) {
    if (ledger == NULL || path == NULL || id == 0)
        return ledger == NULL ? TL_INVALID : fail(ledger, TL_INVALID);
    uint64_t key = 0, check = 0;
    fingerprint(path, &key, &check);
    size_t index = 0, count = vec_count(ledger->records);
    tl_status status = TL_OK;
    if (count != 0 && record_at(ledger, count - 1)->id < id)
        status = append(ledger, id, key, check, signature); /* the common case: a load */
    else if (find_id(ledger, id, &index))
        status = replace(ledger, index, key, check, signature);
    else if (count == 0)
        status = append(ledger, id, key, check, signature);
    else
        status = TL_STATE; /* would break id order: ids only grow */
    return status == TL_OK ? TL_OK : fail(ledger, status);
}
/* Drop removed records, then index the survivors again. */
static void compact(tl_ledger *ledger) {
    struct record *records = vec_data(ledger->records);
    size_t kept = 0;
    for (size_t i = 0; i < vec_count(ledger->records); i++)
        if (!bit(ledger->dead, i))
            records[kept++] = records[i];
    vec_truncate(ledger->records, kept);
    vec_clear(ledger->dead);
    vec_clear(ledger->seen);
    ledger->dead_count = 0;
    tl_status status = cover_bits(ledger, kept);
    if (status == TL_OK && ledger->indexed)
        status = index_records(ledger);
    if (status != TL_OK) {
        ledger->indexed = false; /* the old index points at moved records */
        fail(ledger, status);
    }
}
void ledger_remove(tl_ledger *ledger, uint64_t id) {
    size_t index = 0;
    if (ledger == NULL || !find_id(ledger, id, &index) || bit(ledger->dead, index))
        return;
    unlist(ledger, index);
    set_bit(ledger->dead, index, true);
    ledger->dead_count++;
    ledger->live--;
    if (ledger->dead_count >= LEDGER_COMPACT_MIN &&
        ledger->dead_count * 2 >= vec_count(ledger->records))
        compact(ledger);
}
void ledger_seal(tl_ledger *ledger) {
    if (ledger == NULL)
        return;
    vec_shrink(ledger->records);
    if (!ledger->failed && !ledger->indexed && index_records(ledger) != TL_OK)
        fail(ledger, TL_NOMEM);
    ledger->complete = !ledger->failed;
}
void ledger_invalidate(tl_ledger *ledger) {
    if (ledger != NULL)
        fail(ledger, TL_STATE);
}
bool ledger_complete(const tl_ledger *ledger) {
    return ledger != NULL && ledger->complete;
}
tl_status ledger_begin(tl_ledger *ledger) {
    if (ledger == NULL)
        return TL_INVALID;
    tl_status status = cover_bits(ledger, vec_count(ledger->records));
    if (status != TL_OK)
        return status;
    if (vec_count(ledger->seen) != 0)
        memset(vec_data(ledger->seen), 0, vec_count(ledger->seen) * sizeof(uint64_t));
    return TL_OK;
}
bool ledger_find(tl_ledger *ledger, const char *path, uint64_t *id, uint64_t *signature) {
    if (ledger == NULL || path == NULL || id == NULL || signature == NULL)
        return false;
    /* Lookups before the seal of a refill index what was appended so far. */
    if (!ledger->indexed && index_records(ledger) != TL_OK)
        return false;
    struct path_key key = {ledger, 0, 0};
    uint64_t hash = 0;
    fingerprint(path, &hash, &key.check);
    uint32_t index = 0;
    if (!hashmap_find(ledger->paths, hash, check_equal, &key, &index))
        return false;
    set_bit(ledger->seen, index, true);
    *id = record_at(ledger, index)->id;
    *signature = record_at(ledger, index)->signature;
    return true;
}
tl_status ledger_unseen(const tl_ledger *ledger, tl_vec *ids) {
    if (ledger == NULL || ids == NULL)
        return TL_INVALID;
    tl_status status = TL_OK;
    for (size_t i = 0; i < vec_count(ledger->records) && status == TL_OK; i++)
        if (!bit(ledger->dead, i) && !bit(ledger->seen, i))
            status = vec_append(ids, &record_at(ledger, i)->id);
    return status;
}
bool ledger_contains(const tl_ledger *ledger, uint64_t id) {
    size_t index = 0;
    return ledger != NULL && find_id(ledger, id, &index) && !bit(ledger->dead, index);
}
size_t ledger_count(const tl_ledger *ledger) {
    return ledger == NULL ? 0 : ledger->live;
}
