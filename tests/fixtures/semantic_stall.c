/* Inject one slow native query to verify cancellation keeps inference memory live. */
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utf8proc.h>

/** Preserve real Unicode decomposition; consume a test marker and stall once
 * when this thread normalizes "bill". Pointers are borrowed; retain the real
 * return/errors. Mutable state is thread-local and confined to this test shim. */
utf8proc_ssize_t utf8proc_decompose_char(utf8proc_int32_t codepoint, utf8proc_int32_t *destination,
                                         utf8proc_ssize_t capacity, utf8proc_option_t options,
                                         int *last_boundclass) {
    typedef utf8proc_ssize_t (*decompose_function)(utf8proc_int32_t, utf8proc_int32_t *,
                                                   utf8proc_ssize_t, utf8proc_option_t, int *);
    void *symbol = dlsym(RTLD_NEXT, "utf8proc_decompose_char");
    decompose_function decompose = NULL;
    _Static_assert(sizeof(decompose) == sizeof(symbol), "ELF function pointer size");
    memcpy(&decompose, &symbol, sizeof(decompose));
    if (decompose == NULL)
        return UTF8PROC_ERROR_INVALIDUTF8;
    static _Thread_local size_t matched;
    static const char WORD[] = "bill";
    matched = codepoint == WORD[matched] ? matched + 1 : codepoint == 'b' ? 1 : 0;
    if (matched == sizeof(WORD) - 1) {
        matched = 0;
        const char *marker = getenv("TORCHLIGHT_TEST_SEMANTIC_STALL");
        if (marker != NULL && unlink(marker) == 0) {
            struct timespec delay = {0, 300000000};
            while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
            }
        }
    }
    return decompose(codepoint, destination, capacity, options, last_boundclass);
}
