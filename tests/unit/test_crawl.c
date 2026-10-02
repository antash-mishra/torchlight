/* Crawler errors, unreadable scopes, allowlist traversal and root coverage. */
#include "test.h"
#include "torchlight/crawl.h"
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
    CHECK(crawl_create(NULL, NULL, 0, &crawler) == TL_OK);
    struct observed observed = {0};
    tl_status status = crawl_run(crawler, root, observe, &observed);
    crawl_destroy(crawler);
    CHECK(chmod(locked, 0700) == 0 && rmdir(locked) == 0 && rmdir(sibling) == 0 &&
          rmdir(root) == 0);
    CHECK(status == TL_OK && observed.entries == 4 && observed.unreadable == 1 && observed.sibling);
}
struct listing {
    char paths[16][128];
    size_t count;
};
static tl_status list_path(void *context, const tl_crawl_entry *entry) {
    struct listing *listing = context;
    CHECK(listing->count < 16 && strlen(entry->path) < 128);
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
    CHECK(crawl_create(NULL, allow, 1, &crawler) == TL_OK);
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
void test_crawl(void) {
    tl_crawl *crawler = NULL;
    CHECK(crawl_create(NULL, NULL, 0, &crawler) == TL_OK);
    size_t count = 0;
    CHECK(crawl_run(crawler, "/a/torchlight/nonexistent/root", stop_scan, &count) == TL_IO &&
          count == 0);
    CHECK(crawl_run(crawler, ".", stop_scan, &count) == TL_LIMIT && count == 1);
    CHECK(crawl_run(crawler, ".", NULL, &count) == TL_INVALID);
    crawl_destroy(crawler);
    unreadable_directory_is_reported();
    allowlist_traversal();
    const char *invalid[] = {NULL};
    CHECK(crawl_create(NULL, invalid, 1, &crawler) == TL_INVALID && crawler == NULL);
}
