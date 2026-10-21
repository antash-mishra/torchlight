/* Crawler errors, unreadable scopes, allowlist traversal, excluded scopes,
 * tool-managed folders and root coverage. */
#include "test.h"
#include "torchlight/crawl.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static tl_status stop_scan(void *context, const tl_crawl_entry *entry) {
    size_t *count = context;
    CHECK(entry->is_root);
    (*count)++;
    return TL_LIMIT;
}
struct observed {
    size_t entries, unreadable;
    bool sibling;
};
static tl_status observe(void *context, const tl_crawl_entry *entry) {
    struct observed *observed = context;
    observed->entries++;
    if (entry->unreadable) {
        observed->unreadable++;
        CHECK(entry->is_dir && entry->has_stat && strstr(entry->path, "/locked") != NULL);
    }
    if (strstr(entry->path, "/sibling") != NULL)
        observed->sibling = true;
    return TL_OK;
}
/* Regression: one unreadable directory failed the whole walk. It is now
 * reported as unreadable and the walk continues to its siblings. fts reports
 * the directory twice (before and after its listing fails): 4 entries. */
static void unreadable_directory_is_reported(void) {
    if (geteuid() == 0)
        return; /* root can read mode-0 directories, so there is nothing to test */
    char root[] = "/tmp/torchlight-crawl-XXXXXX", locked[64], sibling[64];
    CHECK(mkdtemp(root) != NULL);
    CHECK(snprintf(locked, sizeof(locked), "%s/locked", root) < (int)sizeof(locked));
    CHECK(snprintf(sibling, sizeof(sibling), "%s/sibling", root) < (int)sizeof(sibling));
    CHECK(mkdir(locked, 0700) == 0 && mkdir(sibling, 0700) == 0 && chmod(locked, 0) == 0);
    tl_crawl *crawler = NULL;
    CHECK(crawl_create(NULL, 0, NULL, 0, &crawler) == TL_OK);
    struct observed observed = {0};
    tl_status status = crawl_run(crawler, root, observe, &observed);
    crawl_destroy(crawler);
    CHECK(chmod(locked, 0700) == 0 && rmdir(locked) == 0 && rmdir(sibling) == 0 &&
          rmdir(root) == 0);
    CHECK(status == TL_OK && observed.entries == 4 && observed.unreadable == 1 && observed.sibling);
}
enum { LISTING_CAPACITY = 32 };
struct listing {
    char paths[LISTING_CAPACITY][128];
    size_t count;
};
static tl_status list_path(void *context, const tl_crawl_entry *entry) {
    struct listing *listing = context;
    CHECK(listing->count < LISTING_CAPACITY && strlen(entry->path) < 128);
    snprintf(listing->paths[listing->count++], 128, "%s", entry->path);
    return TL_OK;
}
static bool listed(const struct listing *listing, const char *root, const char *suffix) {
    char path[160];
    snprintf(path, sizeof(path), "%s%s", root, suffix);
    for (size_t i = 0; i < listing->count; i++) {
        if (strcmp(listing->paths[i], path) == 0)
            return true;
    }
    return false;
}
static void make(const char *root, const char *suffix, bool directory) {
    char path[160];
    snprintf(path, sizeof(path), "%s%s", root, suffix);
    if (directory) {
        CHECK(mkdir(path, 0700) == 0);
        return;
    }
    FILE *file = fopen(path, "w");
    CHECK(file != NULL && fclose(file) == 0);
}
/* An allowlisted hidden directory is indexed; its hidden ancestor is only
 * traversed, and siblings that do not lead to it stay excluded. */
