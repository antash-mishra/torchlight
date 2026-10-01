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
/** Copy path into builder with monotonically increasing nonzero id and indexed-root flag.
 * TL_STATE after finish, TL_INVALID for empty path or non-increasing id, plus allocation
 * errors. A channel-build failure poisons the builder: discard it. Other errors
 * leave previously added entries usable. Paths need not be unique at this layer. */
tl_status lexical_add(tl_lexical *engine, uint64_t id, const char *path, bool is_root);
/** Seal builder for immutable queries; TL_INVALID/STATE on lifecycle errors. */
tl_status lexical_finish(tl_lexical *engine);
/** Create scratch for this sealed engine, owned by caller until destroy. Engine
 * must outlive workspace. TL_INVALID/STATE/NOMEM/LIMIT; out NULL on failure.
 * Separate workspaces permit concurrent queries without shared mutable state. */
tl_status lexical_workspace_create(const tl_lexical *engine, tl_lexical_workspace **out);
/** Free scratch, leaving engine untouched; NULL allowed. */
void lexical_workspace_destroy(tl_lexical_workspace *workspace);
/** Query sealed engine with its workspace. Copy at most capacity results to
 * caller buffer (1..LEXICAL_MAX_RESULTS); out_count is zero on error. Paths are
 * borrowed until engine destruction. Query <= LEXICAL_QUERY_BYTES raw non-NUL
 * bytes. Empty/space-only queries return indexed roots. Every nonempty word
 * must match the basename or a single parent directory name; a word containing
 * '/' may match across the full path. Exact raw paths/basenames have priority.
 * No I/O/heap allocation.
 * TL_INVALID/STATE/LIMIT on contract violations; scratch is reusable on failure. */
tl_status lexical_query(const tl_lexical *engine, tl_lexical_workspace *workspace,
                        const char *query, tl_result *results, size_t capacity, size_t *out_count);
/** Return number of entries; zero for NULL, no errors. */
size_t lexical_count(const tl_lexical *engine);
#endif
