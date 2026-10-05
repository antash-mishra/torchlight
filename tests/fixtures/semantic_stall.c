/* Inject slow inference and cache contention to verify semantic job/build lifetimes. */
#include <dlfcn.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utf8proc.h>

/** Preserve real statement execution, except cache BEGIN while a busy marker
 * exists. The borrowed statement stays caller-owned; return SQLITE_BUSY without
 * acquiring a transaction for that injected failure. Errors otherwise propagate.
 * Thread-local identification confines the fault to the semantic worker. */
int sqlite3_step(sqlite3_stmt *statement) {
    typedef int (*step_function)(sqlite3_stmt *);
    void *symbol = dlsym(RTLD_NEXT, "sqlite3_step");
    step_function step = NULL;
    _Static_assert(sizeof(step) == sizeof(symbol), "ELF function pointer size");
    memcpy(&step, &symbol, sizeof(step));
    if (step == NULL)
        return SQLITE_ERROR;
    const char *sql = sqlite3_sql(statement);
    static _Thread_local bool cache_thread;
    /* Cache lookups identify this worker; schema setup also names cache tables
     * on the ordinary catalog writer and must not mark that thread. */
    if (sql != NULL && strstr(sql, "FROM embedding_cache") != NULL)
        cache_thread = true;
    if (cache_thread && sql != NULL && strcmp(sql, "BEGIN IMMEDIATE") == 0) {
        const char *marker = getenv("TORCHLIGHT_TEST_SEMANTIC_BUSY");
        if (marker != NULL && access(marker, F_OK) == 0)
            return SQLITE_BUSY;
    }
    return step(statement);
}

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