static void allowlist_traversal(void) {
    char root[] = "/tmp/torchlight-allow-XXXXXX", allowed[96];
    CHECK(mkdtemp(root) != NULL);
    const char *dirs[] = {"/.config", "/.config/nvim", "/.config/other", "/build"};
    for (size_t i = 0; i < 4; i++)
        make(root, dirs[i], true);
    make(root, "/.config/nvim/init.lua", false);
    make(root, "/.config/other/secret", false);
    make(root, "/.config/top-level-file", false);
    snprintf(allowed, sizeof(allowed), "%s/.config/nvim", root);
    const char *allow[] = {allowed};
    tl_crawl *crawler = NULL;
    CHECK(crawl_create(NULL, 0, allow, 1, &crawler) == TL_OK);
    struct listing listing = {0};
    CHECK(crawl_run(crawler, root, list_path, &listing) == TL_OK);
    CHECK(listed(&listing, root, "/.config/nvim") &&
          listed(&listing, root, "/.config/nvim/init.lua"));
    CHECK(!listed(&listing, root, "/.config") && !listed(&listing, root, "/.config/other"));
    CHECK(!listed(&listing, root, "/.config/top-level-file") && !listed(&listing, root, "/build"));
    /* Coverage: the allowlisted directory is covered, its hidden parent is not. */
    char inner[96];
    CHECK(crawl_covers(crawler, root, allowed));
    snprintf(inner, sizeof(inner), "%s/.config", root);
    CHECK(!crawl_covers(crawler, root, inner));
    snprintf(inner, sizeof(inner), "%s/build", root);
    CHECK(!crawl_covers(crawler, root, inner) && crawl_covers(crawler, root, root));
    const char *roots[] = {root, allowed, root, inner};
    bool keep[4];
    crawl_select_roots(crawler, roots, 4, keep);
    CHECK(keep[0] && !keep[1] && !keep[2] && keep[3]);
    crawl_destroy(crawler);
    const char *cleanup[] = {"/.config/nvim/init.lua",
                             "/.config/other/secret",
                             "/.config/top-level-file",
                             "/.config/nvim",
                             "/.config/other",
                             "/.config",
                             "/build"};
    for (size_t i = 0; i < 7; i++) {
        char path[160];
        snprintf(path, sizeof(path), "%s%s", root, cleanup[i]);
        CHECK(remove(path) == 0);
    }
    CHECK(rmdir(root) == 0);
}
static tl_status no_roots(void *context, const tl_crawl_entry *entry) {
    CHECK(!entry->is_root);
    return list_path(context, entry);
}
/* Scoped rescans report the directory and children (not grandchildren), or
 * the whole subtree, with the root walk's exclusions and no root flag. */
static void scoped_walks(void) {
    char root[] = "/tmp/torchlight-scope-XXXXXX", scope[96];
    CHECK(mkdtemp(root) != NULL);
    make(root, "/a", true);
    make(root, "/a/b", true);
    make(root, "/a/.hidden", true);
    make(root, "/a/b/deep.txt", false);
    make(root, "/a/.hidden/secret", false);
    make(root, "/a/top.txt", false);
    snprintf(scope, sizeof(scope), "%s/a", root);
    tl_crawl *crawler = NULL;
    CHECK(crawl_create(NULL, 0, NULL, 0, &crawler) == TL_OK);
    struct listing children = {0}, tree = {0};
    CHECK(crawl_scope(crawler, scope, false, no_roots, &children) == TL_OK);
    CHECK(children.count == 3 && listed(&children, root, "/a") && listed(&children, root, "/a/b") &&
          listed(&children, root, "/a/top.txt"));
    CHECK(crawl_scope(crawler, scope, true, no_roots, &tree) == TL_OK);
    CHECK(tree.count == 4 && listed(&tree, root, "/a/b/deep.txt") &&
          !listed(&tree, root, "/a/.hidden/secret"));
    char missing[128];
    snprintf(missing, sizeof(missing), "%s/missing", root);
    CHECK(crawl_scope(crawler, missing, false, no_roots, &tree) == TL_IO);
    CHECK(crawl_scope(crawler, "relative", false, no_roots, &tree) == TL_INVALID);
    crawl_destroy(crawler);
    const char *cleanup[] = {"/a/b/deep.txt", "/a/.hidden/secret", "/a/top.txt",
                             "/a/b",          "/a/.hidden",        "/a"};
    for (size_t i = 0; i < 6; i++) {
        char path[160];
        snprintf(path, sizeof(path), "%s%s", root, cleanup[i]);
        CHECK(remove(path) == 0);
    }
    CHECK(rmdir(root) == 0);
}
struct boundaries {
    size_t entries, boundaries;
    bool start;
};
static tl_status observe_boundary(void *context, const tl_crawl_entry *entry) {
    struct boundaries *seen = context;
    seen->entries++;
    if (entry->device_boundary) {
        CHECK(entry->is_dir && entry->has_stat);
        seen->boundaries++;
        seen->start = seen->start || strstr(entry->path, "/sub") == NULL;
    }
    return TL_OK;
}
/* The walk's start counts as a device boundary (nothing to compare it with);
 * a subdirectory and a file on the same device do not. A scoped walk starts
 * a new comparison at its own directory. */
