/* Query evaluation for the sealed lexical engine:
 *   0. one-symbol queries are answered from results precomputed at seal time;
 *   1. per word, collect channel hits (prefix, typo, trigram) for entries;
 *      parent directories are scored lazily, at most once per word;
 *   2. for the first word, scan every entry (or only the cached membership of
 *      a word it extends), unless enough strong hits prove the scan pointless;
 *   3. filter candidates by every other word, deferring parent-only matches
 *      of the first word until the results so far cannot rule them out;
 *   4. keep the best results in a bounded heap with deterministic ties.
 * Every shortcut is exact: results equal those of a full evaluation. No
 * allocation, filesystem I/O or SQL happens here. */
#include "lexical_internal.h"
#include "torchlight/fuzzy.h"
#include "torchlight/parallel.h"
#include "torchlight/subseq.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
enum {
    LEXICAL_EXACT_PATH = 20000000,
    LEXICAL_EXACT_BASENAME = 10000000,
    LEXICAL_ROOT_SCORE = 1,
    /* Added to basename subsequence scores (fuzzy_score) so they outrank
     * subsequences inside parent directory names. */
    LEXICAL_BASENAME_BONUS = 1500,
    /* A parent directory with a name/token starting with the word (e.g. the
     * "finance" folder for "finance invoice") is strong context: it outranks
     * weak, scattered basename subsequences, but no basename key prefix. */
    LEXICAL_PARENT_PREFIX_SCORE = 2500,
    /* A whole basename token one edit away: below every prefix hit, above
     * ordinary subsequence matches. */
    LEXICAL_TYPO_SCORE = 3500,
    /* Trigram hits score 1000..2000 by shared fraction: below subsequences. */
    LEXICAL_TRIGRAM_BASE = 1000,
    LEXICAL_TRIGRAM_RANGE = 1000,
    /* Shorter basenames win otherwise-equal matches (README.md before
     * README-old.md) by up to this much; smaller than any tier gap. */
    LEXICAL_LENGTH_BONUS_MAX = 64,
    LEXICAL_QUERY_SYMBOLS = LEXICAL_QUERY_BYTES * 4,
    /* Above this fraction of nodes, resolving parents sequentially costs less
     * than repeatedly walking random ancestor chains during a batch. */
    LEXICAL_DIRECTORY_BATCH_DIVISOR = 8,
    /* Four participants fit the reference CPU while leaving the writer room.
     * Small engines/batches avoid thread lifecycle and dispatch overhead. */
    LEXICAL_PARALLEL_ENGINE_MIN = 65536,
    LEXICAL_PARALLEL_BATCH_MIN = 4096,
    LEXICAL_PARALLEL_PARTICIPANTS = 4,
    /* Retain filename/context words through fallback without a table for every
     * possible query word. Timestamp names commonly need four words. */
    LEXICAL_WORD_CACHES = 4,
    /* Words need one byte each plus a separating byte. */
    LEXICAL_MAX_WORDS = LEXICAL_QUERY_BYTES / 2 + 1
};
_Static_assert((int)LEXICAL_TYPO_SCORE < (int)PREFIX_INITIALS_SCORE,
               "typos rank below prefix hits");
_Static_assert((int)LEXICAL_PARENT_PREFIX_SCORE < (int)LEXICAL_TYPO_SCORE,
               "parent context ranks below basename typos");
_Static_assert(LEXICAL_TRIGRAM_BASE + LEXICAL_TRIGRAM_RANGE < LEXICAL_PARENT_PREFIX_SCORE,
               "partial trigram overlap ranks below strong parent context");
/* Sum of the largest per-word scores for any query stays below exact matches
 * (fuzzy_score_bound(n) is 256 + 72n, so the fuzzy part sums to at most
 * 256 per word plus 72 per query symbol). */
_Static_assert((long long)LEXICAL_MAX_WORDS *(PREFIX_BASENAME_SCORE + LEXICAL_PREFIX_BONUS_MAX +
                                              PREFIX_COMPLETE_BONUS + LEXICAL_BASENAME_BONUS +
                                              LEXICAL_LENGTH_BONUS_MAX + 256) +
                       72LL * LEXICAL_QUERY_SYMBOLS <
                   LEXICAL_EXACT_BASENAME,
               "word scores cannot reach exact-match priority");
