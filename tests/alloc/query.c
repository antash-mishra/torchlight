/* Interpose the platform allocator to detect library/indirect query allocation.
 * Separate from ASan: its allocator interposition would mask the libc path.
 * Queries may allocate only to compile Frizbee matchers for words a workspace
 * has not cached (bounded per word and scoring thread); a repeated query, a
 * cached one-symbol answer and vector/fusion queries (including the two-pass
 * prefix shortlist) allocate nothing. */
#include "torchlight/fuzzy.h"
#include "torchlight/lexical.h"
#include "torchlight/rank.h"
#include "torchlight/vector.h"
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
extern void *__libc_memalign(size_t alignment, size_t size);
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
/** Interpose posix_memalign (Rust's allocator uses it for aligned SIMD state). */
int posix_memalign(void **out, size_t alignment, size_t size) {
    if (atomic_load(&probing))
        atomic_fetch_add(&allocations, 1);
    void *pointer = __libc_memalign(alignment, size);
    if (pointer == NULL)
        return 12; /* ENOMEM without pulling errno.h into the interposer */
    *out = pointer;
    return 0;
}
/** Interpose aligned_alloc with the same tracking as malloc. */
void *aligned_alloc(size_t alignment, size_t size) {
    if (atomic_load(&probing))
        atomic_fetch_add(&allocations, 1);
    return __libc_memalign(alignment, size);
}
/** Interpose memalign with the same tracking as malloc. */
void *memalign(size_t alignment, size_t size) {
    if (atomic_load(&probing))
        atomic_fetch_add(&allocations, 1);
    return __libc_memalign(alignment, size);
}
/** Forward free to the allocator paired with the interposed allocation calls. */
void free(void *pointer) {
    __libc_free(pointer);
}

/* Scoring participants of a large-engine workspace (see lexical_query.c). */
enum { ALLOCATION_PARTICIPANTS = 4 };
static size_t matcher_allocations;
/* Measure the most allocations compiling one matcher takes (short, long and
 * non-ASCII words), the unit of the per-query bound. */
static void calibrate(void) {
    static const size_t lengths[] = {1, 5, FUZZY_MATCHER_MAX_SYMBOLS};
    static const uint32_t letters[] = {'n', 0xe9};
    uint32_t symbols[FUZZY_MATCHER_MAX_SYMBOLS];
    uint8_t boundaries[FUZZY_MATCHER_MAX_SYMBOLS] = {1};
    matcher_allocations = 0;
    for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
        for (size_t c = 0; c < sizeof(letters) / sizeof(letters[0]); c++) {
            for (size_t i = 0; i < lengths[l]; i++)
                symbols[i] = letters[c];
            tl_text word = {.symbols = symbols, .boundaries = boundaries, .length = lengths[l]};
            tl_fuzzy_matcher *matcher = NULL;
            atomic_store(&allocations, 0);
            atomic_store(&probing, true);
            tl_status status = fuzzy_matcher_create(word, &matcher);
            atomic_store(&probing, false);
            fuzzy_matcher_destroy(matcher);
            size_t used = atomic_load(&allocations);
            if (status == TL_OK && used > matcher_allocations)
                matcher_allocations = used;
        }
    }
}
static size_t count_words(const char *query) {
    size_t words = 0;
    for (size_t i = 0; query[i] != 0; i++)
        words += query[i] != ' ' && (i == 0 || query[i - 1] == ' ');
    return words;
}
static size_t measured_query(const tl_lexical *engine, tl_lexical_workspace *workspace,
                             const char *query, tl_status *status) {
    tl_result results[10];
    size_t count = 0;
    atomic_store(&allocations, 0);
    atomic_store(&probing, true);
    *status = lexical_query(engine, workspace, query, results, 10, &count);
    atomic_store(&probing, false);
    return atomic_load(&allocations);
}
/* First run: at most one matcher per word and participant. Second run of the
 * same query reuses the cached words and must not allocate at all. */
static tl_status check_query(const tl_lexical *engine, tl_lexical_workspace *workspace,
                             const char *query) {
    tl_status status = TL_OK;
    size_t bound = count_words(query) * ALLOCATION_PARTICIPANTS * matcher_allocations;
    size_t first = measured_query(engine, workspace, query, &status);
    if (status != TL_OK)
        return status;
    size_t again = measured_query(engine, workspace, query, &status);
    if (first > bound || again != 0) {
        fprintf(stderr, "query of %zu bytes allocated %zu then %zu times (bound %zu)\n",
                strlen(query), first, again, bound);
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
    const char *cases[] = {"",           "proej",       "prxoje",         "RAEDME",
                           "root notes", "caf\xc3\xa9", "caf\x65\xcc\x81"};
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
    const char *queries[] = {"ao",    "root ao", "md",    "mad noets", "mad",
                             "notes", "root",    "noets", "x z"};
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]) && status == TL_OK; i++)
        status = check_query(engine, workspace, queries[i]);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
    return status;
}