static void device_boundaries(void) {
    char root[] = "/tmp/torchlight-boundary-XXXXXX", sub[64], file[80];
    CHECK(mkdtemp(root) != NULL);
    CHECK(snprintf(sub, sizeof(sub), "%s/sub", root) < (int)sizeof(sub) && mkdir(sub, 0700) == 0);
    CHECK(snprintf(file, sizeof(file), "%s/file", sub) < (int)sizeof(file));
    FILE *created = fopen(file, "w");
    CHECK(created != NULL && fclose(created) == 0);
    tl_crawl *crawler = NULL;
    CHECK(crawl_create(NULL, 0, NULL, 0, &crawler) == TL_OK);
    struct boundaries seen = {0};
    CHECK(crawl_run(crawler, root, observe_boundary, &seen) == TL_OK);
    CHECK(seen.entries == 3 && seen.boundaries == 1 && seen.start);
    seen = (struct boundaries){0};
    CHECK(crawl_scope(crawler, sub, true, observe_boundary, &seen) == TL_OK);
    CHECK(seen.entries == 2 && seen.boundaries == 1 && !seen.start);
    crawl_destroy(crawler);
    CHECK(unlink(file) == 0 && rmdir(sub) == 0 && rmdir(root) == 0);
}
struct records {
    size_t entries, matched;
};
static tl_status compare_record(void *context, const tl_crawl_entry *entry) {
    struct records *records = context;
    struct stat info;
    CHECK(lstat(entry->path, &info) == 0);
    records->entries++;
    if (entry->has_stat && entry->has_identity && !entry->unreadable &&
        entry->is_dir == S_ISDIR(info.st_mode) && entry->inode == (uint64_t)info.st_ino &&
        entry->mtime == (int64_t)info.st_mtime && entry->size == (int64_t)info.st_size)
        records->matched++;
    return TL_OK;
}
/* The walk skips fts's own stat of files (FTS_NOSTAT), so every record's
 * metadata comes from the single statx: files, a symlink (never followed)
 * and nested directories, which must still be entered. */