struct word {
    tl_text text;
    uint64_t repeats; /* lexical_repeat_mask of the word */
    bool path;        /* contains '/': match across the full path */
    int maximum;      /* query-specific bound once its channels are prepared */
};
struct heap_item {
    int score;
    uint32_t slot;
};
struct entry_match {
    int score;
    bool subsequence, parent_only;
};
struct word_cache {
    int *hit;
    uint32_t *touched;
    size_t touched_count;
    int channel_max;
    int *nearest;
    uint32_t *nearest_stamp, *prefix_stamp;
    const struct word *word;
    uint32_t query_epoch, dir_epoch;
    bool complete;
};
struct tl_lexical_workspace {
    const tl_lexical *engine;
    /* Per entry: channel score of the current word, and accumulated total
     * (zero means "not a candidate"). */
    int *hit, *total;
    uint32_t *touched, *candidates, *deferred;
    size_t touched_count, candidate_count, deferred_count;
    /* seen[slot] == epoch marks entries already pushed or made candidates in
     * this query; the epoch advances per query, so no reset pass is needed. */
    uint32_t *seen, epoch;
    /* Single-word queries stream matches straight into the heap ("direct");
     * multiword queries collect candidates and filter them by later words. */
    bool direct;
    /* path_order ranks [exact_lo, exact_hi) equal the raw query exactly. */
    size_t exact_lo, exact_hi;
    /* The normalized query without surrounding whitespace, for exact names. */
    tl_text trimmed;
    /* Largest subsequence score the current word can give any entry. */
    int subsequence_bound, channel_max;
    /* Per directory node, valid for the current word when its stamp equals
     * dir_epoch: the score of the nearest matching usable ancestor-or-self.
     * Directories are scored lazily, only when a scored entry needs them. */
    int *nearest;
    uint32_t *nearest_stamp, *prefix_stamp, dir_epoch;
    struct word_cache evidence[LEXICAL_WORD_CACHES];
    size_t active_word;
    /* Full-path reconstruction for words containing '/'. */
    uint32_t *chain_nodes, *chain_symbols;
    uint8_t *chain_boundaries;
    tl_subseq_cache *cache;
    tl_trigram_scratch *trigram;
    tl_typo_scratch *typo;
    tl_mask_scratch *name_masks;
    tl_parallel *parallel;
    struct entry_match *batch;
    struct heap_item heap[LEXICAL_MAX_RESULTS];
    size_t heap_count;
    struct word words[LEXICAL_MAX_WORDS];
    uint32_t symbols[LEXICAL_QUERY_SYMBOLS];
    uint8_t boundaries[LEXICAL_QUERY_SYMBOLS];
    size_t offsets[LEXICAL_QUERY_SYMBOLS];
};
static void *allocate_array(size_t count, size_t size) {
    size_t bytes = 0;
    if (tl_size_multiply(count == 0 ? 1 : count, size, &bytes) != TL_OK)
        return NULL;
    return calloc(1, bytes);
}
static tl_status allocate_workspace(const tl_lexical *engine, tl_lexical_workspace *workspace) {
    size_t entries = engine->count, nodes = dirtree_count(engine->tree);
    size_t chain = engine->max_path_symbols + 1;
    struct {
        void **array;
        size_t count, size;
    } arrays[] = {{(void **)&workspace->total, entries, sizeof(int)},
                  {(void **)&workspace->candidates, entries, sizeof(uint32_t)},
                  {(void **)&workspace->deferred, entries, sizeof(uint32_t)},
                  {(void **)&workspace->seen, entries, sizeof(uint32_t)},
                  {(void **)&workspace->batch, entries, sizeof(struct entry_match)},
                  {(void **)&workspace->chain_nodes, chain, sizeof(uint32_t)},
                  {(void **)&workspace->chain_symbols, chain, sizeof(uint32_t)},
                  {(void **)&workspace->chain_boundaries, chain, sizeof(uint8_t)}};
    for (size_t i = 0; i < sizeof(arrays) / sizeof(arrays[0]); i++) {
        *arrays[i].array = allocate_array(arrays[i].count, arrays[i].size);
        if (*arrays[i].array == NULL)
            return TL_NOMEM;
    }
    for (size_t i = 0; i < LEXICAL_WORD_CACHES; i++) {
        struct word_cache *cache = &workspace->evidence[i];
        cache->hit = allocate_array(entries, sizeof(int));
        cache->touched = allocate_array(entries, sizeof(uint32_t));
        cache->nearest = allocate_array(nodes, sizeof(int));
        cache->nearest_stamp = allocate_array(nodes, sizeof(uint32_t));
        cache->prefix_stamp = allocate_array(nodes, sizeof(uint32_t));
        if (cache->hit == NULL || cache->touched == NULL || cache->nearest == NULL ||
            cache->nearest_stamp == NULL || cache->prefix_stamp == NULL)
            return TL_NOMEM;
    }
    tl_status status = subseq_cache_create(entries, &workspace->cache);
    if (status == TL_OK)
        status = trigram_scratch_create(engine->trigram, &workspace->trigram);
    if (status == TL_OK)
        status = typo_scratch_create(engine->typo, &workspace->typo);
    if (status == TL_OK)
        status = mask_scratch_create(engine->name_masks, &workspace->name_masks);
    if (status == TL_OK && entries >= LEXICAL_PARALLEL_ENGINE_MIN)
        status = parallel_create(LEXICAL_PARALLEL_PARTICIPANTS, &workspace->parallel);
    return status;
}
tl_status lexical_workspace_create(const tl_lexical *engine, tl_lexical_workspace **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (engine == NULL)
        return TL_INVALID;
    if (!engine->finished)
        return TL_STATE;
    tl_lexical_workspace *workspace = calloc(1, sizeof(*workspace));
    if (workspace == NULL)
        return TL_NOMEM;
    tl_status status = allocate_workspace(engine, workspace);
    if (status != TL_OK) {
        lexical_workspace_destroy(workspace);
        return status;
    }
    workspace->engine = engine;
    *out = workspace;
    return TL_OK;
}
void lexical_workspace_destroy(tl_lexical_workspace *workspace) {
    if (workspace == NULL)
        return;
    parallel_destroy(workspace->parallel);
    void *arrays[] = {workspace->total,           workspace->candidates,
                      workspace->deferred,        workspace->seen,
                      workspace->chain_nodes,     workspace->chain_symbols,
                      workspace->chain_boundaries};
    for (size_t i = 0; i < sizeof(arrays) / sizeof(arrays[0]); i++)
        free(arrays[i]);
    for (size_t i = 0; i < LEXICAL_WORD_CACHES; i++) {
        free(workspace->evidence[i].hit);
        free(workspace->evidence[i].touched);
        free(workspace->evidence[i].nearest);
        free(workspace->evidence[i].nearest_stamp);
        free(workspace->evidence[i].prefix_stamp);
    }
    subseq_cache_destroy(workspace->cache);
    trigram_scratch_destroy(workspace->trigram);
    typo_scratch_destroy(workspace->typo);
    mask_scratch_destroy(workspace->name_masks);
    free(workspace->batch);
    free(workspace);
}
/* ---- channel hits ------------------------------------------------------- */
static void record_hit(tl_lexical_workspace *workspace, size_t slot, int score) {
    if (workspace->hit[slot] == 0)
        workspace->touched[workspace->touched_count++] = (uint32_t)slot;
    if (score > workspace->hit[slot])
        workspace->hit[slot] = score;
    if (score > workspace->channel_max)
        workspace->channel_max = score;
}
static tl_status on_prefix_hit(void *context, size_t slot, int score) {
    tl_lexical_workspace *workspace = context;
    record_hit(workspace, slot, score + workspace->engine->prefix_bonus);
    return TL_OK;
}
static tl_status on_field_hit(void *context, size_t field, int score) {
    tl_lexical_workspace *workspace = context;
    const struct lexical_field *fields = vec_const_data(workspace->engine->fields);
    bool complete_token = score == PREFIX_BASENAME_SCORE + PREFIX_COMPLETE_BONUS ||
                          score == PREFIX_TOKEN_SCORE + PREFIX_COMPLETE_BONUS;
    int complete = complete_token ? PREFIX_COMPLETE_BONUS : 0;
    record_hit(workspace, fields[field].slot, fields[field].weight + complete);
    return TL_OK;
}
/* Auxiliary fields are normally a small application catalog. Prefix hits are
 * indexed; mask-filtered fuzzy evidence preserves abbreviation coverage. */
