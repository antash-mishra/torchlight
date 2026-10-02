/* Ordered subsequence checks after a conservative mask rejection, and a
 * double-buffered cache of the last word's complete membership. */
#include "torchlight/subseq.h"
#include <stdlib.h>
#include <string.h>
/* The word and mode a membership list belongs to. */
struct cached_word {
    int mode;
    size_t length;
    uint32_t symbols[SUBSEQ_CACHE_MAX_SYMBOLS];
};
struct tl_subseq_cache {
    uint32_t *buffers[2];
    size_t capacity, count;
    /* buffers[current] holds the committed membership; the other is written. */
    int current;
    bool valid, recording;
    struct cached_word committed, pending;
};
bool subseq_matches(tl_text text, tl_text query) {
    if (query.length == 0)
        return true;
    if (text.symbols == NULL || query.symbols == NULL)
        return false;
    if ((text.mask & query.mask) != query.mask)
        return false;
    size_t matched = 0;
    for (size_t i = 0; i < text.length; i++) {
        if (text.symbols[i] == query.symbols[matched])
            matched++;
        if (matched == query.length)
            return true;
    }
    return false;
}
tl_status subseq_cache_create(size_t capacity, tl_subseq_cache **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    size_t bytes = 0;
    tl_status status = tl_size_multiply(capacity == 0 ? 1 : capacity, sizeof(uint32_t), &bytes);
    if (status != TL_OK)
        return status;
    tl_subseq_cache *cache = calloc(1, sizeof(*cache));
    if (cache == NULL)
        return TL_NOMEM;
    cache->buffers[0] = malloc(bytes);
    cache->buffers[1] = malloc(bytes);
    if (cache->buffers[0] == NULL || cache->buffers[1] == NULL) {
        subseq_cache_destroy(cache);
        return TL_NOMEM;
    }
    cache->capacity = capacity;
    *out = cache;
    return TL_OK;
}
void subseq_cache_destroy(tl_subseq_cache *cache) {
    if (cache == NULL)
        return;
    free(cache->buffers[0]);
    free(cache->buffers[1]);
    free(cache);
}
bool subseq_cache_lookup(const tl_subseq_cache *cache, tl_text word, int mode,
                         const uint32_t **members, size_t *count) {
    if (cache == NULL || members == NULL || count == NULL || !cache->valid ||
        cache->committed.mode != mode || word.length < cache->committed.length ||
        (word.symbols == NULL && word.length != 0))
        return false;
    if (cache->committed.length != 0 && memcmp(word.symbols, cache->committed.symbols,
                                               cache->committed.length * sizeof(uint32_t)) != 0)
        return false;
    *members = cache->buffers[cache->current];
    *count = cache->count;
    return true;
}
tl_status subseq_cache_begin(tl_subseq_cache *cache, tl_text word, int mode, uint32_t **buffer) {
    if (cache == NULL || buffer == NULL || (word.symbols == NULL && word.length != 0))
        return TL_INVALID;
    cache->recording = false;
    if (word.length > SUBSEQ_CACHE_MAX_SYMBOLS)
        return TL_LIMIT;
    cache->pending.mode = mode;
    cache->pending.length = word.length;
    if (word.length != 0)
        memcpy(cache->pending.symbols, word.symbols, word.length * sizeof(uint32_t));
    cache->recording = true;
    *buffer = cache->buffers[1 - cache->current];
    return TL_OK;
}
tl_status subseq_cache_commit(tl_subseq_cache *cache, size_t count) {
    if (cache == NULL)
        return TL_INVALID;
    if (!cache->recording)
        return TL_STATE;
    if (count > cache->capacity)
        return TL_LIMIT;
    cache->recording = false;
    cache->current = 1 - cache->current;
    cache->count = count;
    cache->committed = cache->pending;
    cache->valid = true;
    return TL_OK;
}
