/* Children prunes walk the path index instead of every row below the
 * directory (M7 Phase 3): compare store_prune_children with a brute-force
 * model of the statement it replaced over random trees whose names sort
 * around '/' ("a", "a.b", "a-", "a0", "a/x"), with orphans, kept paths,
 * nested roots and the "/" directory. */
#include "test.h"
#include "torchlight/store.h"
#include <string.h>
#include <unistd.h>
enum {
    PRUNE_ROUNDS = 120,
    PRUNE_MAX_ROWS = 48,
    PRUNE_MAX_DEPTH = 3,
    PRUNE_PATH_BYTES = 64,
    PRUNE_PERCENT = 100
};
static const char *const PRUNE_NAMES[] = {"a", "a.b", "a-", "a0", "b", "ab", "\xff"};
#define PRUNE_NAME_COUNT (sizeof(PRUNE_NAMES) / sizeof(PRUNE_NAMES[0]))
struct prune_case {
    char rows[PRUNE_MAX_ROWS][PRUNE_PATH_BYTES];
    bool seen[PRUNE_MAX_ROWS], kept[PRUNE_MAX_ROWS], root[PRUNE_MAX_ROWS];
    size_t count;
    uint64_t random;
};
static uint32_t prune_random(struct prune_case *test, uint32_t bound) {
    test->random ^= test->random << 13;
    test->random ^= test->random >> 7;
    test->random ^= test->random << 17;
    return (uint32_t)(test->random % bound);
}
static bool chance(struct prune_case *test, uint32_t percent) {
    return prune_random(test, PRUNE_PERCENT) < percent;
}
/* A random path below "/r" (or the "/r2" and "/q" neighbors) at most
 * PRUNE_MAX_DEPTH components deep; parents need not exist (orphans). */
static void random_path(struct prune_case *test, char *out) {
    static const char *const bases[] = {"/r", "/r", "/r", "/r2", "/q"};
    size_t length = (size_t)snprintf(out, PRUNE_PATH_BYTES, "%s", bases[prune_random(test, 5)]);
    uint32_t depth = 1 + prune_random(test, PRUNE_MAX_DEPTH);
    for (uint32_t i = 0; i < depth; i++)
        length += (size_t)snprintf(out + length, PRUNE_PATH_BYTES - length, "/%s",
                                   PRUNE_NAMES[prune_random(test, PRUNE_NAME_COUNT)]);
}
static bool listed(const struct prune_case *test, const char *path) {
    for (size_t i = 0; i < test->count; i++)
        if (strcmp(test->rows[i], path) == 0)
            return true;
    return false;
}
static void random_case(struct prune_case *test) {
    test->count = 0;
    const char *fixed[] = {"/", "/r", "/r2", "/q"};
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        if (chance(test, 80))
            snprintf(test->rows[test->count++], PRUNE_PATH_BYTES, "%s", fixed[i]);
    uint32_t extra = prune_random(test, PRUNE_MAX_ROWS - 4);
    for (uint32_t i = 0; i < extra; i++) {
        char path[PRUNE_PATH_BYTES];
        random_path(test, path);
        if (!listed(test, path))
            snprintf(test->rows[test->count++], PRUNE_PATH_BYTES, "%s", path);
    }
    for (size_t i = 0; i < test->count; i++) {
        test->seen[i] = chance(test, 50);
        test->kept[i] = chance(test, 5);
        test->root[i] = strcmp(test->rows[i], "/r") != 0 && chance(test, 5);
    }
}
/* path is at or below base, component-wise. */
static bool at_or_below(const char *path, const char *base) {
    size_t length = strlen(base);
    return strncmp(path, base, length) == 0 && (path[length] == 0 || path[length] == '/');
}
/* The replaced statement: a row below the directory's prefix goes when its
 * direct-child component is unseen, unless it lies at or below a kept path
 * or a registered root inside [prefix, limit). */
