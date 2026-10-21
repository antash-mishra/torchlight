/* ADR 0040 equivalence: a writer that compares scans with the persisted-state
 * map and one that writes every entry (the behavior before it) must leave
 * identical catalogs after the same random filesystem changes, whether a
 * startup scan finds changes made while no daemon ran or they happen live. */
#include "test.h"
#include "torchlight/store.h"
#include "torchlight/vec.h"
#include "torchlight/writer.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define EQUIVALENCE_ROUNDS 5
#define OFFLINE_OPERATIONS 40
#define LIVE_OPERATIONS 12
#define INITIAL_OPERATIONS 80
#define EQUIVALENCE_SEED UINT64_C(0x2545f4914f6cdd1d)
#define TREE_LIMIT 600
#define NAME_BYTES 300
#define SETTLE_MS 300
#define WAIT_STEPS 20000
#define OPERATION_KINDS 10
/* A file in the root the random operations never list, so never touch: every
 * comparison then holds more than the root whenever scans work. Without it,
 * removals, renames out of the tree and hidden or ignored names could leave
 * nothing indexable, failing the count check although both writers agreed
 * (seeds 7, 16 and 19 did here, and a release runner's directory order did
 * for the default seed). */
#define ANCHOR_NAME "anchor.txt"
struct mode {
    char database[128], config_path[128];
    tl_config *config;
    tl_catalog *catalog;
    tl_writer *writer;
    bool exhaustive;
};
struct world {
    char base[64], root[96], outside[96];
    uint64_t random;
    unsigned counter;
    struct mode modes[2];
};
struct tree {
    char paths[TREE_LIMIT][NAME_BYTES];
    bool directories[TREE_LIMIT];
    size_t count;
};
static uint64_t next_random(struct world *world) {
    world->random ^= world->random << 13;
    world->random ^= world->random >> 7;
    world->random ^= world->random << 17;
    return world->random;
}
static size_t pick(struct world *world, size_t count) {
    return (size_t)(next_random(world) % count);
}
static void pause_ms(long milliseconds) {
    const struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    CHECK(nanosleep(&delay, NULL) == 0);
}
/* Depth-first listing below path (unreadable directories are not entered). */
static void list_into(struct tree *tree, const char *path) {
    DIR *directory = opendir(path);
    if (directory == NULL)
        return;
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL && tree->count < TREE_LIMIT) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, ANCHOR_NAME) == 0)
            continue;
        size_t index = tree->count;
        if (snprintf(tree->paths[index], NAME_BYTES, "%s/%s", path, entry->d_name) >= NAME_BYTES)
            continue;
        struct stat info;
        if (lstat(tree->paths[index], &info) != 0)
            continue;
        tree->directories[index] = S_ISDIR(info.st_mode);
        tree->count++;
        if (tree->directories[index])
            list_into(tree, tree->paths[index]);
    }
    CHECK(closedir(directory) == 0);
}
static struct tree *list_tree(const struct world *world) {
    struct tree *tree = calloc(1, sizeof(*tree));
    CHECK(tree != NULL);
    snprintf(tree->paths[0], NAME_BYTES, "%s", world->root);
    tree->directories[0] = true;
    tree->count = 1;
    list_into(tree, world->root);
    return tree;
}
static const char *random_directory(struct world *world, const struct tree *tree) {
    for (;;) {
        size_t index = pick(world, tree->count);
        if (tree->directories[index])
            return tree->paths[index];
    }
}
/* Names include hidden and ignored ones, which no scan may index. */
static void child_name(struct world *world, const char *parent, const char *kind, char *out) {
    static const char *const prefixes[] = {"", "", "", "", ".hidden-", "node_modules"};
    const char *prefix = prefixes[pick(world, 6)];
    unsigned number = world->counter++;
    if (strcmp(prefix, "node_modules") == 0)
        snprintf(out, NAME_BYTES, "%s/node_modules", parent);
    else
        snprintf(out, NAME_BYTES, "%s/%s%s%u.txt", parent, prefix, kind, number);
}
static void write_bytes(const char *path, int flags, size_t count) {
    int fd = open(path, flags | O_WRONLY, 0600);
    if (fd < 0)
        return;
    char bytes[64] = {0};
    ssize_t written = write(fd, bytes, count % sizeof(bytes));
    (void)written;
    CHECK(close(fd) == 0);
}
/* Remove path and everything below it, unlocking directories on the way. */
static void remove_tree(const char *path) {
    struct stat info;
    if (lstat(path, &info) != 0)
        return;
    if (!S_ISDIR(info.st_mode)) {
        CHECK(unlink(path) == 0);
        return;
    }
    CHECK(chmod(path, 0700) == 0);
    DIR *directory = opendir(path);
    CHECK(directory != NULL);
    struct dirent *entry = NULL;
    char child[NAME_BYTES];
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 &&
            snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) < NAME_BYTES)
            remove_tree(child);
    }
    CHECK(closedir(directory) == 0);
    CHECK(rmdir(path) == 0);
}
/* Swap a directory for a new one at the same path holding the same objects:
 * the store retires the old row's subtree although every descendant is
 * unchanged, so an exact scan must write them back (ADR 0040). */
