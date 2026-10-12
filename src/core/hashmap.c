/* Linear-probing hash map storing hashes and uint32 values; keys stay external.
 * An external-hash map stores values only and asks its caller for their hashes. */
#include "torchlight/hashmap.h"
#include <stdlib.h>
#include <string.h>
enum {
    HASHMAP_INITIAL_CAPACITY = 64,
    /* Grow when more than half full: linear probing degrades quickly above that. */
    HASHMAP_MAX_LOAD_PERCENT = 50
};
static const uint32_t HASHMAP_EMPTY = UINT32_MAX;
/* hashes is NULL for an external-hash map: hash_of supplies them instead. */
struct tl_hashmap {
    uint64_t *hashes;
    uint32_t *values;
    size_t count, capacity;
    tl_hashmap_hash_of hash_of;
    const void *hash_context;
};
/* FNV-1a has weak low bits; mix before masking so similar keys spread out. */
static size_t bucket(uint64_t hash, size_t capacity) {
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    return (size_t)hash & (capacity - 1);
}
static uint64_t stored_hash(const tl_hashmap *map, size_t slot) {
    return map->hashes != NULL ? map->hashes[slot]
                               : map->hash_of(map->hash_context, map->values[slot]);
}
static tl_status allocate(size_t capacity, bool external, uint64_t **hashes, uint32_t **values) {
    *hashes = external ? NULL : malloc(capacity * sizeof(**hashes));
    *values = malloc(capacity * sizeof(**values));
    if ((!external && *hashes == NULL) || *values == NULL) {
        free(*hashes);
        free(*values);
        return TL_NOMEM;
    }
    for (size_t i = 0; i < capacity; i++)
        (*values)[i] = HASHMAP_EMPTY;
    return TL_OK;
}
static tl_status create(tl_hashmap_hash_of hash_of, const void *context, tl_hashmap **out) {
    *out = NULL;
    tl_hashmap *map = calloc(1, sizeof(*map));
    if (map == NULL)
        return TL_NOMEM;
    map->hash_of = hash_of;
    map->hash_context = context;
    if (allocate(HASHMAP_INITIAL_CAPACITY, hash_of != NULL, &map->hashes, &map->values) != TL_OK) {
        free(map);
        return TL_NOMEM;
    }
    map->capacity = HASHMAP_INITIAL_CAPACITY;
    *out = map;
    return TL_OK;
}
tl_status hashmap_create(tl_hashmap **out) {
    return out == NULL ? TL_INVALID : create(NULL, NULL, out);
}
tl_status hashmap_create_external(tl_hashmap_hash_of hash_of, const void *context,
                                  tl_hashmap **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    return hash_of == NULL ? TL_INVALID : create(hash_of, context, out);
}
void hashmap_destroy(tl_hashmap *map) {
    if (map == NULL)
        return;
    free(map->hashes);
    free(map->values);
    free(map);
}
static void place(uint64_t *hashes, uint32_t *values, size_t capacity, uint64_t hash,
                  uint32_t value) {
    size_t slot = bucket(hash, capacity);
    while (values[slot] != HASHMAP_EMPTY)
        slot = (slot + 1) & (capacity - 1);
    if (hashes != NULL)
        hashes[slot] = hash;
    values[slot] = value;
}
/* Rehash every value into a table of capacity slots (a power of two). */
static tl_status resize(tl_hashmap *map, size_t capacity) {
    uint64_t *hashes = NULL;
    uint32_t *values = NULL;
    if (allocate(capacity, map->hashes == NULL, &hashes, &values) != TL_OK)
        return TL_NOMEM;
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->values[i] != HASHMAP_EMPTY)
            place(hashes, values, capacity, stored_hash(map, i), map->values[i]);
    }
    free(map->hashes);
    free(map->values);
    map->hashes = hashes;
    map->values = values;
    map->capacity = capacity;
    return TL_OK;
}
static bool fits(size_t count, size_t capacity) {
    return count * 100 <= capacity * HASHMAP_MAX_LOAD_PERCENT;
}
tl_status hashmap_reserve(tl_hashmap *map, size_t count) {
    if (map == NULL)
        return TL_INVALID;
    if (count > SIZE_MAX / 100)
        return TL_LIMIT;
    size_t capacity = map->capacity;
    while (!fits(count, capacity)) {
        if (capacity > SIZE_MAX / 2 / sizeof(uint64_t))
            return TL_LIMIT;
        capacity *= 2;
    }
    return capacity == map->capacity ? TL_OK : resize(map, capacity);
}
/* hashmap_find for external-hash maps, kept apart so the stored-hash probe
 * loop below (on the query path: trigram and directory lookups) stays as
 * simple as before. */
