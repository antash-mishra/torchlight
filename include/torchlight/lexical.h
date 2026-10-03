/* Immutable lexical engine and caller-owned bounded per-query workspace. */
#ifndef TORCHLIGHT_LEXICAL_H
#define TORCHLIGHT_LEXICAL_H
#include "torchlight/common.h"
#include <stdbool.h>
#define LEXICAL_QUERY_BYTES 256
#define LEXICAL_MAX_RESULTS 1000
typedef struct tl_lexical tl_lexical;
typedef struct tl_lexical_workspace tl_lexical_workspace;
typedef struct {
    uint64_t id;
    const char *path;
    int score;
} tl_result;
/** Create owned builder; out NULL on invalid/allocation failure. */
tl_status lexical_create(tl_lexical **out);
/** Free engine and all paths; workspaces must no longer be used. NULL allowed. */
void lexical_destroy(tl_lexical *engine);
/** Copy an absolute path into the builder with a monotonically increasing
 * nonzero id and an indexed-root flag. TL_STATE after finish or after a
 * failure; TL_INVALID for empty/relative paths or non-increasing ids (the
 * builder stays usable); allocation/limit errors poison the builder, so
 * discard it. Paths need not be unique at this layer. */
tl_status lexical_add(tl_lexical *engine, uint64_t id, const char *path, bool is_root);
/** Add path with immutable directory metadata. Same lifetime/errors and ordered
 * id requirements as lexical_add; metadata never affects matching/ranking. */
tl_status lexical_add_entry(tl_lexical *engine, uint64_t id, const char *path, bool is_root,
                            bool is_dir);
/** Read directory metadata for sealed engine/id; false if unknown/unmarked.
 * No allocation/I/O, engine must remain alive. Legacy lexical_add marks no dirs. */
bool lexical_is_dir(const tl_lexical *engine, uint64_t id);
/** Build all channels and seal the engine for immutable queries.
 * TL_INVALID/TL_STATE on lifecycle errors, TL_NOMEM/TL_LIMIT/TL_IO (discard). */
tl_status lexical_finish(tl_lexical *engine);
/** Create scratch for this sealed engine, owned by caller until destroy. Engine
 * must outlive workspace. TL_INVALID/STATE/NOMEM/LIMIT/IO; out NULL on failure.
 * Separate workspaces permit concurrent queries without shared mutable state.
 * A workspace remembers the last word's complete subsequence membership to
 * narrow the next query that extends it; results never depend on that cache.
 * Large-engine workspaces own a fixed worker pool created here. One coordinator per
 * workspace; its workers score disjoint batches against read-only context. */
tl_status lexical_workspace_create(const tl_lexical *engine, tl_lexical_workspace **out);
/** Join owned workers and free scratch, leaving engine untouched; NULL allowed.
 * Finish all queries before destroying this workspace. */
void lexical_workspace_destroy(tl_lexical_workspace *workspace);
/** Query sealed engine with its workspace. Copy at most capacity results to
 * caller buffer (1..LEXICAL_MAX_RESULTS); out_count is zero on error. Paths are
 * borrowed until engine destruction. Query <= LEXICAL_QUERY_BYTES raw non-NUL
 * bytes. Empty/space-only queries return indexed roots. Every whitespace-
 * separated word must match, through any channel: a basename prefix, token or
 * initials; a basename subsequence; a one-edit basename token typo; enough
 * shared basename trigrams; or a prefix/subsequence within one parent
 * directory name below the indexed roots. A word containing '/' may instead
 * match across the full path. Exact raw paths, then exact basenames, have
 * priority. Ties order by raw path bytes, then id. No I/O/heap allocation.
 * TL_INVALID/STATE/LIMIT on contract violations; scratch is reusable on failure. */
tl_status lexical_query(const tl_lexical *engine, tl_lexical_workspace *workspace,
                        const char *query, tl_result *results, size_t capacity, size_t *out_count);
/** Return number of entries; zero for NULL, no errors. */
size_t lexical_count(const tl_lexical *engine);
/** Resolve a nonzero file id in a sealed engine by binary search. Borrow exact
 * raw path until engine destruction, out NULL on error. TL_INVALID for NULL/0,
 * TL_STATE for unsealed engine or absent id. No I/O/heap allocation. */
tl_status lexical_resolve(const tl_lexical *engine, uint64_t id, const char **out);
#endif
