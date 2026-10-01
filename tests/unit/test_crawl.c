/* Crawler error, unreadable-scope and callback propagation independently of storage. */
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
    CHECK(crawl_create(NULL, &crawler) == TL_OK);
    struct observed observed = {0};
    tl_status status = crawl_run(crawler, root, observe, &observed);
    crawl_destroy(crawler);
    CHECK(chmod(locked, 0700) == 0 && rmdir(locked) == 0 && rmdir(sibling) == 0 &&
          rmdir(root) == 0);
    CHECK(status == TL_OK && observed.entries == 4 && observed.unreadable == 1 && observed.sibling);
}
void test_crawl(void) {
    tl_crawl *crawler = NULL;
    CHECK(crawl_create(NULL, &crawler) == TL_OK);
    size_t count = 0;
    CHECK(crawl_run(crawler, "/a/torchlight/nonexistent/root", stop_scan, &count) == TL_IO &&
          count == 0);
    CHECK(crawl_run(crawler, ".", stop_scan, &count) == TL_LIMIT && count == 1);
    CHECK(crawl_run(crawler, ".", NULL, &count) == TL_INVALID);
    crawl_destroy(crawler);
    unreadable_directory_is_reported();
}