static void replace_directory(struct world *world, const char *path) {
    char held[NAME_BYTES], names[64][NAME_BYTES], from[NAME_BYTES * 2], to[NAME_BYTES * 2];
    snprintf(held, sizeof(held), "%s/held%u", world->outside, world->counter++);
    if (rename(path, held) != 0)
        return;
    CHECK(mkdir(path, 0700) == 0);
    DIR *directory = opendir(held);
    size_t count = 0;
    struct dirent *entry = NULL;
    while (directory != NULL && (entry = readdir(directory)) != NULL && count < 64)
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            snprintf(names[count++], NAME_BYTES, "%s", entry->d_name);
    if (directory != NULL)
        CHECK(closedir(directory) == 0);
    for (size_t i = 0; i < count; i++) {
        snprintf(from, sizeof(from), "%s/%s", held, names[i]);
        snprintf(to, sizeof(to), "%s/%s", path, names[i]);
        int code = rename(from, to);
        (void)code; /* a locked directory cannot move; it goes with held */
    }
    remove_tree(held);
}
static void rename_entry(struct world *world, const struct tree *tree, const char *path) {
    char target[NAME_BYTES];
    /* Sometimes out of the indexed tree, so the move has no paired event. */
    const char *parent = pick(world, 5) == 0 ? world->outside : random_directory(world, tree);
    snprintf(target, sizeof(target), "%s/moved%u", parent, world->counter++);
    size_t length = strlen(path);
    if (strncmp(target, path, length) == 0 && target[length] == '/')
        return; /* into its own subtree */
    int code = rename(path, target);
    (void)code; /* locked directories cannot move */
}
static void touch_entry(struct world *world, const char *path) {
    struct timespec times[2] = {{.tv_sec = (time_t)(1000000000 + pick(world, 100000))},
                                {.tv_sec = (time_t)(1000000000 + pick(world, 100000))}};
    int code = utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW);
    (void)code;
}
/* One random change; failures (e.g. inside a locked directory) are skipped. */
static void operate(struct world *world) {
    struct tree *tree = list_tree(world);
    size_t index = 1 + pick(world, tree->count > 1 ? tree->count - 1 : 1);
    const char *entry = index < tree->count ? tree->paths[index] : NULL;
    bool directory = index < tree->count && tree->directories[index];
    char name[NAME_BYTES];
    int code = 0;
    switch (pick(world, OPERATION_KINDS)) {
    case 0:
        child_name(world, random_directory(world, tree), "file", name);
        write_bytes(name, O_CREAT, pick(world, 64));
        break;
    case 1:
        child_name(world, random_directory(world, tree), "dir", name);
        code = mkdir(name, 0700);
        break;
    case 2:
        if (entry != NULL)
            remove_tree(entry);
        break;
    case 3:
        if (entry != NULL)
            rename_entry(world, tree, entry);
        break;
    case 4:
        if (entry != NULL)
            touch_entry(world, entry);
        break;
    case 5:
        if (entry != NULL && !directory)
            write_bytes(entry, O_APPEND, 1 + pick(world, 32));
        break;
    case 6: /* replaced file: same name, new object */
        if (entry != NULL && !directory && unlink(entry) == 0)
            write_bytes(entry, O_CREAT, pick(world, 64));
        break;
    case 7:
        if (entry != NULL && directory)
            replace_directory(world, entry);
        break;
    case 8: /* toggle an unreadable directory */
        if (entry != NULL && directory && geteuid() != 0) {
            struct stat info;
            if (lstat(entry, &info) == 0)
                code = chmod(entry, (info.st_mode & 0700) == 0 ? 0700 : 0);
        }
        break;
    default:
        child_name(world, random_directory(world, tree), "link", name);
        code = symlink(world->outside, name);
        break;
    }
    (void)code;
    free(tree);
}
struct row {
    char *path;
    bool is_dir, is_root;
    uint64_t signature;
};
static tl_status collect_row(void *context, const tl_store_entry *entry) {
    struct row row = {strdup(entry->path), entry->is_dir, entry->is_root, entry->signature};
    CHECK(row.path != NULL);
    return vec_append(context, &row);
}
static int compare_rows(const void *left, const void *right) {
    return strcmp(((const struct row *)left)->path, ((const struct row *)right)->path);
}
static tl_vec *catalog_rows(const char *database) {
    tl_store *store = NULL;
    tl_vec *rows = NULL;
    CHECK(store_create(database, &store) == TL_OK &&
          vec_create(sizeof(struct row), &rows) == TL_OK);
    CHECK(store_load(store, collect_row, rows) == TL_OK);
    store_destroy(store);
    qsort(vec_data(rows), vec_count(rows), sizeof(struct row), compare_rows);
    return rows;
}
static void free_rows(tl_vec *rows) {
    struct row *data = vec_data(rows);
    for (size_t i = 0; i < vec_count(rows); i++)
        free(data[i].path);
    vec_destroy(rows);
}
/* Both catalogs hold the same paths with the same kind, root flag and stored
 * metadata (ids may differ: retired rows are written back in another order). */