static tl_status prepare_fields(tl_lexical_workspace *workspace, tl_text word) {
    const tl_lexical *engine = workspace->engine;
    tl_status status = prefix_query(engine->field_prefix, word, on_field_hit, workspace);
    const struct lexical_field *fields = vec_const_data(engine->fields);
    for (size_t i = 0; i < vec_count(engine->fields) && status == TL_OK; i++) {
        int score = 0;
        status = fuzzy_score(tokenize_view(fields[i].text), word, &score);
        if (status == TL_OK && score > 0)
            record_hit(workspace, fields[i].slot, score);
    }
    return status;
}
static tl_status on_typo_hit(void *context, size_t slot) {
    record_hit(context, slot, LEXICAL_TYPO_SCORE);
    return TL_OK;
}
static tl_status on_trigram_hit(void *context, size_t slot, size_t shared, size_t total) {
    record_hit(context, slot, LEXICAL_TRIGRAM_BASE + (int)(LEXICAL_TRIGRAM_RANGE * shared / total));
    return TL_OK;
}
/* Mark a directory whose name has a key starting with the word. Any key kind
 * inside a directory name counts as the same parent-context evidence. */
static tl_status on_directory_hit(void *context, size_t node, int score) {
    tl_lexical_workspace *workspace = context;
    (void)score;
    workspace->prefix_stamp[node] = workspace->dir_epoch;
    return TL_OK;
}
static void reset_hits(struct word_cache *cache) {
    for (size_t i = 0; i < cache->touched_count; i++)
        cache->hit[cache->touched[i]] = 0;
    cache->touched_count = 0;
    cache->channel_max = 0;
}
/* A directory's own evidence for the word: a subsequence within its name, or
 * at least the parent prefix score when one of its keys starts with the word.
 * Directories above the indexed roots carry no evidence. */
static tl_status own_score(tl_lexical_workspace *workspace, tl_text word, uint32_t node, int *out) {
    const tl_lexical *engine = workspace->engine;
    *out = 0;
    if ((engine->dir_masks[node] & word.mask) != word.mask)
        return TL_OK;
    tl_text name = dirtree_name(engine->tree, node);
    if (name.length == 0 || !engine->usable[node])
        return TL_OK;
    tl_status status = fuzzy_score(name, word, out);
    if (status == TL_OK && workspace->prefix_stamp[node] == workspace->dir_epoch &&
        *out < LEXICAL_PARENT_PREFIX_SCORE)
        *out = LEXICAL_PARENT_PREFIX_SCORE;
    return status;
}
/* Score of the nearest ancestor-or-self of node with its own evidence. Walks
 * up to the closest directory already resolved for this word, then resolves
 * the walked chain top-down, so each directory is scored at most once per word. */
static tl_status nearest_score(tl_lexical_workspace *workspace, tl_text word, uint32_t node,
                               int *out) {
    const tl_dirtree *tree = workspace->engine->tree;
    size_t depth = 0;
    uint32_t cursor = node;
    while (cursor != DIRTREE_NONE && workspace->nearest_stamp[cursor] != workspace->dir_epoch) {
        workspace->chain_nodes[depth++] = cursor;
        cursor = dirtree_parent(tree, cursor);
    }
    int inherited = cursor == DIRTREE_NONE ? 0 : workspace->nearest[cursor];
    while (depth > 0) {
        uint32_t current = workspace->chain_nodes[--depth];
        int own = 0;
        tl_status status = own_score(workspace, word, current, &own);
        if (status != TL_OK)
            return status;
        inherited = own > 0 ? own : inherited;
        workspace->nearest[current] = inherited;
        workspace->nearest_stamp[current] = workspace->dir_epoch;
    }
    *out = inherited;
    return TL_OK;
}
/* Query epochs guard stable word views. Reusing complete channel and directory
 * evidence avoids duplicate lookup/expansion when an early bound needs fallback.
 * Eviction changes work only; tables are reset before use for another word. */
static bool select_evidence(tl_lexical_workspace *workspace, const struct word *word) {
    size_t selected = (workspace->active_word + 1) % LEXICAL_WORD_CACHES;
    bool reused = false;
    for (size_t i = 0; i < LEXICAL_WORD_CACHES; i++) {
        if (workspace->evidence[i].query_epoch == workspace->epoch &&
            workspace->evidence[i].word == word) {
            selected = i;
            reused = true;
            break;
        }
    }
    struct word_cache *cache = &workspace->evidence[selected];
    if (!reused) {
        reset_hits(cache);
        cache->query_epoch = workspace->epoch;
        cache->word = word;
        cache->complete = false;
        if (++cache->dir_epoch == 0) {
            size_t nodes = dirtree_count(workspace->engine->tree);
            memset(cache->nearest_stamp, 0, nodes * sizeof(uint32_t));
            memset(cache->prefix_stamp, 0, nodes * sizeof(uint32_t));
            cache->dir_epoch = 1;
        }
    }
    workspace->active_word = selected;
    workspace->hit = cache->hit;
    workspace->touched = cache->touched;
    workspace->touched_count = cache->touched_count;
    workspace->channel_max = cache->channel_max;
    workspace->nearest = cache->nearest;
    workspace->nearest_stamp = cache->nearest_stamp;
    workspace->prefix_stamp = cache->prefix_stamp;
    workspace->dir_epoch = cache->dir_epoch;
    return reused;
}
/* Resolve a large batch's parent context sequentially. Reuse any nodes already
 * visited lazily in this word; every parent precedes its children. */
static tl_status score_directories(tl_lexical_workspace *workspace, tl_text word) {
    struct word_cache *cache = &workspace->evidence[workspace->active_word];
    if (cache->complete)
        return TL_OK;
    const tl_lexical *engine = workspace->engine;
    size_t nodes = dirtree_count(engine->tree);
    for (uint32_t node = 0; node < nodes; node++) {
        if (workspace->nearest_stamp[node] == workspace->dir_epoch)
            continue;
        int own = 0;
        tl_status status = own_score(workspace, word, node, &own);
        if (status != TL_OK)
            return status;
        uint32_t parent = dirtree_parent(engine->tree, node);
        workspace->nearest[node] =
            own > 0 || parent == DIRTREE_NONE ? own : workspace->nearest[parent];
        workspace->nearest_stamp[node] = workspace->dir_epoch;
    }
    cache->complete = true;
    return TL_OK;
}
/* Largest score a word can earn from subsequence evidence: a basename
 * subsequence, a parent directory key prefix, or a subsequence inside a
 * parent name; words with '/' only have full-path subsequences. */