static bool find_external(const tl_hashmap *map, uint64_t hash, tl_hashmap_equal equal,
                          const void *context, uint32_t *out) {
    for (size_t slot = bucket(hash, map->capacity); map->values[slot] != HASHMAP_EMPTY;
         slot = (slot + 1) & (map->capacity - 1)) {
        if (stored_hash(map, slot) == hash && equal(context, map->values[slot])) {
            *out = map->values[slot];
            return true;
        }
    }
    return false;
}
bool hashmap_find(const tl_hashmap *map, uint64_t hash, tl_hashmap_equal equal, const void *context,
                  uint32_t *out) {
    if (map == NULL || equal == NULL || out == NULL)
        return false;
    if (map->hashes == NULL)
        return find_external(map, hash, equal, context, out);
    for (size_t slot = bucket(hash, map->capacity); map->values[slot] != HASHMAP_EMPTY;
         slot = (slot + 1) & (map->capacity - 1)) {
        if (map->hashes[slot] == hash && equal(context, map->values[slot])) {
            *out = map->values[slot];
            return true;
        }
    }
    return false;
}
tl_status hashmap_insert(tl_hashmap *map, uint64_t hash, uint32_t value) {
    if (map == NULL || value == HASHMAP_EMPTY)
        return TL_INVALID;
    if (!fits(map->count + 1, map->capacity)) {
        if (map->capacity > SIZE_MAX / 2 / sizeof(uint64_t))
            return TL_LIMIT;
        tl_status status = resize(map, map->capacity * 2);
        if (status != TL_OK)
            return status;
    }
    place(map->hashes, map->values, map->capacity, hash, value);
    map->count++;
    return TL_OK;
}
/* Backward-shift deletion: walk the probe run after the hole and move back
 * every entry whose home bucket does not lie between the hole and its slot
 * (cyclically), so each stays reachable from its home without tombstones. */
static void close_hole(tl_hashmap *map, size_t hole) {
    size_t mask = map->capacity - 1;
    for (size_t slot = (hole + 1) & mask; map->values[slot] != HASHMAP_EMPTY;
         slot = (slot + 1) & mask) {
        size_t home = bucket(stored_hash(map, slot), map->capacity);
        if (((slot - home) & mask) < ((slot - hole) & mask))
            continue;
        if (map->hashes != NULL)
            map->hashes[hole] = map->hashes[slot];
        map->values[hole] = map->values[slot];
        hole = slot;
    }
    map->values[hole] = HASHMAP_EMPTY;
}
bool hashmap_remove(tl_hashmap *map, uint64_t hash, tl_hashmap_equal equal, const void *context) {
    if (map == NULL || equal == NULL)
        return false;
    for (size_t slot = bucket(hash, map->capacity); map->values[slot] != HASHMAP_EMPTY;
         slot = (slot + 1) & (map->capacity - 1)) {
        if (stored_hash(map, slot) == hash && equal(context, map->values[slot])) {
            close_hole(map, slot);
            map->count--;
            return true;
        }
    }
    return false;
}
size_t hashmap_count(const tl_hashmap *map) {
    return map == NULL ? 0 : map->count;
}
uint64_t hashmap_hash(uint64_t state, const void *bytes, size_t length) {
    const uint64_t prime = UINT64_C(0x100000001b3);
    const unsigned char *data = bytes;
    for (size_t i = 0; i < length; i++) {
        state ^= data[i];
        state *= prime;
    }
    return state;
}
/* MurmurHash3 x64_128 (Austin Appleby, public domain): multiply and rotate
 * constants of the reference implementation. */
#define MURMUR_C1 UINT64_C(0x87c37b91114253d5)
#define MURMUR_C2 UINT64_C(0x4cf5ad432745937f)
#define MURMUR_BLOCK 16U
#define MURMUR_WORD 8U
static uint64_t rotate_left(uint64_t value, unsigned bits) {
    return (value << bits) | (value >> (64U - bits));
}
static uint64_t mix_first(uint64_t word) {
    return rotate_left(word * MURMUR_C1, 31) * MURMUR_C2;
}
static uint64_t mix_second(uint64_t word) {
    return rotate_left(word * MURMUR_C2, 33) * MURMUR_C1;
}
/* Final avalanche: every input bit affects every output bit. */
static uint64_t mix_final(uint64_t value) {
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33;
    return value;
}
static uint64_t load_word(const unsigned char *bytes) {
    uint64_t word = 0;
    memcpy(&word, bytes, sizeof(word));
    return word;
}
tl_hash_pair hashmap_hash_pair(const void *bytes, size_t length) {
    const unsigned char *data = bytes;
    uint64_t first = 0, second = 0;
    size_t blocks = length / MURMUR_BLOCK;
    for (size_t i = 0; i < blocks; i++) {
        const unsigned char *block = data + i * MURMUR_BLOCK;
        first ^= mix_first(load_word(block));
        first = rotate_left(first, 27) + second;
        first = first * 5 + 0x52dce729;
        second ^= mix_second(load_word(block + MURMUR_WORD));
        second = rotate_left(second, 31) + first;
        second = second * 5 + 0x38495ab5;
    }
    /* The last 0 to 15 bytes, little-endian, as the reference reads them. */
    uint64_t tail[2] = {0, 0};
    size_t rest = length % MURMUR_BLOCK;
    for (size_t i = 0; i < rest; i++)
        tail[i / MURMUR_WORD] |= (uint64_t)data[blocks * MURMUR_BLOCK + i]
                                 << (MURMUR_WORD * (i % MURMUR_WORD));
    if (rest > MURMUR_WORD)
        second ^= mix_second(tail[1]);
    if (rest > 0)
        first ^= mix_first(tail[0]);
    first ^= (uint64_t)length;
    second ^= (uint64_t)length;
    first += second;
    second += first;
    first = mix_final(first);
    second = mix_final(second);
    first += second;
    second += first;
    return (tl_hash_pair){first, second};
}