static void one_stat_records(void) {
    char root[] = "/tmp/torchlight-records-XXXXXX", path[128];
    CHECK(mkdtemp(root) != NULL);
    make(root, "/a", true);
    make(root, "/a/b", true);
    make(root, "/a/b/deep.txt", false);
    snprintf(path, sizeof(path), "%s/a/b/deep.txt", root);
    FILE *file = fopen(path, "w");
    CHECK(file != NULL && fputs("12345", file) >= 0 && fclose(file) == 0);
    struct timespec times[2] = {{.tv_sec = 1000000000}, {.tv_sec = 1000000000}};
    CHECK(utimensat(AT_FDCWD, path, times, 0) == 0);
    make(root, "/top.txt", false);
    char link[128];
    snprintf(link, sizeof(link), "%s/link", root);
    snprintf(path, sizeof(path), "%s/a", root);
    CHECK(symlink(path, link) == 0);
    tl_crawl *crawler = NULL;
    CHECK(crawl_create(NULL, 0, NULL, 0, &crawler) == TL_OK);
    struct records records = {0};
    CHECK(crawl_run(crawler, root, compare_record, &records) == TL_OK);
    CHECK(records.entries == 6 && records.matched == 6);
    records = (struct records){0};
    CHECK(crawl_scope(crawler, path, true, compare_record, &records) == TL_OK);
    CHECK(records.entries == 3 && records.matched == 3);
    crawl_destroy(crawler);
    const char *cleanup[] = {"/a/b/deep.txt", "/a/b", "/a", "/top.txt", "/link"};
    for (size_t i = 0; i < 5; i++) {
        snprintf(path, sizeof(path), "%s%s", root, cleanup[i]);
        CHECK(remove(path) == 0);
    }
    CHECK(rmdir(root) == 0);
}
static void remove_all(const char *root, const char *const *suffixes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        char path[160];
        snprintf(path, sizeof(path), "%s%s", root, suffixes[i]);
        CHECK(remove(path) == 0);
    }
    CHECK(rmdir(root) == 0);
}
/* Configured exclusions: excluded subtrees are never reported, an exclusion
 * beats the allowlist, a root inside one reports nothing, and coverage and
 * scoped walks follow the same rule. */
static void excluded_scopes(void) {
    char root[] = "/tmp/torchlight-exclude-XXXXXX", scopes[3][96], allowed[96], path[160];
    CHECK(mkdtemp(root) != NULL);
    const char *dirs[] = {"/keep", "/sdk", "/other", "/other/sdk2", "/.config", "/.config/nvim"};
    for (size_t i = 0; i < 6; i++)
        make(root, dirs[i], true);
    const char *files[] = {"/keep/a.txt", "/sdk/lib.so", "/other/sdk2/x", "/.config/nvim/init"};
    for (size_t i = 0; i < 4; i++)
        make(root, files[i], false);
    snprintf(scopes[0], sizeof(scopes[0]), "%s/sdk", root);
    snprintf(scopes[1], sizeof(scopes[1]), "%s/other/sdk2", root);
    snprintf(scopes[2], sizeof(scopes[2]), "%s/.config", root);
    snprintf(allowed, sizeof(allowed), "%s/.config/nvim", root);
    const char *exclude[] = {scopes[0], scopes[1], scopes[2]}, *allow[] = {allowed};
    tl_crawl *crawler = NULL;
    CHECK(crawl_create(exclude, 3, allow, 1, &crawler) == TL_OK);
    struct listing listing = {0};
    CHECK(crawl_run(crawler, root, list_path, &listing) == TL_OK);
    CHECK(listing.count == 4 && listed(&listing, root, "") && listed(&listing, root, "/keep") &&
          listed(&listing, root, "/keep/a.txt") && listed(&listing, root, "/other"));
    CHECK(!crawl_covers(crawler, root, scopes[0]) && !crawl_covers(crawler, root, allowed));
    snprintf(path, sizeof(path), "%s/keep", root);
    CHECK(crawl_covers(crawler, root, path) && !crawl_covers(crawler, scopes[0], scopes[0]));
    /* A scoped walk of the parent skips the excluded child. */
    snprintf(path, sizeof(path), "%s/other", root);
    listing = (struct listing){0};
    CHECK(crawl_scope(crawler, path, true, list_path, &listing) == TL_OK);
    CHECK(listing.count == 1 && listed(&listing, root, "/other"));
    /* A root inside an exclusion is walked but reports nothing. */
    listing = (struct listing){0};
    CHECK(crawl_run(crawler, scopes[0], list_path, &listing) == TL_OK && listing.count == 0);
    crawl_destroy(crawler);
    const char *invalid[] = {NULL};
    CHECK(crawl_create(NULL, 1, NULL, 0, &crawler) == TL_INVALID && crawler == NULL);
    CHECK(crawl_create(invalid, 1, NULL, 0, &crawler) == TL_INVALID && crawler == NULL);
    const char *cleanup[] = {"/keep/a.txt",   "/sdk/lib.so", "/other/sdk2/x", "/.config/nvim/init",
                             "/.config/nvim", "/.config",    "/other/sdk2",   "/other",
                             "/sdk",          "/keep"};
    remove_all(root, cleanup, 10);
}
static void write_text(const char *root, const char *suffix, const char *text) {
    char path[160];
    snprintf(path, sizeof(path), "%s%s", root, suffix);
    FILE *file = fopen(path, "w");
    CHECK(file != NULL && fputs(text, file) >= 0 && fclose(file) == 0);
}
static bool covers(const tl_crawl *crawler, const char *root, const char *suffix) {
    char path[160];
    snprintf(path, sizeof(path), "%s%s", root, suffix);
    return crawl_covers(crawler, root, path);
}
/* Folders a tool manages are skipped with everything in them, by the markers
 * the tools write: a venv, a Conda environment, an Android SDK, a signed
 * cache tag and Go's module cache, and by names (site-packages). Look-alikes
 * stay: a pyvenv.cfg folder, an unsigned tag, a project's own "mod" package.
 * Coverage and scoped walks agree with the root walk; the allowlist and an
 * explicit root override the markers. */
