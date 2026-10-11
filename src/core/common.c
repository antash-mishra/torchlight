/* Shared error descriptions, checked size arithmetic and thread names. */
#include "torchlight/common.h"
#include <pthread.h>
#include <string.h>
/* Linux keeps 15 name bytes plus the terminator. */
#define THREAD_NAME_BYTES 16
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
void tl_thread_name(const char *name) {
    if (name == NULL)
        return;
    char truncated[THREAD_NAME_BYTES];
    size_t length = strnlen(name, THREAD_NAME_BYTES - 1);
    memcpy(truncated, name, length);
    truncated[length] = 0;
    /* A name only helps diagnostics; failing to set one changes nothing else. */
    int named = pthread_setname_np(pthread_self(), truncated);
    (void)named;
}
