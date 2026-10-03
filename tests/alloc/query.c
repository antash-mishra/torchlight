/* Interpose the platform allocator to detect library/indirect query allocation.
 * Separate from ASan: its allocator interposition would mask the libc path. */
#include "torchlight/lexical.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* glibc's underlying entry points avoid recursive dlsym allocation. The
 * supported reference platform uses glibc; other libcs require an adapter. */
extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *pointer, size_t size);
extern void __libc_free(void *pointer);
static atomic_bool probing;
static atomic_size_t allocations;

/** Interpose malloc; count calls while a query is active, forwarding ownership. */
void *malloc(size_t size) {
    if (atomic_load(&probing))
        atomic_fetch_add(&allocations, 1);
    return __libc_malloc(size);
}
/** Interpose calloc with the same tracking/lifetime as malloc. */
void *calloc(size_t count, size_t size) {
    if (atomic_load(&probing))
        atomic_fetch_add(&allocations, 1);
    return __libc_calloc(count, size);
}
/** Interpose realloc; ownership and failure behavior remain libc's. */
void *realloc(void *pointer, size_t size) {
    if (atomic_load(&probing))
        atomic_fetch_add(&allocations, 1);
    return __libc_realloc(pointer, size);
}
/** Forward free to the allocator paired with the interposed allocation calls. */
void free(void *pointer) {
    __libc_free(pointer);
}

static tl_status check_query(const tl_lexical *engine, tl_lexical_workspace *workspace,
                             const char *query) {
    tl_result results[10];
    size_t count = 0;
    atomic_store(&allocations, 0);
    atomic_store(&probing, true);
    tl_status status = lexical_query(engine, workspace, query, results, 10, &count);
    atomic_store(&probing, false);
    if (atomic_load(&allocations) != 0) {
        fprintf(stderr, "query of %zu bytes allocated %zu times\n", strlen(query),
                atomic_load(&allocations));
        return TL_STATE;
    }
    return status;
}

static tl_status check_lengths(const tl_lexical *engine, tl_lexical_workspace *workspace) {
    const size_t lengths[] = {1, 2, 3, 128, 130, 131, LEXICAL_QUERY_BYTES};
    char query[LEXICAL_QUERY_BYTES + 1];
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        for (size_t j = 0; j < lengths[i]; j++)
            query[j] = (char)('a' + j % 26);
        query[lengths[i]] = 0;
        tl_status status = check_query(engine, workspace, query);
        if (status != TL_OK)
            return status;
    }
    const char *cases[] = {"", "RAEDME", "root notes", "caf\xc3\xa9", "caf\x65\xcc\x81"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        tl_status status = check_query(engine, workspace, cases[i]);
        if (status != TL_OK)
            return status;
    }
    return TL_OK;
}
/* Exercise worker dispatch as well as the small-engine serial path. Repeated
 * tokens keep fixture construction cheap; every file matches the abbreviation. */
static tl_status check_workers(void) {
    enum { ALLOCATION_CORPUS_ENTRIES = 100000 };
    tl_lexical *engine = NULL;
    tl_lexical_workspace *workspace = NULL;
    tl_status status = lexical_create(&engine);
    for (size_t i = 0; i < ALLOCATION_CORPUS_ENTRIES && status == TL_OK; i++) {
        char path[64];
        int length = snprintf(path, sizeof(path), "/root/mad_notes_%zu.txt", i);
        status = length < 0 || (size_t)length >= sizeof(path)
                     ? TL_LIMIT
                     : lexical_add(engine, i + 1, path, false);
    }
    if (status == TL_OK)
        status = lexical_finish(engine);
    if (status == TL_OK)
        status = lexical_workspace_create(engine, &workspace);
    const char *queries[] = {"ao", "root ao", "md", "mad noets", "mad", "notes", "root", "x z"};
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]) && status == TL_OK; i++)
        status = check_query(engine, workspace, queries[i]);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
    return status;
}

int main(void) {
    tl_lexical *engine = NULL;
    tl_lexical_workspace *workspace = NULL;
    tl_status status = lexical_create(&engine);
    const char *paths[] = {"/root/abcdefghijklmnopqrstuvwxyz.txt", "/root/projectNotes.md",
                           "/root/README.md", "/root/Caf\xc3\xa9.pdf", "/root/bad\xff.txt"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && status == TL_OK; i++)
        status = lexical_add(engine, i + 1, paths[i], false);
    if (status == TL_OK)
        status = lexical_finish(engine);
    if (status == TL_OK)
        status = lexical_workspace_create(engine, &workspace);
    if (status == TL_OK)
        status = check_lengths(engine, workspace);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
    if (status == TL_OK)
        status = check_workers();
    if (status == TL_OK)
        puts("Allocation-free query checks passed.");
    return status == TL_OK ? 0 : 1;
}
