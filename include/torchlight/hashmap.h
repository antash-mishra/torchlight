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
/** Return the hash value was inserted under (maps from hashmap_create_external). */
typedef uint64_t (*tl_hashmap_hash_of)(const void *context, uint32_t value);
/* Two 64-bit halves of one 128-bit hash (hashmap_hash_pair). */
typedef struct {
    uint64_t first, second;
} tl_hash_pair;
/** Create an owned empty map; out is NULL on TL_INVALID/TL_NOMEM. */
tl_status hashmap_create(tl_hashmap **out);
/** Create an owned empty map that stores only values, 4 bytes per slot
 * instead of 12, for callers that keep each value's hash themselves. Whenever
 * the map needs a stored value's hash (lookups, growth, removal) it calls
 * hash_of(context, value), which must return the hash the value was inserted
 * under for as long as the value is stored. context is borrowed and must
 * outlive the map. Same API and errors as hashmap_create otherwise. */
tl_status hashmap_create_external(tl_hashmap_hash_of hash_of, const void *context,
                                  tl_hashmap **out);
/** Grow the table so it holds count values without growing again. TL_INVALID
 * for NULL; TL_NOMEM/TL_LIMIT leave the map unchanged. */
tl_status hashmap_reserve(tl_hashmap *map, size_t count);
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
/** Remove the value found as hashmap_find would find it. Returns whether one
 * was removed (false for NULL arguments or no match). Later lookups of other
 * values keep working: entries after the hole shift back rather than leaving
 * tombstones, so removal never degrades probing. No allocation. */
bool hashmap_remove(tl_hashmap *map, uint64_t hash, tl_hashmap_equal equal, const void *context);
/** Return the number of stored values; zero for NULL, no errors. */
size_t hashmap_count(const tl_hashmap *map);
/** Extend an FNV-1a hash state with length bytes. Hashing a then b equals
 * hashing their concatenation, so callers can hash keys in pieces. */
uint64_t hashmap_hash(uint64_t state, const void *bytes, size_t length);
/** Hash length bytes in one pass, eight at a time, into the two halves of a
 * 128-bit hash (MurmurHash3 x64_128, seed 0): several times faster than two
 * hashmap_hash passes over a path. Words are read in native byte order, so
 * values are for in-memory use only, never persisted. No errors. */
tl_hash_pair hashmap_hash_pair(const void *bytes, size_t length);
#endif