static void compare_catalogs(const struct world *world, int round) {
    tl_vec *exact = catalog_rows(world->modes[0].database);
    tl_vec *reference = catalog_rows(world->modes[1].database);
    const struct row *a = vec_const_data(exact), *b = vec_const_data(reference);
    size_t i = 0, j = 0;
    bool equal = true;
    while (i < vec_count(exact) || j < vec_count(reference)) {
        int order = i == vec_count(exact)       ? 1
                    : j == vec_count(reference) ? -1
                                                : strcmp(a[i].path, b[j].path);
        if (order == 0 && a[i].is_dir == b[j].is_dir && a[i].is_root == b[j].is_root &&
            a[i].signature == b[j].signature) {
            i++, j++;
            continue;
        }
        equal = false;
        fprintf(stderr, "round %d: exact %s | exhaustive %s\n", round, order <= 0 ? a[i].path : "-",
                order >= 0 ? b[j].path : "-");
        i += order <= 0;
        j += order >= 0;
    }
    CHECK(equal && vec_count(exact) > 1);
    free_rows(exact);
    free_rows(reference);
}
static void start_mode(struct mode *mode) {
    CHECK(config_create("torchlight", mode->database, mode->config_path, NULL, &mode->config) ==
          TL_OK);
    CHECK(catalog_create(2, &mode->catalog) == TL_OK);
    tl_writer_options options = {.config = mode->config,
                                 .catalog = mode->catalog,
                                 .socket_path = "/tmp/torchlight-unused-equivalence.sock",
                                 .watch_capacity = 4096,
                                 .max_entries = 100000,
                                 .max_path_bytes = 1 << 22,
                                 .readers = 1,
                                 .rescan_ms = 3600000,
                                 .history_days = 30,
                                 .exhaustive = mode->exhaustive};
    CHECK(writer_create(&options, &mode->writer) == TL_OK);
}
static void stop_mode(struct mode *mode) {
    writer_destroy(mode->writer);
    CHECK(catalog_destroy(mode->catalog) == TL_OK);
    config_destroy(mode->config);
    mode->writer = NULL;
}
/* Wait until a writer has finished at least one pass and none followed for
 * SETTLE_MS (follow-up rescans of replaced directories included). */