static void tool_managed_folders(void) {
    char root[] = "/tmp/torchlight-managed-XXXXXX", path[160];
    CHECK(mkdtemp(root) != NULL);
    const char *dirs[] = {"/own",
                          "/own/mod",
                          "/own/mod/cache",
                          "/venv",
                          "/venv/lib",
                          "/conda",
                          "/conda/conda-meta",
                          "/Sdk",
                          "/Sdk/ndk",
                          "/go",
                          "/go/pkg",
                          "/go/pkg/mod",
                          "/go/pkg/mod/cache",
                          "/go/pkg/mod/cache/download",
                          "/tagged",
                          "/untagged",
                          "/fakevenv",
                          "/fakevenv/pyvenv.cfg",
                          "/lib",
                          "/lib/site-packages"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        make(root, dirs[i], true);
    const char *files[] = {"/own/notes.txt", "/own/mod/cache/x.go", "/venv/pyvenv.cfg",
                           "/venv/lib/x.py", "/Sdk/.knownPackages", "/lib/site-packages/a.py"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        make(root, files[i], false);
    write_text(root, "/tagged/CACHEDIR.TAG",
               "Signature: 8a477f597d28d172789f06886806bc55\n# a cache\n");
    write_text(root, "/untagged/CACHEDIR.TAG", "Signature: not a cache\n");
    tl_crawl *crawler = NULL;
    CHECK(crawl_create(NULL, 0, NULL, 0, &crawler) == TL_OK);
    struct listing listing = {0};
    CHECK(crawl_run(crawler, root, list_path, &listing) == TL_OK);
    const char *kept[] = {"",
                          "/own",
                          "/own/notes.txt",
                          "/own/mod",
                          "/own/mod/cache",
                          "/own/mod/cache/x.go",
                          "/go",
                          "/go/pkg",
                          "/untagged",
                          "/untagged/CACHEDIR.TAG",
                          "/fakevenv",
                          "/fakevenv/pyvenv.cfg",
                          "/lib"};
    CHECK(listing.count == sizeof(kept) / sizeof(kept[0]));
    for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
        CHECK(listed(&listing, root, kept[i]));
    CHECK(!covers(crawler, root, "/venv") && !covers(crawler, root, "/venv/lib") &&
          !covers(crawler, root, "/conda") && !covers(crawler, root, "/Sdk/ndk") &&
          !covers(crawler, root, "/go/pkg/mod") && !covers(crawler, root, "/tagged") &&
          !covers(crawler, root, "/lib/site-packages"));
    CHECK(covers(crawler, root, "/go/pkg") && covers(crawler, root, "/own/mod") &&
          covers(crawler, root, "/untagged") && covers(crawler, root, "/fakevenv/pyvenv.cfg"));
    /* A children-only scan of the root reports the same direct children. */
    listing = (struct listing){0};
    CHECK(crawl_scope(crawler, root, false, no_roots, &listing) == TL_OK);
    CHECK(listing.count == 6 && listed(&listing, root, "/own") && listed(&listing, root, "/go") &&
          listed(&listing, root, "/untagged") && listed(&listing, root, "/fakevenv") &&
          listed(&listing, root, "/lib") && !listed(&listing, root, "/venv"));
    snprintf(path, sizeof(path), "%s/go", root);
    listing = (struct listing){0};
    CHECK(crawl_scope(crawler, path, true, no_roots, &listing) == TL_OK);
    CHECK(listing.count == 2 && listed(&listing, root, "/go/pkg"));
    /* An explicit root is walked even when a tool manages it. */
    snprintf(path, sizeof(path), "%s/Sdk", root);
    listing = (struct listing){0};
    CHECK(crawl_run(crawler, path, list_path, &listing) == TL_OK);
    CHECK(listing.count == 3 && listed(&listing, root, "/Sdk/ndk"));
    crawl_destroy(crawler);
    /* An allowlisted managed folder is indexed like any other. */
    snprintf(path, sizeof(path), "%s/venv", root);
    const char *allow[] = {path};
    CHECK(crawl_create(NULL, 0, allow, 1, &crawler) == TL_OK);
    listing = (struct listing){0};
    CHECK(crawl_run(crawler, root, list_path, &listing) == TL_OK);
    CHECK(listed(&listing, root, "/venv/lib/x.py") && !listed(&listing, root, "/conda"));
    CHECK(covers(crawler, root, "/venv/lib"));
    crawl_destroy(crawler);
    const char *cleanup[] = {"/own/notes.txt",
                             "/own/mod/cache/x.go",
                             "/venv/pyvenv.cfg",
                             "/venv/lib/x.py",
                             "/Sdk/.knownPackages",
                             "/lib/site-packages/a.py",
                             "/tagged/CACHEDIR.TAG",
                             "/untagged/CACHEDIR.TAG",
                             "/lib/site-packages",
                             "/lib",
                             "/fakevenv/pyvenv.cfg",
                             "/fakevenv",
                             "/untagged",
                             "/tagged",
                             "/go/pkg/mod/cache/download",
                             "/go/pkg/mod/cache",
                             "/go/pkg/mod",
                             "/go/pkg",
                             "/go",
                             "/Sdk/ndk",
                             "/Sdk",
                             "/conda/conda-meta",
                             "/conda",
                             "/venv/lib",
                             "/venv",
                             "/own/mod/cache",
                             "/own/mod",
                             "/own"};
    remove_all(root, cleanup, sizeof(cleanup) / sizeof(cleanup[0]));
}
void test_crawl(void) {
    one_stat_records();
    tool_managed_folders();
    device_boundaries();
    scoped_walks();
    tl_crawl *crawler = NULL;
    CHECK(crawl_create(NULL, 0, NULL, 0, &crawler) == TL_OK);
    size_t count = 0;
    CHECK(crawl_run(crawler, "/a/torchlight/nonexistent/root", stop_scan, &count) == TL_IO &&
          count == 0);
    CHECK(crawl_run(crawler, ".", stop_scan, &count) == TL_LIMIT && count == 1);
    CHECK(crawl_run(crawler, ".", NULL, &count) == TL_INVALID);
    crawl_destroy(crawler);
    unreadable_directory_is_reported();
    allowlist_traversal();
    excluded_scopes();
    const char *invalid[] = {NULL};
    CHECK(crawl_create(NULL, 0, invalid, 1, &crawler) == TL_INVALID && crawler == NULL);
}
