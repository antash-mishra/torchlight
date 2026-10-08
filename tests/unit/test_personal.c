/* Search-thread personal state: remembered searches, once-per-event opens,
 * file boosts and their mapping keys, startup adoption and clearing. */
#include "test.h"
#include "torchlight/personal.h"
#include <string.h>

enum { DAY = 24 * 3600, RETENTION = 30 * DAY, NOW = 2000000 };

static int file_boost(const tl_catalog_boosts *files, uint64_t id) {
    for (size_t i = 0; i < files->count; i++)
        if (files->ids[i] == id)
            return files->values[i];
    return 0;
}
static tl_catalog_boosts boosts_for(tl_personal *personal, const char *query) {
    tl_catalog_boosts files;
    const tl_lexical_boost *apps = NULL;
    size_t app_count = 99;
    CHECK(personal_boosts(personal, NULL, query, NOW, &files, &apps, &app_count) == TL_OK);
    CHECK(apps == NULL && app_count == 0);
    return files;
}
static void remembered_searches(void) {
    tl_personal *personal = NULL;
    CHECK(personal_create(0, &personal) == TL_INVALID && personal == NULL);
    CHECK(personal_create(RETENTION, &personal) == TL_OK);
    personal_remember(personal, "s1", "chrome");
    CHECK(strcmp(personal_query_of(personal, "s1"), "chrome") == 0);
    CHECK(personal_query_of(personal, "s2") == NULL && personal_query_of(personal, "") == NULL);
    char id[16];
    for (int i = 0; i < PERSONAL_RECENT_SEARCHES; i++) {
        snprintf(id, sizeof(id), "n%d", i);
        personal_remember(personal, id, "x");
    }
    CHECK(personal_query_of(personal, "s1") == NULL && personal_query_of(personal, "n0") != NULL);
    personal_remember(personal, NULL, "x");
    /* Oversized input is ignored without touching the oldest search (n0 sits
     * in the slot the next search overwrites). Regression: it was blanked. */
    char oversized[LEXICAL_QUERY_BYTES + 2];
    memset(oversized, 'q', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = 0;
    personal_remember(personal, "big", oversized);
    CHECK(personal_query_of(personal, "big") == NULL && personal_query_of(personal, "n0") != NULL);
    personal_destroy(personal);
    personal_destroy(NULL);
}
/* The popup opens a result on its own connection after its latest search,
 * while other clients may keep searching: an open still finds its search's
 * query after a couple of hundred searches from anyone. Regression: 32
 * shared slots lost the query link (and the search row) to a busy client. */
static void busy_clients(void) {
    enum { OTHER_SEARCHES = 200 };
    tl_personal *personal = NULL;
    CHECK(personal_create(RETENTION, &personal) == TL_OK);
    personal_remember(personal, "popup:1", "chap");
    char id[32];
    for (int i = 0; i < OTHER_SEARCHES; i++) {
        snprintf(id, sizeof(id), "script:%d", i);
        personal_remember(personal, id, "x");
    }
    const char *query = personal_query_of(personal, "popup:1");
    CHECK(query != NULL && strcmp(query, "chap") == 0);
    personal_destroy(personal);
}
static void opens_and_boosts(void) {
    tl_personal *personal = NULL;
    CHECK(personal_create(RETENTION, &personal) == TL_OK);
    tl_catalog_boosts empty = boosts_for(personal, "c");
    CHECK(empty.count == 0 && personal_items(personal) == 0);
    tl_usage_target chapter = {7, NULL}, firefox = {0, "firefox.desktop"};
    CHECK(personal_record(personal, "e1", chapter, "chap", NOW) == TL_OK);
    CHECK(personal_record(personal, "e1", chapter, "chap", NOW) == TL_OK); /* retried */
    CHECK(personal_record(personal, "e2", firefox, NULL, NOW) == TL_OK);
    CHECK(personal_record(personal, "", chapter, NULL, NOW) == TL_INVALID);
    CHECK(personal_items(personal) == 2);
    tl_catalog_boosts files = boosts_for(personal, "c");
    /* One open (retry counted once) plus its query: 133 + 600. Apps never
     * appear among file boosts. */
    CHECK(files.count == 1 && file_boost(&files, 7) == 733);
    uint64_t key = files.key;
    files = boosts_for(personal, "x");
    CHECK(file_boost(&files, 7) == 133 && files.key == key);
    CHECK(personal_record(personal, "e3", (tl_usage_target){8, NULL}, NULL, NOW) == TL_OK);
    files = boosts_for(personal, "x");
    CHECK(files.count == 2 && files.key != key);
    personal_destroy(personal);
}
/* The startup summary absorbs opens recorded meanwhile; after a clear it is
 * stale and discarded. Keys never repeat across the swap. */
static void adoption_and_clearing(void) {
    tl_personal *personal = NULL;
    CHECK(personal_create(RETENTION, &personal) == TL_OK);
    CHECK(personal_record(personal, "live", (tl_usage_target){1, NULL}, NULL, NOW) == TL_OK);
    uint64_t key = boosts_for(personal, "").key;
    tl_usage *loaded = NULL;
    CHECK(usage_create(RETENTION, &loaded) == TL_OK);
    CHECK(usage_record(loaded, (tl_usage_target){1, NULL}, NULL, NOW - DAY) == TL_OK);
    CHECK(usage_record(loaded, (tl_usage_target){2, NULL}, NULL, NOW - DAY) == TL_OK);
    personal_adopt(personal, loaded);
    CHECK(personal_items(personal) == 2);
    tl_catalog_boosts files = boosts_for(personal, "");
    CHECK(files.key != key && files.count == 2);
    CHECK(file_boost(&files, 1) > file_boost(&files, 2));
    personal_clear(personal);
    CHECK(personal_items(personal) == 0 && boosts_for(personal, "").count == 0);
    CHECK(usage_create(RETENTION, &loaded) == TL_OK);
    CHECK(usage_record(loaded, (tl_usage_target){3, NULL}, NULL, NOW) == TL_OK);
    personal_adopt(personal, loaded); /* predates the clear: discarded */
    CHECK(personal_items(personal) == 0);
    personal_adopt(personal, NULL);
    personal_adopt(NULL, NULL);
    personal_destroy(personal);
}
void test_personal(void) {
    remembered_searches();
    busy_clients();
    opens_and_boosts();
    adoption_and_clearing();
}
