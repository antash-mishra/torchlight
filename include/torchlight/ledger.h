/* Persisted-state map (the ledger, M7 Phase 2, ADR 0040): for every committed
 * catalog row, a fingerprint of its path and the store signature of its
 * columns, so a scan can tell unchanged entries and vanished rows apart
 * without asking SQLite. Paths are not stored: two independent 64-bit hashes
 * identify a path, about 60 bytes per row in all. */
#ifndef TORCHLIGHT_LEDGER_H
#define TORCHLIGHT_LEDGER_H
#include "torchlight/common.h"
#include "torchlight/vec.h"
#include <stdbool.h>
typedef struct tl_ledger tl_ledger;
/** Create an owned, empty and incomplete ledger (see ledger_complete).
 * out NULL on TL_INVALID/TL_NOMEM. */
tl_status ledger_create(tl_ledger **out);
/** Free the ledger; NULL allowed, no errors. */
void ledger_destroy(tl_ledger *ledger);
/** Forget every row and mark the ledger incomplete until ledger_seal. NULL
 * allowed; no errors. */
void ledger_reset(tl_ledger *ledger);
/** Record committed row id at path (borrowed, hashed and not kept) with its
 * store signature, replacing what an existing record of id held. A new id
 * must exceed every recorded id (SQLite AUTOINCREMENT ids only grow): a
 * smaller one returns TL_STATE. TL_INVALID for NULL or id 0, TL_NOMEM and
 * TL_LIMIT. Any failure marks the ledger incomplete. */
tl_status ledger_put(tl_ledger *ledger, uint64_t id, const char *path, uint64_t signature);
/** Forget row id (deleted, or about to be reloaded after a commit); no-op when
 * absent. May compact storage. NULL allowed; no errors. */
void ledger_remove(tl_ledger *ledger, uint64_t id);
/** Declare the ledger complete (it holds every committed row) and release
 * spare capacity. A failed ledger_put since ledger_reset keeps it incomplete.
 * NULL allowed; no errors. */
void ledger_seal(tl_ledger *ledger);
/** Mark the ledger incomplete: rows may be missing or stale, so scans must not
 * rely on it until it is rebuilt. NULL allowed; no errors. */
void ledger_invalidate(tl_ledger *ledger);
/** Whether the ledger is sealed and nothing invalidated it since. false for
 * NULL. */
bool ledger_complete(const tl_ledger *ledger);
/** Start a scan: clear every seen mark. TL_INVALID for NULL, TL_NOMEM. */
tl_status ledger_begin(tl_ledger *ledger);
/** Look up the row at path (borrowed): on a hit write its id and signature,
 * mark it seen since ledger_begin and return true. false for a miss or NULL
 * arguments. No allocation. */
bool ledger_find(tl_ledger *ledger, const char *path, uint64_t *id, uint64_t *signature);
/** Append to ids (a vec of uint64_t) the id of every row not seen since
 * ledger_begin, ascending. TL_INVALID for NULL, TL_NOMEM (ids may hold a
 * prefix then). */
tl_status ledger_unseen(const tl_ledger *ledger, tl_vec *ids);
/** Whether a record of id exists. false for NULL. No allocation. */
bool ledger_contains(const tl_ledger *ledger, uint64_t id);
/** Number of recorded rows; zero for NULL. */
size_t ledger_count(const tl_ledger *ledger);
#endif
