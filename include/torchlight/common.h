/* Shared status codes and bounded allocation helpers. */
#ifndef TORCHLIGHT_COMMON_H
#define TORCHLIGHT_COMMON_H
#include <stddef.h>
#include <stdint.h>
typedef enum { TL_OK, TL_INVALID, TL_NOMEM, TL_IO, TL_LIMIT, TL_STATE } tl_status;
/** Return a static description of status; no ownership transfer or errors. */
const char *tl_status_string(tl_status status);
/** Check count * size, returning TL_LIMIT on overflow, TL_INVALID for NULL out. */
tl_status tl_size_multiply(size_t count, size_t size, size_t *out);
#endif
