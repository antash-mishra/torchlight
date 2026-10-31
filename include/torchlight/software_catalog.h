/* Resident available-software snapshots, refreshed by one bounded worker. */
#ifndef TORCHLIGHT_SOFTWARE_CATALOG_H
#define TORCHLIGHT_SOFTWARE_CATALOG_H
#include "torchlight/software_backend.h"
typedef struct tl_software_catalog tl_software_catalog;
typedef enum {
    SOFTWARE_DISABLED,
    SOFTWARE_LOADING,
    SOFTWARE_READY,
    SOFTWARE_UNAVAILABLE,
    SOFTWARE_FAILED,
    SOFTWARE_STALE
} tl_software_state;
/** Create an owned catalog/worker. Optional backend is copied; its context must
 * outlive destroy. Disabled catalogs start no thread. TL_INVALID/NOMEM/IO;
 * out NULL on error. Initial discovery is asynchronous. */
tl_status software_catalog_create(bool enabled, const tl_software_backend *backend,
                                  tl_software_catalog **out);
/** Cancel, join and free catalog after releasing leases; NULL allowed. */
void software_catalog_destroy(tl_software_catalog *catalog);
/** Acquire/release a short exclusive query lease. No allocation/I/O. A single
 * coordinator may search on a lease. Entry pointers borrow the lease. */
void software_catalog_acquire(tl_software_catalog *catalog);
void software_catalog_release(tl_software_catalog *catalog);
/** Query current snapshot under a lease; same contract as software_query.
 * Empty/unready catalog returns TL_OK with zero entries, never blocks on work. */
tl_status software_catalog_query(tl_software_catalog *catalog, const char *query,
                                 const atomic_bool *cancel, const tl_software_entry **out,
                                 size_t capacity, size_t *count);
/** Name-only suggestions under a lease; same contract as software_suggest.
 * Empty/unready snapshots return zero entries and first_exact=false. */
tl_status software_catalog_suggest(tl_software_catalog *catalog, const char *query,
                                   const atomic_bool *cancel, const tl_software_entry **out,
                                   size_t capacity, size_t *count, bool *first_exact);
/** Resolve an exact current identity/revision under a lease; NULL when stale.
 * Client still revalidates the external source before handoff. No I/O. */
const tl_software_entry *software_catalog_resolve(const tl_software_catalog *catalog,
                                                  const tl_software_ref *ref, uint64_t revision);
/** Read state/count under a lease; disabled/zero for NULL. No errors or I/O. */
tl_software_state software_catalog_state(const tl_software_catalog *catalog);
size_t software_catalog_count(const tl_software_catalog *catalog);
/** Read snapshot sequence under a lease; changes only on publication. Zero for
 * NULL/unpublished. No errors, ownership transfer, allocation or I/O. */
uint64_t software_catalog_gen(const tl_software_catalog *catalog);
/** Static lowercase state name; unknown for invalid enum. No ownership. */
const char *software_catalog_state_name(tl_software_state state);
/** Request one coalesced download/refresh; thread-safe, no I/O. Requests while
 * a refresh is active coalesce into it. Disabled catalogs ignore requests. */
void software_catalog_refresh(tl_software_catalog *catalog);
#endif
