/* Checked vector growth, hash map probing/growth and arithmetic boundary tests. */
#include "test.h"
#include "torchlight/hashmap.h"
#include "torchlight/vec.h"
#include <string.h>
static void vectors(void) {
    tl_vec *vec = NULL;
    CHECK(vec_create(0, &vec) == TL_INVALID && vec == NULL);
    CHECK(vec_create(sizeof(size_t), &vec) == TL_OK);
    for (size_t i = 0; i < 1000; i++)
        CHECK(vec_append(vec, &i) == TL_OK);
    const size_t *items = vec_const_data(vec);
    CHECK(vec_count(vec) == 1000);
    for (size_t i = 0; i < 1000; i++)
        CHECK(items[i] == i);
    CHECK(vec_append(vec, NULL) == TL_INVALID);
    size_t more[3] = {7, 8, 9};
    CHECK(vec_append_array(vec, more, 3) == TL_OK && vec_count(vec) == 1003);
    CHECK(vec_append_array(vec, NULL, 0) == TL_OK && vec_append_array(vec, NULL, 1) == TL_INVALID);
    CHECK(((const size_t *)vec_const_data(vec))[1002] == 9);
    vec_shrink(vec);
    CHECK(vec_count(vec) == 1003);
    vec_truncate(vec, 2000); /* not smaller: unchanged */
    CHECK(vec_count(vec) == 1003);
    vec_truncate(vec, 2);
    CHECK(vec_count(vec) == 2 && ((const size_t *)vec_const_data(vec))[1] == 1);
    vec_truncate(NULL, 0);
    vec_clear(vec);
    CHECK(vec_count(vec) == 0 && vec_reserve(vec, 5000) == TL_OK);
    vec_destroy(vec);
    CHECK(vec_count(NULL) == 0);
    vec_destroy(NULL);
}
struct keys {
    const char *const *names;
    const char *wanted;
};
static bool same_name(const void *context, uint32_t value) {
    const struct keys *keys = context;
    return strcmp(keys->names[value], keys->wanted) == 0;
}
static void hash_maps(void) {
    static const char *const names[] = {"alpha", "beta", "gamma", "delta"};
    tl_hashmap *map = NULL;
    CHECK(hashmap_create(&map) == TL_OK && hashmap_count(map) == 0);
    for (uint32_t i = 0; i < 4; i++)
        CHECK(hashmap_insert(map, hashmap_hash(HASHMAP_HASH_SEED, names[i], strlen(names[i])), i) ==
              TL_OK);
    CHECK(hashmap_insert(map, 1, UINT32_MAX) == TL_INVALID);
    struct keys keys = {names, "gamma"};
    uint32_t found = 0;
    CHECK(hashmap_find(map, hashmap_hash(HASHMAP_HASH_SEED, "gamma", 5), same_name, &keys, &found));
    CHECK(found == 2);
    keys.wanted = "omega";
    CHECK(
        !hashmap_find(map, hashmap_hash(HASHMAP_HASH_SEED, "omega", 5), same_name, &keys, &found));
    /* Equal hashes with different keys must be told apart by the callback. */
    CHECK(hashmap_insert(map, 42, 0) == TL_OK && hashmap_insert(map, 42, 3) == TL_OK);
    keys.wanted = "delta";
    CHECK(hashmap_find(map, 42, same_name, &keys, &found) && found == 3);
    /* Growth keeps every value reachable. */
    for (uint32_t i = 0; i < 5000; i++)
        CHECK(hashmap_insert(map, UINT64_C(1000000) + i, i % 4) == TL_OK);
    CHECK(hashmap_count(map) == 5006);
    keys.wanted = "beta";
    CHECK(hashmap_find(map, UINT64_C(1000000) + 4001, same_name, &keys, &found) && found == 1);
    /* Hashing in pieces equals hashing the concatenation. */
    CHECK(hashmap_hash(hashmap_hash(HASHMAP_HASH_SEED, "ab", 2), "cd", 2) ==
          hashmap_hash(HASHMAP_HASH_SEED, "abcd", 4));
    hashmap_destroy(map);
    hashmap_destroy(NULL);
}
static bool same_index(const void *context, uint32_t value) {
    return value == *(const uint32_t *)context;
}
/* Every seventh value shares one hash, so removals happen inside one long
 * probe run as well as in ordinary short ones. */
static uint64_t removal_hash(uint32_t value) {
    return value % 7 == 0 ? 99 : hashmap_hash(HASHMAP_HASH_SEED, &value, sizeof(value));
}
/* Removal (M7 watch slots) leaves every other value reachable, including
 * values that probed past the removed one, and removed values can return. */
