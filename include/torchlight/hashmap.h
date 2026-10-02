/* Open-addressing map from caller-computed 64-bit hashes to uint32 values.
 * Keys stay owned by the caller: lookups pass an equality callback that
 * compares the caller's key with the key behind a stored value. */
#ifndef TORCHLIGHT_HASHMAP_H
#define TORCHLIGHT_HASHMAP_H
#include "torchlight/common.h"
#include <stdbool.h>
/* Starting state for hashmap_hash; pass the previous result to extend it. */
#define HASHMAP_HASH_SEED UINT64_C(0xcbf29ce484222325)
typedef struct tl_hashmap tl_hashmap;
/** Return whether the caller key in context equals the key stored for value. */
typedef bool (*tl_hashmap_equal)(const void *context, uint32_t value);
/** Create an owned empty map; out is NULL on TL_INVALID/TL_NOMEM. */
tl_status hashmap_create(tl_hashmap **out);
/** Free the map; caller keys are untouched. NULL is allowed. */
void hashmap_destroy(tl_hashmap *map);
/** Find the value whose stored hash equals hash and for which equal(context,
 * value) is true. Returns true and writes *out on a hit; false otherwise or for
 * NULL arguments. No allocation. */
bool hashmap_find(const tl_hashmap *map, uint64_t hash, tl_hashmap_equal equal, const void *context,
                  uint32_t *out);
/** Insert value under hash; the caller guarantees its key is not present yet.
 * value must not be UINT32_MAX (reserved for empty slots): TL_INVALID. Growth
 * failures return TL_NOMEM/TL_LIMIT and leave the map unchanged. */
tl_status hashmap_insert(tl_hashmap *map, uint64_t hash, uint32_t value);
/** Return the number of stored values; zero for NULL, no errors. */
size_t hashmap_count(const tl_hashmap *map);
/** Extend an FNV-1a hash state with length bytes. Hashing a then b equals
 * hashing their concatenation, so callers can hash keys in pieces. */
uint64_t hashmap_hash(uint64_t state, const void *bytes, size_t length);
#endif
