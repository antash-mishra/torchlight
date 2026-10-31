/* Immutable available-software metadata and allocation-free lexical retrieval. */
#ifndef TORCHLIGHT_SOFTWARE_H
#define TORCHLIGHT_SOFTWARE_H
#include "torchlight/lexical.h"
#include <string.h>
#define SOFTWARE_ID_BYTES 256
#define SOFTWARE_PART_BYTES 128
#define SOFTWARE_ARCH_BYTES 32
#define SOFTWARE_NAME_BYTES 256
#define SOFTWARE_SUMMARY_BYTES 1024
#define SOFTWARE_MAX_ENTRIES 50000
#define SOFTWARE_SUGGESTIONS 3
#define SOFTWARE_TEXT_BYTES (32U * 1024U * 1024U)
typedef struct {
    char installation[SOFTWARE_PART_BYTES], remote[SOFTWARE_PART_BYTES];
    char app_id[SOFTWARE_ID_BYTES], arch[SOFTWARE_ARCH_BYTES], branch[SOFTWARE_PART_BYTES];
} tl_software_ref;
typedef struct {
    tl_software_ref ref;
    const char *name, *summary;
    uint64_t source_revision, revision;
    bool installed;
} tl_software_entry;
typedef struct tl_software tl_software;
typedef struct tl_software_reader tl_software_reader;
/** Validate bounded Flatpak identity fields, including user/system/named scope.
 * No I/O/allocation; borrowed NULL or malformed references return false. */
bool software_ref_valid(const tl_software_ref *ref);
/** Compare all identity fields; NULL false. No allocation or ownership transfer. */
static inline bool software_ref_equal(const tl_software_ref *a, const tl_software_ref *b) {
    return a != NULL && b != NULL && strcmp(a->installation, b->installation) == 0 &&
           strcmp(a->remote, b->remote) == 0 && strcmp(a->app_id, b->app_id) == 0 &&
           strcmp(a->arch, b->arch) == 0 && strcmp(a->branch, b->branch) == 0;
}
/** Stable hash of full identity, for UI lookup only: equality must also compare
 * the complete reference. Zero for NULL; no allocation or ownership transfer. */
uint64_t software_ref_hash(const tl_software_ref *ref);
/** Create an owned empty software builder; out NULL on TL_INVALID/NOMEM. */
tl_status software_create(tl_software **out);
/** Copy one valid entry into the builder. TL_INVALID/STATE/LIMIT/NOMEM. On an
 * allocation/build error discard the builder. Duplicate references are rejected
 * at finish. Source text must be UTF-8, name nonempty; revision is computed. */
tl_status software_add(tl_software *software, const tl_software_entry *entry);
/** Seal a builder, validating identity uniqueness and preparing the lexical
 * engine. TL_INVALID/STATE/NOMEM/LIMIT; discard on error. No borrowed inputs. */
tl_status software_finish(tl_software *software);
/** Free owned software; all readers must already be destroyed. NULL allowed. */
void software_destroy(tl_software *software);
/** Create owned scratch for a sealed index. Index must outlive it; one query
 * coordinator per reader. TL_INVALID/STATE/NOMEM/LIMIT; out NULL on error. */
tl_status software_reader_create(const tl_software *software, tl_software_reader **out);
/** Free reader scratch; NULL allowed; its borrowed index stays owned by caller. */
void software_reader_destroy(tl_software_reader *reader);
/** Search in resident memory into caller pointers (capacity 1..1000), borrowed
 * until index destruction. Empty query returns no results. No I/O or subprocesses; only the lexical
 * engine’s bounded new-word matcher compilation may allocate, while warm queries allocate nothing.
 * Optional cancellation flag borrowed for this call only. TL_INVALID/LIMIT/CANCELLED; count zero on
 * error. */
tl_status software_query(tl_software_reader *reader, const char *query, const atomic_bool *cancel,
                         const tl_software_entry **out, size_t capacity, size_t *count);
/** Suggest distinct, uninstalled apps whose names match every query word by
 * word start or initials. Unicode normalized; exact names come first. Broad fuzzy
 * and description-only matches are excluded. Installed IDs in the bounded
 * candidate pool also suppress other sources of that ID.
 * Same lease/ownership/errors as software_query, capacity 1..3. first_exact is
 * false on errors/empty results. No I/O; borrowed cancel is optional. */
tl_status software_suggest(tl_software_reader *reader, const char *query, const atomic_bool *cancel,
                           const tl_software_entry **out, size_t capacity, size_t *count,
                           bool *first_exact);
/** Resolve an exact identity and revision in resident memory. Borrowed result
 * survives until index destruction; NULL for absent/stale/invalid identity. */
const tl_software_entry *software_resolve(const tl_software *software, const tl_software_ref *ref,
                                          uint64_t revision);
/** Entry count, zero for NULL; no errors or ownership changes. */
size_t software_count(const tl_software *software);
#endif
