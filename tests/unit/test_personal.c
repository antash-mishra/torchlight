/* Search-thread personal state: remembered searches, once-per-event opens,
 * file boosts and their mapping keys, application boosts across desktop
 * changes, startup adoption and clearing. */
#include "test.h"
#include "torchlight/personal.h"
#include <glib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
enum { APP_PATH_BYTES = 256, APP_CREATED_MAX = 8 };
/* Create path and its missing parents, recording each directory created
 * (outermost first) so cleanup removes exactly those. */
static size_t create_missing(const char *path, char (*created)[APP_PATH_BYTES]) {
    if (g_file_test(path, G_FILE_TEST_IS_DIR))
        return 0;
    char *parent = g_path_get_dirname(path);
    size_t count = create_missing(parent, created);
    g_free(parent);
    CHECK(count < APP_CREATED_MAX && mkdir(path, 0700) == 0);
    CHECK(snprintf(created[count], APP_PATH_BYTES, "%s", path) > 0);
    return count + 1;
}
static void write_app(const char *applications, const char *id, const char *name,
                      const char *extra) {
    char path[APP_PATH_BYTES];
    CHECK(snprintf(path, sizeof(path), "%s/%s", applications, id) > 0);
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    CHECK(fprintf(file, "[Desktop Entry]\nType=Application\nExec=/bin/true\nName=%s\n%s", name,
                  extra) > 0);
    CHECK(fclose(file) == 0);
}
/* Sorted-id position of desktop_id in the leased snapshot. */
static size_t position_of(const tl_desktop *desktop, const char *desktop_id) {
    for (size_t i = 0; i < desktop_count(desktop); i++)
        if (strcmp(desktop_entry(desktop, i)->desktop_id, desktop_id) == 0)
            return i;
    CHECK(false);
    return SIZE_MAX;
}
/* Desktop id of the first application for "s" with these boosts (leased). */
static const char *first_app(tl_desktop *desktop, const tl_lexical_boost *boosts, size_t count) {
    tl_result results[10];
    size_t found = 0;
    CHECK(desktop_query_boosted(desktop, "s", boosts, count, results, 10, &found) == TL_OK);
    CHECK(found >= 1);
    return desktop_resolve(desktop, results[0].id)->desktop_id;
}
/* The one installed application opened from "s" gets its boost at position:
 * 133 for the open plus 600 for its query. The uninstalled one gets none. */
static void check_app_boost(tl_personal *personal, tl_desktop *desktop, size_t position) {
    tl_catalog_boosts files;
    const tl_lexical_boost *apps = NULL;
    size_t count = 0;
    CHECK(personal_boosts(personal, desktop, "s", NOW, &files, &apps, &count) == TL_OK);
    CHECK(files.count == 0 && count == 1);
    CHECK(apps[0].position == position && apps[0].boost == 733);
    CHECK(strcmp(first_app(desktop, apps, count), "shell.desktop") == 0);
}
/* Request a desktop refresh and wait until the snapshot after gen is live. */
static uint64_t wait_for_desktop(tl_desktop *desktop, uint64_t gen) {
    const struct timespec pause = {.tv_nsec = 10000000};
    uint64_t current = gen;
    desktop_refresh(desktop);
    for (size_t i = 0; i < 500 && current == gen; i++) {
        desktop_acquire(desktop);
        current = desktop_gen(desktop);
        desktop_release(desktop);
        if (current == gen)
            CHECK(nanosleep(&pause, NULL) == 0);
    }
    CHECK(current != gen);
    return current;
}
/* Applications are boosted by desktop id at their current position in the
 * leased snapshot. Editing an application's file gives it a new session id,
 * which sorts last, and the boost follows it; a removed application gets no
 * boost and lends none to the entry that takes its place. */
static void application_boosts(void) {
    char temporary[] = "/tmp/torchlight-personal-XXXXXX";
    CHECK(mkdtemp(temporary) != NULL);
    char system[APP_PATH_BYTES], created[APP_CREATED_MAX][APP_PATH_BYTES];
    CHECK(snprintf(system, sizeof(system), "%s/system", temporary) > 0 && mkdir(system, 0700) == 0);
    /* GLib resolves the XDG data directories once per process: these apply
     * when this test runs alone, otherwise test_desktop's (since removed)
     * stay in effect. Applications go wherever GLib looks either way. */
    CHECK(setenv("XDG_DATA_HOME", temporary, 1) == 0 && setenv("XDG_DATA_DIRS", system, 1) == 0);
    char *applications = g_build_filename(g_get_user_data_dir(), "applications", NULL);
    size_t created_count = create_missing(applications, created);
    static const char *const ids[] = {"screenshot.desktop", "settings.desktop", "shell.desktop",
                                      "sound.desktop"};
    static const char *const names[] = {"Screenshot", "Settings", "Shell Terminal", "Sound"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        write_app(applications, ids[i], names[i], "");
    tl_desktop *desktop = NULL;
    tl_personal *personal = NULL;
    CHECK(desktop_create(&desktop) == TL_OK && personal_create(RETENTION, &personal) == TL_OK);
    tl_usage_target shell = {0, "shell.desktop"}, uninstalled = {0, "uninstalled.desktop"};
    CHECK(personal_record(personal, "launch", shell, "s", NOW) == TL_OK);
    CHECK(personal_record(personal, "gone", uninstalled, "s", NOW) == TL_OK);
    desktop_acquire(desktop);
    CHECK(strcmp(first_app(desktop, NULL, 0), "shell.desktop") != 0);
    size_t before = position_of(desktop, "shell.desktop");
    check_app_boost(personal, desktop, before);
    uint64_t gen = desktop_gen(desktop);
    desktop_release(desktop);
    write_app(applications, "shell.desktop", "Shell Terminal", "Comment=edited\n");
    gen = wait_for_desktop(desktop, gen);
    desktop_acquire(desktop);
    size_t after = position_of(desktop, "shell.desktop");
    CHECK(after != before);
    check_app_boost(personal, desktop, after);
    desktop_release(desktop);
    char path[APP_PATH_BYTES];
    CHECK(snprintf(path, sizeof(path), "%s/shell.desktop", applications) > 0 && unlink(path) == 0);
    wait_for_desktop(desktop, gen);
    desktop_acquire(desktop);
    tl_catalog_boosts files;
    const tl_lexical_boost *apps = NULL;
    size_t count = 99;
    CHECK(personal_boosts(personal, desktop, "s", NOW, &files, &apps, &count) == TL_OK);
    CHECK(count == 0);
    desktop_release(desktop);
    personal_destroy(personal);
    desktop_destroy(desktop);
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        CHECK(snprintf(path, sizeof(path), "%s/%s", applications, ids[i]) > 0);
        CHECK(strcmp(ids[i], "shell.desktop") == 0 || unlink(path) == 0);
    }
    for (size_t i = created_count; i-- > 0;)
        CHECK(rmdir(created[i]) == 0);
    g_free(applications);
    CHECK(rmdir(system) == 0 && (access(temporary, F_OK) != 0 || rmdir(temporary) == 0));
    CHECK(unsetenv("XDG_DATA_HOME") == 0 && unsetenv("XDG_DATA_DIRS") == 0);
}
void test_personal(void) {
    remembered_searches();
    busy_clients();
    opens_and_boosts();
    application_boosts();
    adoption_and_clearing();
}