static tl_status check_hybrid(void) {
    enum { ALLOCATION_VECTOR_ROWS = 1000, ALLOCATION_VECTOR_DIMENSIONS = 4 };
    tl_vector *index = NULL;
    tl_vector_workspace *workspace = NULL;
    tl_rank *ranker = NULL;
    const float query[] = {1, -2, 3, -4};
    tl_rank_candidate lexical[ALLOCATION_VECTOR_ROWS], semantic[ALLOCATION_VECTOR_ROWS];
    tl_vector_result neighbors[ALLOCATION_VECTOR_ROWS];
    tl_rank_result fused[ALLOCATION_VECTOR_ROWS];
    tl_status status =
        vector_create(1, ALLOCATION_VECTOR_DIMENSIONS, ALLOCATION_VECTOR_ROWS, SIZE_MAX, &index);
    for (size_t i = 0; i < ALLOCATION_VECTOR_ROWS && status == TL_OK; i++) {
        float values[] = {(float)(i + 1), -2, 3, -4};
        status = vector_add(index, i + 1, 1, values, ALLOCATION_VECTOR_DIMENSIONS);
        lexical[i] = (tl_rank_candidate){i + 1, "/root/shared", RANK_REGULAR};
        semantic[i] = (tl_rank_candidate){ALLOCATION_VECTOR_ROWS - i, "/root/shared", RANK_REGULAR};
    }
    if (status == TL_OK)
        status = vector_finish(index);
    if (status == TL_OK)
        status = vector_workspace_create(index, &workspace);
    if (status == TL_OK)
        status = rank_create(ALLOCATION_VECTOR_ROWS * 2, RANK_DEFAULT_RRF_K, &ranker);
    atomic_store(&allocations, 0);
    atomic_store(&probing, true);
    for (size_t i = 0; i < 20 && status == TL_OK; i++) {
        size_t count = 0;
        status = vector_query(index, workspace, 1, query, ALLOCATION_VECTOR_DIMENSIONS, neighbors,
                              ALLOCATION_VECTOR_ROWS, &count);
        if (status == TL_OK)
            status = rank_fuse(ranker, lexical, ALLOCATION_VECTOR_ROWS, semantic,
                               ALLOCATION_VECTOR_ROWS, fused, ALLOCATION_VECTOR_ROWS, &count);
    }
    atomic_store(&probing, false);
    if (atomic_load(&allocations) != 0) {
        fprintf(stderr, "vector/fusion query allocated %zu times\n", atomic_load(&allocations));
        status = TL_STATE;
    }
    rank_destroy(ranker);
    vector_workspace_destroy(workspace);
    vector_destroy(index);
    return status;
}

/* The first pass, its candidate trims and the rescoring use workspace scratch. */
static tl_status check_prefix_shortlist(void) {
    enum { PREFIX_ROWS = 600, PREFIX_DIMENSIONS = 32, PREFIX_LENGTH = 16, PREFIX_SHORTLIST = 50 };
    tl_vector *index = NULL;
    tl_vector_workspace *workspace = NULL;
    tl_status status = vector_create_prefix_int8(1, PREFIX_DIMENSIONS, PREFIX_ROWS, PREFIX_LENGTH,
                                                 PREFIX_SHORTLIST, SIZE_MAX, &index);
    float values[PREFIX_DIMENSIONS];
    for (size_t i = 0; i < PREFIX_ROWS && status == TL_OK; i++) {
        for (size_t j = 0; j < PREFIX_DIMENSIONS; j++)
            values[j] = (float)((int)((i * 31 + j * 7 + i * j) % 23) - 11);
        status = vector_add(index, i + 1, 1, values, PREFIX_DIMENSIONS);
    }
    if (status == TL_OK)
        status = vector_finish(index);
    if (status == TL_OK)
        status = vector_workspace_create(index, &workspace);
    atomic_store(&allocations, 0);
    atomic_store(&probing, true);
    for (size_t i = 0; i < 20 && status == TL_OK; i++) {
        tl_vector_result neighbors[PREFIX_SHORTLIST];
        size_t count = 0;
        values[i % PREFIX_DIMENSIONS] += 1;
        status = vector_query(index, workspace, 1, values, PREFIX_DIMENSIONS, neighbors,
                              PREFIX_SHORTLIST, &count);
    }
    atomic_store(&probing, false);
    if (atomic_load(&allocations) != 0) {
        fprintf(stderr, "prefix shortlist query allocated %zu times\n", atomic_load(&allocations));
        status = TL_STATE;
    }
    vector_workspace_destroy(workspace);
    vector_destroy(index);
    return status;
}

int main(void) {
    calibrate();
    if (matcher_allocations == 0) {
        fputs("matcher calibration failed\n", stderr);
        return 1;
    }
    tl_lexical *engine = NULL;
    tl_lexical_workspace *workspace = NULL;
    tl_status status = lexical_create(&engine);
    const char *paths[] = {"/root/abcdefghijklmnopqrstuvwxyz.txt", "/root/projectNotes.md",
                           "/root/README.md", "/root/Caf\xc3\xa9.pdf", "/root/bad\xff.txt"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && status == TL_OK; i++)
        status =
            lexical_add_fields(engine, i + 1, paths[i], "document editor", "screen resolution");
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
        status = check_hybrid();
    if (status == TL_OK)
        status = check_prefix_shortlist();
    if (status == TL_OK)
        puts("Allocation-free query checks passed.");
    return status == TL_OK ? 0 : 1;
}
