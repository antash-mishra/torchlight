/* Shared error descriptions and checked size arithmetic. */
#include "torchlight/common.h"
const char *tl_status_string(tl_status status) {
    switch (status) {
    case TL_OK:
        return "success";
    case TL_INVALID:
        return "invalid argument";
    case TL_NOMEM:
        return "out of memory";
    case TL_IO:
        return "I/O failure";
    case TL_LIMIT:
        return "resource limit exceeded";
    case TL_STATE:
        return "invalid lifecycle state";
    case TL_CANCELLED:
        return "cancelled";
    }
    return "unknown status";
}
tl_status tl_size_multiply(size_t count, size_t size, size_t *out) {
    if (out == NULL)
        return TL_INVALID;
    if (size != 0 && count > SIZE_MAX / size)
        return TL_LIMIT;
    *out = count * size;
    return TL_OK;
}