static void hash_map_removal(void) {
    enum { REMOVAL_VALUES = 3000, REMOVAL_STRIDE = 3 };
    tl_hashmap *map = NULL;
    CHECK(hashmap_create(&map) == TL_OK);
    for (uint32_t i = 0; i < REMOVAL_VALUES; i++)
        CHECK(hashmap_insert(map, removal_hash(i), i) == TL_OK);
    for (uint32_t i = 0; i < REMOVAL_VALUES; i += REMOVAL_STRIDE) {
        CHECK(hashmap_remove(map, removal_hash(i), same_index, &i));
        CHECK(!hashmap_remove(map, removal_hash(i), same_index, &i));
    }
    size_t removed = (REMOVAL_VALUES + REMOVAL_STRIDE - 1) / REMOVAL_STRIDE;
    CHECK(hashmap_count(map) == REMOVAL_VALUES - removed);
    for (uint32_t i = 0; i < REMOVAL_VALUES; i++) {
        uint32_t found = UINT32_MAX;
        bool kept = i % REMOVAL_STRIDE != 0;
        CHECK(hashmap_find(map, removal_hash(i), same_index, &i, &found) == kept);
        CHECK(!kept || found == i);
    }
    for (uint32_t i = 0; i < REMOVAL_VALUES; i += REMOVAL_STRIDE)
        CHECK(hashmap_insert(map, removal_hash(i), i) == TL_OK);
    for (uint32_t i = 0; i < REMOVAL_VALUES; i++) {
        uint32_t found = UINT32_MAX;
        CHECK(hashmap_find(map, removal_hash(i), same_index, &i, &found) && found == i);
    }
    uint32_t any = 0;
    CHECK(!hashmap_remove(NULL, 99, same_index, &any) && !hashmap_remove(map, 99, NULL, &any));
    hashmap_destroy(map);
}
/* External-hash maps (M7 Phase 3 ledger index): hashes come from the
 * caller's table; growth, reservation and removal must keep every value
 * reachable through them. */
static uint64_t table_hash(const void *context, uint32_t value) {
    return ((const uint64_t *)context)[value];
}
static void external_hash_maps(void) {
    enum { EXTERNAL_VALUES = 4000, EXTERNAL_STRIDE = 5 };
    static uint64_t hashes[EXTERNAL_VALUES];
    for (uint32_t i = 0; i < EXTERNAL_VALUES; i++)
        hashes[i] = removal_hash(i);
    tl_hashmap *map = NULL;
    CHECK(hashmap_create_external(NULL, hashes, &map) == TL_INVALID && map == NULL);
    CHECK(hashmap_create_external(table_hash, hashes, NULL) == TL_INVALID);
    CHECK(hashmap_create_external(table_hash, hashes, &map) == TL_OK);
    CHECK(hashmap_reserve(map, EXTERNAL_VALUES / 2) == TL_OK &&
          hashmap_reserve(NULL, 1) == TL_INVALID);
    for (uint32_t i = 0; i < EXTERNAL_VALUES; i++) /* past the reservation: grows */
        CHECK(hashmap_insert(map, hashes[i], i) == TL_OK);
    for (uint32_t i = 0; i < EXTERNAL_VALUES; i += EXTERNAL_STRIDE)
        CHECK(hashmap_remove(map, hashes[i], same_index, &i));
    CHECK(hashmap_reserve(map, 3 * EXTERNAL_VALUES) == TL_OK);
    for (uint32_t i = 0; i < EXTERNAL_VALUES; i++) {
        uint32_t found = UINT32_MAX;
        bool kept = i % EXTERNAL_STRIDE != 0;
        CHECK(hashmap_find(map, hashes[i], same_index, &i, &found) == kept);
        CHECK(!kept || found == i);
    }
    CHECK(hashmap_count(map) == EXTERNAL_VALUES - EXTERNAL_VALUES / EXTERNAL_STRIDE);
    hashmap_destroy(map);
}
/* hashmap_hash_pair is MurmurHash3 x64_128 with seed 0: the reference
 * vectors (as Python's mmh3.hash64 reports "foo"), and every length of tail. */
static void hash_pairs(void) {
    tl_hash_pair empty = hashmap_hash_pair("", 0), foo = hashmap_hash_pair("foo", 3);
    CHECK(empty.first == 0 && empty.second == 0);
    CHECK(foo.first == UINT64_C(16316970633193145697) &&
          foo.second == UINT64_C(9128664383759220103));
    char text[40] = "0123456789abcdefghijklmnopqrstuvwxyzABC";
    tl_hash_pair previous = empty;
    for (size_t length = 1; length < sizeof(text); length++) {
        tl_hash_pair pair = hashmap_hash_pair(text, length);
        CHECK(pair.first != pair.second && pair.first != previous.first);
        text[length - 1] ^= 1; /* any byte, in a block or the tail, changes it */
        tl_hash_pair flipped = hashmap_hash_pair(text, length);
        CHECK(flipped.first != pair.first && flipped.second != pair.second);
        text[length - 1] ^= 1;
        previous = pair;
    }
}
void test_core(void) {
    size_t bytes = 0;
    CHECK(tl_size_multiply(SIZE_MAX, 2, &bytes) == TL_LIMIT);
    CHECK(tl_size_multiply(0, SIZE_MAX, &bytes) == TL_OK && bytes == 0);
    CHECK(tl_size_multiply(1, 1, NULL) == TL_INVALID);
    vectors();
    hash_maps();
    hash_map_removal();
    external_hash_maps();
    hash_pairs();
}
