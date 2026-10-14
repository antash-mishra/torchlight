/* Window tracking for the Wayland window source: listing order by activation, readiness,
 * the launcher left out, names and titles, removal, growth and capacity. */
#include "../../ui/gtk/windows_track.h"
#include "test.h"
#include <string.h>
static uint64_t add(windows_track *track, void *data, const char *app_id, const char *title) {
    uint64_t id = 0;
    CHECK(windows_track_add(track, data, &id) == TL_OK && id != 0);
    windows_track_set_app_id(track, id, app_id);
    windows_track_set_title(track, id, title);
    windows_track_set_ready(track, id);
    return id;
}
/* The titles listed, joined by commas, for comparing orders at a glance. */
static const char *titles(const windows_track *track, size_t capacity) {
    static char joined[512];
    tl_window out[8];
    CHECK(capacity <= 8);
    size_t count = windows_track_list(track, "org.torchlight.Launcher", out, capacity);
    size_t used = 0;
    joined[0] = 0;
    for (size_t i = 0; i < count; i++) {
        int written =
            snprintf(joined + used, sizeof(joined) - used, "%s%s", i > 0 ? "," : "", out[i].title);
        CHECK(written >= 0 && (size_t)written < sizeof(joined) - used);
        used += (size_t)written;
    }
    return joined;
}
static void test_order(void) {
    windows_track *track = NULL;
    CHECK(windows_track_create(&track) == TL_OK);
    int notes = 1, mail = 2;
    uint64_t first = add(track, &notes, "org.example.Notes", "First");
    uint64_t second = add(track, &mail, "org.example.Mail", "Second");
    uint64_t third = add(track, NULL, "XLogo", "Third");
    /* Windows never seen active keep the compositor's order. */
    CHECK(strcmp(titles(track, 8), "First,Second,Third") == 0);
    windows_track_set_active(track, first, true);
    CHECK(strcmp(titles(track, 8), "First,Second,Third") == 0);
    windows_track_set_active(track, first, false);
    windows_track_set_active(track, second, true);
    CHECK(strcmp(titles(track, 8), "Second,First,Third") == 0);
    /* Staying active, or repeating the state, does not count as a new use. */
    windows_track_set_active(track, first, true);
    windows_track_set_active(track, second, true);
    CHECK(strcmp(titles(track, 8), "First,Second,Third") == 0);
    CHECK(strcmp(titles(track, 2), "First,Second") == 0);
    /* The launcher's own window is never listed, even when it is the active one. */
    uint64_t popup = add(track, NULL, "org.torchlight.Launcher", "Torchlight");
    windows_track_set_active(track, popup, true);
    CHECK(strcmp(titles(track, 8), "First,Second,Third") == 0);
    /* A window is listed once its first description is complete. */
    uint64_t fourth = 0;
    CHECK(windows_track_add(track, NULL, &fourth) == TL_OK && fourth > popup);
    windows_track_set_title(track, fourth, "Fourth");
    CHECK(strcmp(titles(track, 8), "First,Second,Third") == 0);
    windows_track_set_ready(track, fourth);
    CHECK(strcmp(titles(track, 8), "First,Second,Third,Fourth") == 0);
    CHECK(windows_track_find(track, second) == &mail && windows_track_count(track) == 5);
    CHECK(windows_track_remove(track, second) == &mail &&
          windows_track_find(track, second) == NULL);
    CHECK(windows_track_remove(track, second) == NULL && windows_track_count(track) == 4);
    CHECK(strcmp(titles(track, 8), "First,Third,Fourth") == 0);
    /* Updates for closed or unknown windows are ignored. */
    windows_track_set_active(track, second, true);
    windows_track_set_title(track, 999, "Ghost");
    CHECK(strcmp(titles(track, 8), "First,Third,Fourth") == 0);
    /* Activation reorders whatever the compositor's order was. */
    windows_track_set_active(track, third, true);
    CHECK(strcmp(titles(track, 8), "Third,First,Fourth") == 0);
    CHECK(third != 0);
    windows_track_destroy(track);
}
static void test_names(void) {
    windows_track *track = NULL;
    CHECK(windows_track_create(&track) == TL_OK);
    char long_id[WINDOWS_CLASS_BYTES + 8];
    memset(long_id, 'x', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = 0;
    char long_title[WINDOWS_TITLE_BYTES + 40];
    memset(long_title, 'a', sizeof(long_title) - 1);
    long_title[sizeof(long_title) - 1] = 0;
    /* "é" straddles the cut, so it is dropped whole. */
    memcpy(long_title + WINDOWS_TITLE_BYTES - 2, "\xc3\xa9", 2);
    add(track, NULL, "XLogo", "X logo");
    uint64_t notes = add(track, NULL, "org.example.Notes", "Shopping\nlist");
    add(track, NULL, long_id, long_title);
    tl_window out[4];
    CHECK(windows_track_list(track, NULL, out, 4) == 3);
    CHECK(out[0].handle != 0 && strcmp(out[0].class_name, "XLogo") == 0);
    /* An Xwayland window's app id is its WM_CLASS class, which StartupWMClass names. */
    CHECK(windows_evidence(&out[0], "xlogo.desktop", "XLogo") == WINDOWS_EVIDENCE_INSTANCE);
    CHECK(windows_evidence(&out[0], "xlogo.desktop", NULL) == WINDOWS_EVIDENCE_DESKTOP_NAME);
    CHECK(out[1].handle == notes && strcmp(out[1].title, "Shopping list") == 0);
    CHECK(windows_evidence(&out[1], "org.example.Notes.desktop", NULL) == WINDOWS_EVIDENCE_APP_ID);
    /* A name too long to compare is cleared; a long title is cut on a whole character. */
    CHECK(out[2].app_id[0] == 0 && out[2].class_name[0] == 0 && out[2].instance[0] == 0);
    CHECK(strlen(out[2].title) == WINDOWS_TITLE_BYTES - 2 && out[2].title[0] == 'a');
    windows_track_destroy(track);
}
static void test_growth(void) {
    windows_track *track = NULL;
    CHECK(windows_track_add(NULL, NULL, &(uint64_t){0}) == TL_INVALID);
    CHECK(windows_track_create(NULL) == TL_INVALID);
    CHECK(windows_track_create(&track) == TL_OK && windows_track_count(track) == 0);
    CHECK(windows_track_add(track, NULL, NULL) == TL_INVALID);
    enum { MANY = WINDOWS_MAX * 2 + 3 };
    static int data[MANY];
    for (int i = 0; i < MANY; i++)
        add(track, &data[i], "org.example.Many", "Many");
    CHECK(windows_track_count(track) == MANY);
    /* Every window can be visited, for closing them all. */
    size_t seen = 0;
    for (size_t i = 0; windows_track_data_at(track, i) != NULL; i++)
        seen++;
    CHECK(seen == MANY && windows_track_data_at(track, MANY) == NULL);
    static tl_window out[WINDOWS_MAX];
    CHECK(windows_track_list(track, NULL, out, WINDOWS_MAX) == WINDOWS_MAX);
    /* In the order added, without repeats. */
    for (size_t i = 1; i < WINDOWS_MAX; i++)
        CHECK(out[i].handle > out[i - 1].handle);
    CHECK(windows_track_list(NULL, NULL, out, WINDOWS_MAX) == 0);
    CHECK(windows_track_list(track, NULL, NULL, WINDOWS_MAX) == 0);
    windows_track_destroy(track);
    windows_track_destroy(NULL);
}
void test_windows_track(void) {
    test_order();
    test_names();
    test_growth();
}