static int subsequence_max(const struct word *word) {
    int fuzzy = fuzzy_score_bound(word->text.length);
    if (word->path)
        return fuzzy;
    if (fuzzy > INT_MAX - LEXICAL_BASENAME_BONUS)
        return INT_MAX;
    int best = fuzzy + LEXICAL_BASENAME_BONUS;
    return best > LEXICAL_PARENT_PREFIX_SCORE ? best : LEXICAL_PARENT_PREFIX_SCORE;
}
/* Largest score a word can earn from any evidence; basename keys top the
 * channels (typo and trigram hits score less). */
static int word_max(const tl_lexical *engine, const struct word *word) {
    if (word->maximum != 0)
        return word->maximum;
    int best = subsequence_max(word);
    int prefix = PREFIX_BASENAME_SCORE + engine->prefix_bonus + PREFIX_COMPLETE_BONUS;
    return word->path || best > prefix ? best : prefix;
}
/* Gather all channel evidence for one word. Words with '/' only match across
 * the full path, which no channel key contains, so they skip the channels. */
static tl_status prepare_word(tl_lexical_workspace *workspace, struct word *word) {
    const tl_lexical *engine = workspace->engine;
    bool reused = select_evidence(workspace, word);
    workspace->subsequence_bound = subsequence_max(word);
    word->maximum = workspace->subsequence_bound;
    if (workspace->channel_max > word->maximum)
        word->maximum = workspace->channel_max;
    if (reused)
        return TL_OK;
    if (word->path)
        return TL_OK;
    tl_status status = prefix_query(engine->prefix, word->text, on_prefix_hit, workspace);
    if (status == TL_OK)
        status = typo_query(engine->typo, workspace->typo, word->text, on_typo_hit, workspace);
    if (status == TL_OK && word->text.length <= TRIGRAM_MAX_QUERY_SYMBOLS)
        status = trigram_query(engine->trigram, workspace->trigram, word->text, on_trigram_hit,
                               workspace);
    if (status == TL_OK)
        status = prepare_fields(workspace, word->text);
    if (status == TL_OK)
        status = prefix_query(engine->dir_prefix, word->text, on_directory_hit, workspace);
    if (workspace->channel_max > word->maximum)
        word->maximum = workspace->channel_max;
    workspace->evidence[workspace->active_word].touched_count = workspace->touched_count;
    workspace->evidence[workspace->active_word].channel_max = workspace->channel_max;
    return status;
}
/* ---- per-entry scoring -------------------------------------------------- */
static void append_symbols(tl_lexical_workspace *workspace, size_t *length, tl_text text) {
    memcpy(workspace->chain_symbols + *length, text.symbols, text.length * sizeof(uint32_t));
    memcpy(workspace->chain_boundaries + *length, text.boundaries, text.length);
    *length += text.length;
}
static void append_slash(tl_lexical_workspace *workspace, size_t *length) {
    workspace->chain_symbols[*length] = '/';
    workspace->chain_boundaries[*length] = *length == 0;
    (*length)++;
}
/* Subsequence score across the whole normalized path, rebuilt from the
 * directory chain only after the conservative path mask passes. */
static tl_status path_score(tl_lexical_workspace *workspace, tl_text word, size_t slot, int *out) {
    const tl_lexical *engine = workspace->engine;
    uint32_t dir = engine->columns.dirs[slot];
    tl_text name = lexical_name(engine, slot);
    uint64_t mask = dirtree_path_mask(engine->tree, dir) | name.mask;
    *out = 0;
    if ((mask & word.mask) != word.mask)
        return TL_OK;
    size_t depth = 0, length = 0;
    for (uint32_t node = dir; node != DIRTREE_ROOT && node != DIRTREE_NONE;
         node = dirtree_parent(engine->tree, node))
        workspace->chain_nodes[depth++] = node;
    append_slash(workspace, &length);
    while (depth > 0) {
        append_symbols(workspace, &length,
                       dirtree_name(engine->tree, workspace->chain_nodes[--depth]));
        append_slash(workspace, &length);
    }
    if (name.length != 0)
        append_symbols(workspace, &length, name);
    tl_text path = {.symbols = workspace->chain_symbols,
                    .boundaries = workspace->chain_boundaries,
                    .length = length,
                    .mask = mask};
    return fuzzy_score(path, word, out);
}
/* Score one word against one entry into *out, including channel hits. When
 * subsequence is non-NULL it receives a positive value exactly when the word
 * matches by subsequence alone (basename, parent directory name or full
 * path), which membership recording needs; callers that pass NULL let any
 * dominating channel hit skip the fuzzy scan. *parent_only (optional) is set
 * when the only evidence is a parent directory. */
static tl_status entry_score(tl_lexical_workspace *workspace, const struct word *word, size_t slot,
                             int *subsequence, bool *parent_only, int *out) {
    const tl_lexical *engine = workspace->engine;
    int sub = 0, hit = workspace->hit[slot];
    if (parent_only != NULL)
        *parent_only = false;
    /* Without a hit, a symbol missing from the basename and every ancestor
     * rules out all subsequence evidence: the common case in full scans. */
    if (hit == 0 && (engine->columns.contexts[slot] & word->text.mask) != word->text.mask) {
        if (subsequence != NULL)
            *subsequence = 0;
        *out = 0;
        return TL_OK;
    }
    /* Once a hit beats any subsequence score, a fuzzy scan cannot change the
     * score. Prefix hits (>= initials score) come from basename keys, so they
     * also imply subsequence membership; typo and trigram hits do not. */
    bool implies_subsequence = hit >= PREFIX_INITIALS_SCORE;
    if (!word->path && hit > workspace->subsequence_bound &&
        (subsequence == NULL || implies_subsequence)) {
        if (subsequence != NULL)
            *subsequence = hit;
        *out = hit;
        return TL_OK;
    }
    tl_status status = TL_OK;
    if (word->path) {
        status = path_score(workspace, word->text, slot, &sub);
    } else if ((engine->columns.masks[slot] & word->text.mask) == word->text.mask &&
               (engine->columns.repeats[slot] & word->repeats) == word->repeats) {
        status = fuzzy_score(lexical_name(engine, slot), word->text, &sub);
        sub = sub > 0 ? sub + LEXICAL_BASENAME_BONUS : 0;
    }
    if (status == TL_OK && sub == 0 && !word->path) {
        uint32_t dir = engine->columns.dirs[slot];
        if (workspace->nearest_stamp[dir] == workspace->dir_epoch)
            sub = workspace->nearest[dir]; /* already resolved for this word */
        else
            status = nearest_score(workspace, word->text, dir, &sub);
        if (parent_only != NULL)
            *parent_only = sub > 0 && hit == 0;
    }
    if (status != TL_OK)
        return status;
    if (subsequence != NULL)
        *subsequence = sub;
    *out = sub > hit ? sub : hit;
    return TL_OK;
}
struct batch_query {
    tl_lexical_workspace *workspace;
    const struct word *word;
    const uint32_t *slots;
    bool membership;
};
static tl_status score_range(void *context, size_t begin, size_t end) {
    const struct batch_query *query = context;
    for (size_t i = begin; i < end; i++) {
        size_t slot = query->slots == NULL ? i : query->slots[i];
        struct entry_match *match = &query->workspace->batch[i];
        int sub = 0;
        tl_status status =
            entry_score(query->workspace, query->word, slot, query->membership ? &sub : NULL,
                        &match->parent_only, &match->score);
        if (status != TL_OK)
            return status;
        match->subsequence = sub > 0;
    }
    return TL_OK;
}
/* Workers score independently once directory context is read-only and write
 * disjoint outputs. Selection and compaction stay on the coordinator in the
 * original slot order, preserving deterministic ties and complete membership. */