static bool model_deletes(const struct prune_case *test, const char *directory, const char *row) {
    char prefix[PRUNE_PATH_BYTES];
    snprintf(prefix, sizeof(prefix), "%s%s", directory, strcmp(directory, "/") == 0 ? "" : "/");
    size_t length = strlen(prefix);
    if (strncmp(row, prefix, length) != 0)
        return false;
    const char *slash = strchr(row + length, '/');
    size_t child = slash == NULL ? strlen(row) : (size_t)(slash - row);
    for (size_t i = 0; i < test->count; i++) {
        if (test->seen[i] && strlen(test->rows[i]) == child &&
            strncmp(test->rows[i], row, child) == 0)
            return false;
        if (test->kept[i] && at_or_below(row, test->rows[i]))
            return false;
        if (test->root[i] && strncmp(test->rows[i], prefix, length) == 0 &&
            at_or_below(row, test->rows[i]))
            return false;
    }
    return true;
}
struct survivors {
    char rows[PRUNE_MAX_ROWS][PRUNE_PATH_BYTES];
    size_t count;
};
static tl_status collect_survivor(void *context, const tl_store_entry *entry) {
    struct survivors *survivors = context;
    CHECK(survivors->count < PRUNE_MAX_ROWS && strlen(entry->path) < PRUNE_PATH_BYTES);
    snprintf(survivors->rows[survivors->count++], PRUNE_PATH_BYTES, "%s", entry->path);
    return TL_OK;
}
static void put_row(tl_store *store, const char *path) {
    tl_crawl_entry entry = {.path = path, .is_dir = true, .has_stat = true};
    CHECK(store_put(store, &entry) == TL_OK);
}
/* Save the case's rows (registering its nested roots), then prune directory
 * with the case's seen and kept marks, and compare with the model. */
static void check_case(const struct prune_case *test, const char *directory) {
    char database[] = "/tmp/torchlight-prune-XXXXXX";
    int fd = mkstemp(database);
    CHECK(fd >= 0 && close(fd) == 0);
    tl_store *store = NULL;
    CHECK(store_create(database, &store) == TL_OK && store_begin(store) == TL_OK);
    for (size_t i = 0; i < test->count; i++)
        put_row(store, test->rows[i]);
    for (size_t i = 0; i < test->count; i++)
        if (test->root[i])
            CHECK(store_prune(store, test->rows[i]) == TL_OK);
    CHECK(store_commit(store) == TL_OK && store_begin(store) == TL_OK);
    for (size_t i = 0; i < test->count; i++) {
        if (test->seen[i])
            CHECK(store_mark_seen(store, test->rows[i]) == TL_OK);
        if (test->kept[i])
            CHECK(store_keep(store, test->rows[i]) == TL_OK);
    }
    CHECK(store_prune_children(store, directory) == TL_OK && store_commit(store) == TL_OK);
    struct survivors survivors = {0};
    CHECK(store_load(store, collect_survivor, &survivors) == TL_OK);
    size_t expected = 0;
    for (size_t i = 0; i < test->count; i++) {
        bool kept = !model_deletes(test, directory, test->rows[i]);
        bool found = false;
        for (size_t j = 0; j < survivors.count && !found; j++)
            found = strcmp(survivors.rows[j], test->rows[i]) == 0;
        if (kept != found)
            fprintf(stderr, "prune %s: %s expected %s\n", directory, test->rows[i],
                    kept ? "kept" : "deleted");
        CHECK(kept == found);
        expected += kept;
    }
    CHECK(survivors.count == expected);
    store_destroy(store);
    CHECK(unlink(database) == 0);
}
void test_store_prune(void) {
    struct prune_case test = {.random = UINT64_C(0x9e3779b97f4a7c15)};
    const char *directories[] = {"/r", "/r/a", "/", "/r/a.b"};
    for (size_t round = 0; round < PRUNE_ROUNDS; round++) {
        random_case(&test);
        check_case(&test, directories[round % (sizeof(directories) / sizeof(directories[0]))]);
    }
}
