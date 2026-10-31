/* Swappable background available-software discovery, initially Flatpak. */
#ifndef TORCHLIGHT_SOFTWARE_BACKEND_H
#define TORCHLIGHT_SOFTWARE_BACKEND_H
#include "torchlight/process.h"
#include "torchlight/software.h"
typedef struct {
    void *context;
    /* Build an owned sealed index; TL_STATE means no configured backend/sources.
     * Other errors retain the previous catalog. out NULL on any error. */
    tl_status (*build)(void *context, bool download, tl_process_budget *budget, tl_software **out);
    /* Cheap source/install change stamp. Worker only; no spawning/network. */
    uint64_t (*stamp)(void *context);
} tl_software_backend;
/** Return the Flatpak adapter by value; no owned state or errors. Its callbacks
 * use the stable process environment. Call only on a background worker. */
tl_software_backend software_backend_flatpak(void);
/** Revalidate an exact source revision and remote ref before a client handoff.
 * Background/action worker only. Borrowed arguments, no ownership transfer.
 * TL_STATE for replaced/disabled/missing sources or refs; process errors as in
 * process_capture. Never adds remotes, installs packages or opens applications. */
tl_status software_backend_validate(const tl_software_ref *ref, uint64_t source_revision,
                                    tl_process_budget *budget);
#endif