static tl_status score_batch(tl_lexical_workspace *workspace, const struct word *word,
                             const uint32_t *slots, size_t count, bool membership) {
    struct batch_query query = {workspace, word, slots, membership};
    if (workspace->parallel == NULL || word->path || count < LEXICAL_PARALLEL_BATCH_MIN)
        return score_range(&query, 0, count);
    tl_status status = score_directories(workspace, word->text);
    if (status == TL_OK)
        status = parallel_run(workspace->parallel, count, score_range, &query);
    return status;
}
static int length_bonus(const tl_lexical *engine, size_t slot) {
    uint32_t length = engine->columns.name_lengths[slot];
    return length >= LEXICAL_LENGTH_BONUS_MAX ? 0 : LEXICAL_LENGTH_BONUS_MAX - (int)length;
}
static bool exact_name(const tl_lexical *engine, size_t slot, tl_text query) {
    const struct lexical_columns *columns = &engine->columns;
    return columns->name_lengths[slot] == query.length &&
           memcmp(columns->symbols + columns->name_offsets[slot], query.symbols,
                  query.length * sizeof(uint32_t)) == 0;
}
/* Final ranking score of a matched entry: exact basenames get priority,
 * everything else gains the short-name bonus. */
static int final_score(const tl_lexical_workspace *workspace, size_t slot, int score) {
    const tl_lexical *engine = workspace->engine;
    if (exact_name(engine, slot, workspace->trimmed))
        return LEXICAL_EXACT_BASENAME;
    return score + length_bonus(engine, slot);
}
static void heap_push(tl_lexical_workspace *workspace, struct heap_item item, size_t capacity);
/* Record a match of the first word: straight into the heap for single-word
 * queries; otherwise as a candidate for later words to filter, or deferred
 * when its only evidence is a parent directory (see run_query). */
static void push_match(tl_lexical_workspace *workspace, size_t slot, int score, bool parent_only,
                       size_t capacity) {
    if (workspace->seen[slot] == workspace->epoch)
        return;
    workspace->seen[slot] = workspace->epoch;
    if (workspace->direct) {
        heap_push(workspace,
                  (struct heap_item){final_score(workspace, slot, score), (uint32_t)slot},
                  capacity);
        return;
    }
    workspace->total[slot] = score;
    if (parent_only)
        workspace->deferred[workspace->deferred_count++] = (uint32_t)slot;
    else
        workspace->candidates[workspace->candidate_count++] = (uint32_t)slot;
}
/* ---- first word: skip, narrowed scan or full scan ----------------------- */
/* With a single word, an entry without channel hits scores at most `bound`.
 * Every hit entry is pushed to the heap in one pass; if at least capacity of
 * them beat the bound, no unhit entry can enter the results and the scan is
 * skipped. Otherwise the scan follows, and seen stamps stop double pushes. */
static tl_status try_skip_scan(tl_lexical_workspace *workspace, const struct word *word,
                               size_t capacity, bool *skipped) {
    const tl_lexical *engine = workspace->engine;
    *skipped = false;
    if (word->path || workspace->subsequence_bound > INT_MAX - LEXICAL_LENGTH_BONUS_MAX)
        return TL_OK;
    int bound = workspace->subsequence_bound + LEXICAL_LENGTH_BONUS_MAX;
    size_t strong = 0;
    tl_status status =
        score_batch(workspace, word, workspace->touched, workspace->touched_count, false);
    if (status != TL_OK)
        return status;
    for (size_t i = 0; i < workspace->touched_count; i++) {
        size_t slot = workspace->touched[i];
        int score = workspace->batch[i].score;
        if (score + length_bonus(engine, slot) > bound)
            strong++;
        push_match(workspace, slot, score, false, capacity);
    }
    *skipped = strong >= capacity;
    return TL_OK;
}
/* Score directories in parent-before-child order, then union the entries of
 * matching parent nodes with the basename-mask candidates and channel hits.
 * These are conservative, complete candidates: a basename subsequence cannot
 * lack a required mask bit, and a parent match has a positive nearest score. */
static tl_status prepare_scan(tl_lexical_workspace *workspace, const struct word *word) {
    const tl_lexical *engine = workspace->engine;
    tl_status status = mask_index_query(engine->name_masks, workspace->name_masks, word->text.mask);
    if (status == TL_OK)
        status = score_directories(workspace, word->text);
    size_t nodes = dirtree_count(engine->tree);
    for (uint32_t node = 0; node < nodes && status == TL_OK; node++) {
        if (workspace->nearest[node] == 0)
            continue;
        for (uint32_t p = engine->dir_starts[node];
             p < engine->dir_starts[node + 1] && status == TL_OK; p++)
            status = mask_index_include(workspace->name_masks, engine->dir_entries[p]);
    }
    for (size_t i = 0; i < workspace->touched_count && status == TL_OK; i++)
        status = mask_index_include(workspace->name_masks, workspace->touched[i]);
    return status;
}
/* Score the first word over every entry, or only over the cached membership
 * of a word it extends, recording the new complete subsequence membership.
 * Channel hits outside a narrowed base are added separately: only
 * subsequence matching is monotone under extension. */