static tl_writer_stats settle(const struct mode *mode) {
    tl_writer_stats last = {0}, current = {0};
    long quiet = 0;
    for (size_t i = 0; i < WAIT_STEPS && quiet < SETTLE_MS; i++) {
        pause_ms(1);
        CHECK(writer_stats(mode->writer, &current) == TL_OK);
        bool same = current.reconciliations == last.reconciliations && !current.indexing &&
                    current.reconciliations != 0;
        quiet = same ? quiet + 1 : 0;
        last = current;
    }
    CHECK(quiet >= SETTLE_MS);
    return current;
}
static void start_both(struct world *world) {
    for (int m = 0; m < 2; m++)
        start_mode(&world->modes[m]);
    for (int m = 0; m < 2; m++)
        settle(&world->modes[m]);
}
static void stop_both(struct world *world) {
    for (int m = 0; m < 2; m++)
        stop_mode(&world->modes[m]);
}
static void reconcile_both(struct world *world) {
    for (int m = 0; m < 2; m++)
        CHECK(writer_reconcile(world->modes[m].writer) == TL_OK);
    pause_ms(50);
    for (int m = 0; m < 2; m++)
        settle(&world->modes[m]);
}
/* TORCHLIGHT_EQUIVALENCE_SEED (nonzero decimal) explores other sequences. */
static uint64_t equivalence_seed(void) {
    const char *text = getenv("TORCHLIGHT_EQUIVALENCE_SEED");
    unsigned long long seed = text == NULL ? 0 : strtoull(text, NULL, 10);
    return seed == 0 ? EQUIVALENCE_SEED : (uint64_t)seed;
}
static void prepare_world(struct world *world) {
    *world = (struct world){.random = equivalence_seed()};
    /* A local copy: formatting from one member into another of the same
     * object would alias (snprintf's arguments are restrict). */
    char base[sizeof(world->base)] = "/tmp/torchlight-equivalence-XXXXXX";
    CHECK(mkdtemp(base) != NULL);
    memcpy(world->base, base, sizeof(base));
    snprintf(world->root, sizeof(world->root), "%s/root", base);
    snprintf(world->outside, sizeof(world->outside), "%s/outside", base);
    CHECK(mkdir(world->root, 0700) == 0 && mkdir(world->outside, 0700) == 0);
    char root[sizeof(world->root)];
    memcpy(root, world->root, sizeof(root));
    for (int m = 0; m < 2; m++) {
        struct mode *mode = &world->modes[m];
        mode->exhaustive = m == 1;
        snprintf(mode->database, sizeof(mode->database), "%s/catalog-%d.db", base, m);
        snprintf(mode->config_path, sizeof(mode->config_path), "%s/config-%d", base, m);
        FILE *file = fopen(mode->config_path, "w");
        CHECK(file != NULL && fprintf(file, "root = %s\n", root) > 0 && fclose(file) == 0);
    }
}
/* A fresh tree of plain files and directories (none unreadable, which every
 * scan writes to keep their saved entries). */
