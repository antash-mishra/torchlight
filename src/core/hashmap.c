/* Linear-probing hash map storing hashes and uint32 values; keys stay external. */
#include "torchlight/hashmap.h"
#include <stdlib.h>
enum {
    HASHMAP_INITIAL_CAPACITY = 64,
    /* Grow when more than half full: linear probing degrades quickly above that. */
    HASHMAP_MAX_LOAD_PERCENT = 50
};
static const uint32_t HASHMAP_EMPTY = UINT32_MAX;
struct tl_hashmap {
    uint64_t *hashes;
    uint32_t *values;
    size_t count, capacity;
};
/* FNV-1a has weak low bits; mix before masking so similar keys spread out. */
static size_t bucket(uint64_t hash, size_t capacity) {
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    return (size_t)hash & (capacity - 1);
}
static tl_status allocate(size_t capacity, uint64_t **hashes, uint32_t **values) {
    *hashes = malloc(capacity * sizeof(**hashes));
    *values = malloc(capacity * sizeof(**values));
    if (*hashes == NULL || *values == NULL) {
        free(*hashes);
        free(*values);
        return TL_NOMEM;
    }
    for (size_t i = 0; i < capacity; i++)
        (*values)[i] = HASHMAP_EMPTY;
    return TL_OK;
}
tl_status hashmap_create(tl_hashmap **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_hashmap *map = calloc(1, sizeof(*map));
    if (map == NULL)
        return TL_NOMEM;
    if (allocate(HASHMAP_INITIAL_CAPACITY, &map->hashes, &map->values) != TL_OK) {
        free(map);
        return TL_NOMEM;
    }
    map->capacity = HASHMAP_INITIAL_CAPACITY;
    *out = map;
    return TL_OK;
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
    hashes[slot] = hash;
    values[slot] = value;
}
static tl_status grow(tl_hashmap *map) {
    if (map->capacity > SIZE_MAX / 2 / sizeof(uint64_t))
        return TL_LIMIT;
    size_t capacity = map->capacity * 2;
    uint64_t *hashes = NULL;
    uint32_t *values = NULL;
    if (allocate(capacity, &hashes, &values) != TL_OK)
        return TL_NOMEM;
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->values[i] != HASHMAP_EMPTY)
            place(hashes, values, capacity, map->hashes[i], map->values[i]);
    }
    free(map->hashes);
    free(map->values);
    map->hashes = hashes;
    map->values = values;
    map->capacity = capacity;
    return TL_OK;
}
bool hashmap_find(const tl_hashmap *map, uint64_t hash, tl_hashmap_equal equal, const void *context,
                  uint32_t *out) {
    if (map == NULL || equal == NULL || out == NULL)
        return false;
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
    if ((map->count + 1) * 100 > map->capacity * HASHMAP_MAX_LOAD_PERCENT) {
        tl_status status = grow(map);
        if (status != TL_OK)
            return status;
    }
    place(map->hashes, map->values, map->capacity, hash, value);
    map->count++;
    return TL_OK;
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