static tl_status scan_first(tl_lexical_workspace *workspace, const struct word *word,
                            size_t capacity) {
    const uint32_t *members = NULL;
    size_t member_count = 0, recorded = 0;
    bool narrowed =
        subseq_cache_lookup(workspace->cache, word->text, word->path, &members, &member_count);
    uint32_t *record = NULL;
    tl_status status = subseq_cache_begin(workspace->cache, word->text, word->path, &record);
    if (status != TL_OK && status != TL_LIMIT)
        return status;
    bool recording = status == TL_OK;
    bool filtered = !narrowed && !word->path;
    if (filtered) {
        status = prepare_scan(workspace, word);
        if (status != TL_OK)
            return status;
    }
    size_t count = narrowed ? member_count : workspace->engine->count;
    const uint32_t *slots = members;
    if (filtered) {
        count = 0;
        size_t slot = 0;
        while (mask_index_next(workspace->name_masks, &slot))
            workspace->candidates[count++] = (uint32_t)slot;
        slots = workspace->candidates;
    }
    status = score_batch(workspace, word, slots, count, true);
    if (status != TL_OK)
        return status;
    for (size_t k = 0; k < count; k++) {
        size_t slot = slots == NULL ? k : slots[k];
        struct entry_match match = workspace->batch[k];
        if (match.subsequence && recording)
            record[recorded++] = (uint32_t)slot;
        if (match.score > 0)
            push_match(workspace, slot, match.score, match.parent_only, capacity);
    }
    for (size_t i = 0; narrowed && i < workspace->touched_count; i++) {
        size_t slot = workspace->touched[i];
        int score = 0;
        if (workspace->seen[slot] == workspace->epoch)
            continue;
        status = entry_score(workspace, word, slot, NULL, NULL, &score);
        if (status != TL_OK)
            return status;
        push_match(workspace, slot, score, false, capacity);
    }
    return recording ? subseq_cache_commit(workspace->cache, recorded) : TL_OK;
}
/* Keep only entries of list that also match word, adding its score. */
static tl_status filter_list(tl_lexical_workspace *workspace, const struct word *word,
                             uint32_t *list, size_t *count) {
    size_t kept = 0;
    tl_status status = TL_OK;
    if (!word->path &&
        *count > dirtree_count(workspace->engine->tree) / LEXICAL_DIRECTORY_BATCH_DIVISOR)
        status = score_directories(workspace, word->text);
    if (status == TL_OK)
        status = score_batch(workspace, word, list, *count, false);
    for (size_t i = 0; i < *count && status == TL_OK; i++) {
        uint32_t slot = list[i];
        int score = workspace->batch[i].score;
        if (status == TL_OK && score == 0) {
            workspace->total[slot] = 0;
            continue;
        }
        workspace->total[slot] += score;
        list[kept++] = slot;
    }
    /* On error, keep the untouched tail so cleanup still resets its totals. */
    if (status == TL_OK)
        *count = kept;
    return status;
}
/* Filter list by every word except first. */
static tl_status filter_words(tl_lexical_workspace *workspace, size_t first, size_t words,
                              uint32_t *list, size_t *count) {
    tl_status status = TL_OK;
    for (size_t i = 0; i < words && status == TL_OK && *count != 0; i++) {
        if (i == first)
            continue;
        status = prepare_word(workspace, &workspace->words[i]);
        if (status == TL_OK)
            status = filter_list(workspace, &workspace->words[i], list, count);
    }
    return status;
}
/* Estimate basename-mask candidates plus descendants of possible matching
 * parent names. Overlaps deliberately overcount: this selects a scan order,
 * never a candidate budget. A selective cached membership can be cheaper. */
static tl_status estimate_word(tl_lexical_workspace *workspace, const struct word *word,
                               size_t *out) {
    const tl_lexical *engine = workspace->engine;
    *out = engine->count;
    if (word->path)
        return TL_OK;
    tl_status status = mask_index_query(engine->name_masks, workspace->name_masks, word->text.mask);
    if (status != TL_OK)
        return status;
    size_t count = mask_index_count(workspace->name_masks), nodes = dirtree_count(engine->tree);
    for (size_t node = 0; node < nodes && count < engine->count; node++) {
        if ((engine->dir_masks[node] & word->text.mask) == word->text.mask) {
            size_t descendants = engine->dir_descendants[node];
            count += descendants < engine->count - count ? descendants : engine->count - count;
        }
    }
    *out = count;
    return TL_OK;
}
/* A long parent word can match hundreds of thousands of descendants, while
 * a shorter basename word is selective. Use estimates and complete cached
 * membership sizes rather than preferring length or any cache unconditionally. */
static tl_status choose_first(tl_lexical_workspace *workspace, size_t words, size_t *out) {
    const uint32_t *members = NULL;
    size_t count = 0, best_count = SIZE_MAX;
    *out = 0;
    for (size_t i = 0; words > 1 && i < words; i++) {
        size_t estimate = 0;
        tl_status status = estimate_word(workspace, &workspace->words[i], &estimate);
        if (status != TL_OK)
            return status;
        bool cached = subseq_cache_lookup(workspace->cache, workspace->words[i].text,
                                          workspace->words[i].path, &members, &count);
        if (cached && count < estimate)
            estimate = count;
        bool longer = workspace->words[i].text.length > workspace->words[*out].text.length;
        if (estimate < best_count || (estimate == best_count && longer)) {
            *out = i;
            best_count = estimate;
        }
    }
    return TL_OK;
}
static bool whitespace(uint32_t symbol) {
    return symbol == ' ' || symbol == '\t' || symbol == '\n' || symbol == '\r';
}
static size_t split_words(tl_lexical_workspace *workspace, tl_text query) {
    size_t count = 0;
    for (size_t start = 0; start < query.length;) {
        if (whitespace(query.symbols[start])) {
            start++;
            continue;
        }
        struct word *word = &workspace->words[count++];
        *word = (struct word){
            .text = {.symbols = query.symbols + start, .boundaries = query.boundaries + start}};
        for (; start < query.length && !whitespace(query.symbols[start]); start++) {
            word->text.mask |= tokenize_symbol_mask(query.symbols[start]);
            word->text.length++;
            word->path = word->path || query.symbols[start] == '/';
        }
        word->repeats = lexical_repeat_mask(word->text.symbols, word->text.length);
    }
    return count;
}
/* ---- result selection ---------------------------------------------------- */
/* Total order: higher score first, then raw path bytes, then id (path rank). */
static bool worse(const tl_lexical *engine, struct heap_item a, struct heap_item b) {
    if (a.score != b.score)
        return a.score < b.score;
    return engine->columns.path_rank[a.slot] > engine->columns.path_rank[b.slot];
}
static void sift_down(tl_lexical_workspace *workspace, size_t position, size_t count) {
    struct heap_item *heap = workspace->heap;
    for (;;) {
        size_t child = 2 * position + 1;
        if (child >= count)
            return;
        if (child + 1 < count && worse(workspace->engine, heap[child + 1], heap[child]))
            child++;
        if (!worse(workspace->engine, heap[child], heap[position]))
            return;
        struct heap_item swap = heap[child];
        heap[child] = heap[position];
        heap[position] = swap;
        position = child;
    }
}
/* Min-heap keyed by worse(): the root is the weakest kept result. */
static void heap_push(tl_lexical_workspace *workspace, struct heap_item item, size_t capacity) {
    struct heap_item *heap = workspace->heap;
    if (workspace->heap_count == capacity) {
        if (!worse(workspace->engine, heap[0], item))
            return;
        heap[0] = item;
        sift_down(workspace, 0, capacity);
        return;
    }
    size_t position = workspace->heap_count++;
    while (position > 0 && worse(workspace->engine, item, heap[(position - 1) / 2])) {
        heap[position] = heap[(position - 1) / 2];
        position = (position - 1) / 2;
    }
    heap[position] = item;
}
/* Heapsort in place: repeatedly move the weakest item to the end, leaving the
 * array best-first. */