static void plain_tree(struct world *world) {
    remove_tree(world->root);
    CHECK(mkdir(world->root, 0700) == 0);
    char path[NAME_BYTES];
    for (int d = 0; d < 8; d++) {
        snprintf(path, sizeof(path), "%s/plain%d", world->root, d);
        CHECK(mkdir(path, 0700) == 0);
        for (int f = 0; f < 10; f++) {
            snprintf(path, sizeof(path), "%s/plain%d/file%d.txt", world->root, d, f);
            write_bytes(path, O_CREAT, (size_t)f);
        }
    }
}
/* A full scan that finds nothing new writes nothing (ADR 0040), while the
 * old behavior writes every entry. */
static void unchanged_scan_writes_nothing(struct world *world) {
    reconcile_both(world);
    tl_writer_stats exact = settle(&world->modes[0]), reference = settle(&world->modes[1]);
    CHECK(exact.last_full_reason == WRITER_FULL_RECONCILE && exact.last_scan_writes == 0 &&
          exact.last_scan_unseen == 0);
    CHECK(reference.last_scan_writes > 1);
}
static bool cataloged(const char *database, const char *path) {
    tl_vec *rows = catalog_rows(database);
    bool found = false;
    const struct row *data = vec_const_data(rows);
    for (size_t i = 0; i < vec_count(rows) && !found; i++)
        found = strcmp(data[i].path, path) == 0;
    free_rows(rows);
    return found;
}
/* Deterministic case of the random one: while no daemon ran, a directory was
 * replaced by a new one holding the very same (unchanged) files. The store
 * retires the old directory's subtree; the exact mode must write it back. */
static void replaced_directory_keeps_descendants(struct world *world) {
    char directory[NAME_BYTES], sub[NAME_BYTES], first[NAME_BYTES], second[NAME_BYTES];
    CHECK(snprintf(directory, sizeof(directory), "%s/replaced", world->root) < NAME_BYTES);
    CHECK(snprintf(sub, sizeof(sub), "%s/sub", directory) < NAME_BYTES);
    CHECK(snprintf(first, sizeof(first), "%s/first.txt", directory) < NAME_BYTES);
    CHECK(snprintf(second, sizeof(second), "%s/second.txt", sub) < NAME_BYTES);
    CHECK(mkdir(directory, 0700) == 0 && mkdir(sub, 0700) == 0);
    write_bytes(first, O_CREAT, 3);
    write_bytes(second, O_CREAT, 5);
    start_both(world);
    stop_both(world);
    replace_directory(world, directory);
    start_both(world);
    compare_catalogs(world, -1);
    for (int m = 0; m < 2; m++)
        CHECK(cataloged(world->modes[m].database, first) &&
              cataloged(world->modes[m].database, second));
    stop_both(world);
    remove_tree(directory);
}
void test_scan_equivalence(void) {
    struct world world;
    prepare_world(&world);
    replaced_directory_keeps_descendants(&world);
    char anchor[NAME_BYTES];
    CHECK(snprintf(anchor, sizeof(anchor), "%s/%s", world.root, ANCHOR_NAME) < NAME_BYTES);
    write_bytes(anchor, O_CREAT, 1);
    for (int i = 0; i < INITIAL_OPERATIONS; i++)
        operate(&world);
    for (int round = 0; round < EQUIVALENCE_ROUNDS; round++) {
        start_both(&world); /* the startup scan finds the offline changes */
        compare_catalogs(&world, round);
        for (int i = 0; i < LIVE_OPERATIONS; i++)
            operate(&world);
        pause_ms(200);
        reconcile_both(&world);
        compare_catalogs(&world, round);
        stop_both(&world);
        for (int i = 0; i < OFFLINE_OPERATIONS; i++)
            operate(&world);
    }
    plain_tree(&world);
    start_both(&world);
    unchanged_scan_writes_nothing(&world);
    stop_both(&world);
    remove_tree(world.base);
}
