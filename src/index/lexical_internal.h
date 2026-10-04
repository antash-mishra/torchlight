/* Engine layout shared by lexical.c (construction) and lexical_query.c
 * (search). Internal to the lexical module; not a public header. */
#ifndef TORCHLIGHT_LEXICAL_INTERNAL_H
#define TORCHLIGHT_LEXICAL_INTERNAL_H
#include "torchlight/dirtree.h"
#include "torchlight/lexical.h"
#include "torchlight/mask.h"
#include "torchlight/prefix.h"
#include "torchlight/trigram.h"
#include "torchlight/typo.h"
#include "torchlight/vec.h"
/* Read-only views of the per-entry columns, valid once the engine is sealed.
 * Entries are stored as a struct of arrays so that scans touch only the
 * columns they read (masks and directory links for most entries). */
struct lexical_columns {
    const uint64_t *ids;
    const uint64_t *masks;        /* basename symbol mask */
    const uint64_t *repeats;      /* mask bits of symbols occurring twice or more */
    const uint64_t *contexts;     /* basename mask | every ancestor directory's path mask */
    const uint32_t *path_offsets; /* into paths: exact raw bytes, NUL-terminated */
    const uint32_t *name_offsets; /* into symbols/boundaries: normalized basename */
    const uint32_t *name_lengths; /* basename symbol count */
    const uint32_t *dirs;         /* parent directory node in the dirtree */
    const uint32_t *path_order;   /* slots sorted by raw path bytes, then id */
    const uint32_t *path_rank;    /* inverse of path_order: deterministic tie-break */
    const uint32_t *roots;        /* slots flagged as indexed roots */
    const char *paths;
    const uint32_t *symbols;
    const uint8_t *boundaries;
};
/* One-symbol queries over [a-z0-9] start every typed query and match the
 * most entries, so their best results are precomputed when the engine is
 * sealed. The engine is immutable, so these answers never go stale. */
#define LEXICAL_SYMBOL_QUERIES 36
struct tl_lexical {
    tl_vec *directory_ids;
    tl_vec *ids, *masks, *repeats, *path_offsets, *name_offsets, *name_lengths, *dirs, *roots,
        *paths, *symbols, *boundaries, *scratch_symbols, *scratch_boundaries, *scratch_offsets;
    tl_dirtree *tree;
    /* usable[node] is 0 for directories above every indexed root (e.g. /home
     * for root /home/user): they say nothing about where a file is. */
    uint8_t *usable;
    uint32_t *path_order, *path_rank;
    uint64_t *contexts;
    tl_mask_index *name_masks;
    /* Parent directory -> entry slots, so matching directory context can be
     * unioned with basename mask candidates without scanning every entry. */
    uint32_t *dir_starts, *dir_entries;
    uint64_t *dir_masks;
    uint32_t *dir_descendants;
    tl_prefix *prefix, *dir_prefix;
    tl_trigram *trigram;
    tl_typo *typo;
    struct lexical_columns columns;
    /* symbol_results[i * LEXICAL_MAX_RESULTS ...] answer query
     * lexical_symbol_query(i); valid once symbols_ready. */
    tl_result *symbol_results;
    size_t symbol_counts[LEXICAL_SYMBOL_QUERIES];
    int prefix_bonus;
    bool symbols_ready;
    size_t count, root_count, max_path_symbols;
    bool finished, failed;
};
/** Return the normalized symbol whose results are cached at index
 * (0..LEXICAL_SYMBOL_QUERIES-1): 'a'..'z', then '0'..'9'. */
static inline uint32_t lexical_symbol_query(size_t index) {
    return index < 26 ? (uint32_t)('a' + index) : (uint32_t)('0' + index - 26);
}
/** Inverse of lexical_symbol_query: false when symbol has no cached results. */
static inline bool lexical_symbol_index(uint32_t symbol, size_t *index) {
    if (symbol >= 'a' && symbol <= 'z')
        *index = symbol - 'a';
    else if (symbol >= '0' && symbol <= '9')
        *index = 26 + symbol - '0';
    else
        return false;
    return true;
}
/** Mask bits of symbols that occur at least twice in text. A word with a
 * repeated symbol can only be a subsequence of text whose repeat mask has
 * that bit; hash collisions only admit extra candidates. */
static inline uint64_t lexical_repeat_mask(const uint32_t *symbols, size_t length) {
    uint64_t seen = 0, repeated = 0;
    for (size_t i = 0; i < length; i++) {
        uint64_t bit = tokenize_symbol_mask(symbols[i]);
        repeated |= seen & bit;
        seen |= bit;
    }
    return repeated;
}
/** Borrow the normalized basename view of slot in a sealed engine. */
static inline tl_text lexical_name(const tl_lexical *engine, size_t slot) {
    const struct lexical_columns *columns = &engine->columns;
    tl_text text = {.symbols = columns->symbols + columns->name_offsets[slot],
                    .boundaries = columns->boundaries + columns->name_offsets[slot],
                    .length = columns->name_lengths[slot],
                    .mask = columns->masks[slot]};
    return text;
}
#endif