static void heap_sort(tl_lexical_workspace *workspace) {
    for (size_t count = workspace->heap_count; count > 1; count--) {
        struct heap_item swap = workspace->heap[0];
        workspace->heap[0] = workspace->heap[count - 1];
        workspace->heap[count - 1] = swap;
        sift_down(workspace, 0, count - 1);
    }
}
/* Ranks [lo, hi) of path_order whose raw path equals query exactly. */
static void exact_path_range(const tl_lexical *engine, const char *query, size_t *lo, size_t *hi) {
    const struct lexical_columns *columns = &engine->columns;
    *lo = *hi = 0;
    if (query[0] != '/')
        return;
    size_t low = 0, high = engine->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const char *path = columns->paths + columns->path_offsets[columns->path_order[mid]];
        if (strcmp(path, query) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    size_t end = low;
    while (end < engine->count &&
           strcmp(columns->paths + columns->path_offsets[columns->path_order[end]], query) == 0)
        end++;
    *lo = low;
    *hi = end;
}
/* Multiword queries: rank the entries of list that survived every word. */
static void collect(tl_lexical_workspace *workspace, const uint32_t *list, size_t count,
                    size_t capacity) {
    for (size_t i = 0; i < count; i++) {
        uint32_t slot = list[i];
        heap_push(workspace,
                  (struct heap_item){final_score(workspace, slot, workspace->total[slot]), slot},
                  capacity);
    }
}
static void collect_roots(tl_lexical_workspace *workspace, size_t capacity) {
    const tl_lexical *engine = workspace->engine;
    for (size_t i = 0; i < engine->root_count; i++)
        heap_push(workspace, (struct heap_item){LEXICAL_ROOT_SCORE, engine->columns.roots[i]},
                  capacity);
}
static void begin_query(tl_lexical_workspace *workspace, const char *raw, size_t words) {
    if (++workspace->epoch == 0) {
        /* Wrapped: clear stamps so no stale stamp equals a new epoch. */
        memset(workspace->seen, 0, workspace->engine->count * sizeof(uint32_t));
        for (size_t i = 0; i < LEXICAL_WORD_CACHES; i++)
            workspace->evidence[i].query_epoch = 0;
        workspace->epoch = 1;
    }
    workspace->direct = words == 1;
    workspace->trimmed = (tl_text){0};
    if (words != 0) {
        const struct word *first = &workspace->words[0], *last = &workspace->words[words - 1];
        workspace->trimmed.symbols = first->text.symbols;
        workspace->trimmed.length =
            (size_t)(last->text.symbols - first->text.symbols) + last->text.length;
    }
    exact_path_range(workspace->engine, raw, &workspace->exact_lo, &workspace->exact_hi);
}
/* Exact raw-path matches go first with top priority; seen keeps them from
 * being pushed again by the scan. */
static void push_exact_paths(tl_lexical_workspace *workspace, size_t capacity) {
    for (size_t rank = workspace->exact_lo; rank < workspace->exact_hi; rank++) {
        uint32_t slot = workspace->engine->columns.path_order[rank];
        workspace->seen[slot] = workspace->epoch;
        heap_push(workspace, (struct heap_item){LEXICAL_EXACT_PATH, slot}, capacity);
    }
}
struct exact_names {
    tl_lexical_workspace *workspace;
    size_t capacity;
};
static tl_status on_exact_name(void *context, size_t slot, int score) {
    struct exact_names *names = context;
    tl_lexical_workspace *workspace = names->workspace;
    (void)score;
    if (workspace->seen[slot] == workspace->epoch ||
        !exact_name(workspace->engine, slot, workspace->trimmed))
        return TL_OK;
    workspace->seen[slot] = workspace->epoch;
    heap_push(workspace, (struct heap_item){LEXICAL_EXACT_BASENAME, (uint32_t)slot},
              names->capacity);
    return TL_OK;
}
/* A multiword exact basename may contain separators inside a word (a_b), so
 * that word need not have a token-prefix hit. Preserve exact-name priority
 * before bounding entries without a channel hit for the first word. */
static tl_status push_exact_names(tl_lexical_workspace *workspace, size_t capacity) {
    struct exact_names names = {workspace, capacity};
    return prefix_query(workspace->engine->prefix, workspace->trimmed, on_exact_name, &names);
}
/* Deferred entries matched the first word only through a parent directory,
 * scoring at most max(parent prefix, fuzzy bound) for it plus the most any
 * other word can add. When the heap's weakest result already beats that, they
 * cannot enter the results and need not be filtered. */
static bool deferred_cannot_rank(const tl_lexical_workspace *workspace, size_t first, size_t words,
                                 size_t capacity) {
    if (workspace->heap_count < capacity)
        return false;
    int fuzzy = fuzzy_score_bound(workspace->words[first].text.length);
    long long bound = fuzzy > LEXICAL_PARENT_PREFIX_SCORE ? fuzzy : LEXICAL_PARENT_PREFIX_SCORE;
    for (size_t i = 0; i < words; i++) {
        if (i != first)
            bound += word_max(workspace->engine, &workspace->words[i]);
    }
    return (long long)workspace->heap[0].score > bound + LEXICAL_LENGTH_BONUS_MAX;
}
static tl_status run_multiword(tl_lexical_workspace *workspace, size_t first, size_t words,
                               size_t capacity) {
    tl_status status =
        filter_words(workspace, first, words, workspace->candidates, &workspace->candidate_count);
    if (status != TL_OK)
        return status;
    collect(workspace, workspace->candidates, workspace->candidate_count, capacity);
    if (workspace->deferred_count == 0 || deferred_cannot_rank(workspace, first, words, capacity))
        return TL_OK;
    status = filter_words(workspace, first, words, workspace->deferred, &workspace->deferred_count);
    if (status == TL_OK)
        collect(workspace, workspace->deferred, workspace->deferred_count, capacity);
    return status;
}
/* Evaluate first-word channel hits before a multiword scan. If the heap beats
 * the maximum total of an unhit entry, it is already complete. Otherwise keep
 * seen stamps for these fully evaluated hits, then scan the remaining entries.
 * Known channel maxima tighten later-word bounds without imposing a budget. */
static tl_status try_multiword_hits(tl_lexical_workspace *workspace, size_t first, size_t words,
                                    size_t capacity, bool *skipped) {
    tl_status status = score_batch(workspace, &workspace->words[first], workspace->touched,
                                   workspace->touched_count, false);
    if (status != TL_OK)
        return status;
    for (size_t i = 0; i < workspace->touched_count; i++) {
        size_t slot = workspace->touched[i];
        push_match(workspace, slot, workspace->batch[i].score, false, capacity);
    }
    status = run_multiword(workspace, first, words, capacity);
    if (status != TL_OK)
        return status;
    long long bound = subsequence_max(&workspace->words[first]) + LEXICAL_LENGTH_BONUS_MAX;
    for (size_t i = 0; i < words; i++) {
        if (i != first)
            bound += word_max(workspace->engine, &workspace->words[i]);
    }
    *skipped = workspace->heap_count == capacity && (long long)workspace->heap[0].score > bound;
    for (size_t i = 0; i < workspace->candidate_count; i++)
        workspace->total[workspace->candidates[i]] = 0;
    workspace->candidate_count = 0;
    return *skipped ? TL_OK : prepare_word(workspace, &workspace->words[first]);
}
static tl_status run_query(tl_lexical_workspace *workspace, const char *raw, size_t words,
                           size_t capacity) {
    begin_query(workspace, raw, words);
    if (words == 0) {
        collect_roots(workspace, capacity);
        return TL_OK;
    }
    push_exact_paths(workspace, capacity);
    size_t first = 0;
    bool skipped = false;
    tl_status status = choose_first(workspace, words, &first);
    if (status == TL_OK)
        status = prepare_word(workspace, &workspace->words[first]);
    if (status == TL_OK && words > 1)
        status = push_exact_names(workspace, capacity);
    if (status == TL_OK && words == 1)
        status = try_skip_scan(workspace, &workspace->words[first], capacity, &skipped);
    if (status == TL_OK && words > 1 && !workspace->words[first].path)
        status = try_multiword_hits(workspace, first, words, capacity, &skipped);
    if (status == TL_OK && !skipped)
        status = scan_first(workspace, &workspace->words[first], capacity);
    if (status == TL_OK && !skipped && !workspace->direct)
        status = run_multiword(workspace, first, words, capacity);
    return status;
}
/* Leave hit/total arrays all zero so the next query starts clean. */
static void reset_workspace(tl_lexical_workspace *workspace) {
    for (size_t i = 0; i < LEXICAL_WORD_CACHES; i++)
        reset_hits(&workspace->evidence[i]);
    workspace->touched_count = 0;
    for (size_t i = 0; i < workspace->candidate_count; i++)
        workspace->total[workspace->candidates[i]] = 0;
    for (size_t i = 0; i < workspace->deferred_count; i++)
        workspace->total[workspace->deferred[i]] = 0;
    workspace->candidate_count = workspace->deferred_count = 0;
    workspace->heap_count = 0;
}
tl_status lexical_query(const tl_lexical *engine, tl_lexical_workspace *workspace,
                        const char *query, tl_result *results, size_t capacity, size_t *out_count) {
    if (out_count == NULL)
        return TL_INVALID;
    *out_count = 0;
    if (engine == NULL || workspace == NULL || workspace->engine != engine || query == NULL ||
        results == NULL || capacity == 0)
        return TL_INVALID;
    if (!engine->finished)
        return TL_STATE;
    if (capacity > LEXICAL_MAX_RESULTS)
        return TL_LIMIT;
    size_t length = strnlen(query, LEXICAL_QUERY_BYTES + 1);
    if (length > LEXICAL_QUERY_BYTES)
        return TL_LIMIT;
    tl_text text = {0};
    tl_status status = tokenize_into(query, length, workspace->symbols, workspace->boundaries,
                                     workspace->offsets, LEXICAL_QUERY_SYMBOLS, &text);
    size_t words = status == TL_OK ? split_words(workspace, text) : 0, cached = 0;
    if (status == TL_OK && engine->symbols_ready && words == 1 &&
        workspace->words[0].text.length == 1 &&
        lexical_symbol_index(workspace->words[0].text.symbols[0], &cached)) {
        /* Results are totally ordered, so the first capacity of the cached
         * top LEXICAL_MAX_RESULTS are exactly this query's answer. */
        size_t count =
            engine->symbol_counts[cached] < capacity ? engine->symbol_counts[cached] : capacity;
        memcpy(results, engine->symbol_results + cached * LEXICAL_MAX_RESULTS,
               count * sizeof(tl_result));
        *out_count = count;
        return TL_OK;
    }
    if (status == TL_OK)
        status = run_query(workspace, query, words, capacity);
    if (status == TL_OK) {
        heap_sort(workspace);
        const struct lexical_columns *columns = &engine->columns;
        for (size_t i = 0; i < workspace->heap_count; i++) {
            uint32_t slot = workspace->heap[i].slot;
            results[i] =
                (tl_result){columns->ids[slot], columns->paths + columns->path_offsets[slot],
                            workspace->heap[i].score};
        }
        *out_count = workspace->heap_count;
    }
    reset_workspace(workspace);
    return status;
}
