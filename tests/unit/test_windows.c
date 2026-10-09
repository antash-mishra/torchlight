/* Window snapshots over a fake source, application evidence and title shortening. */
#include "test.h"
#include "torchlight/windows.h"
#include <stdlib.h>
#include <string.h>
/* Up to WINDOWS_MAX + 1 entries, so the fake can overfill a snapshot. */
struct fake_source {
    tl_window windows[WINDOWS_MAX + 1];
    size_t count;
    tl_status list_status;
    uint64_t activated, closed;
};
static tl_status fake_list(void *context, tl_window *out, size_t capacity, size_t *count) {
    struct fake_source *fake = context;
    *count = 0;
    if (fake->list_status != TL_OK)
        return fake->list_status;
    size_t copied = fake->count < capacity ? fake->count : capacity;
    memcpy(out, fake->windows, copied * sizeof(*out));
    *count = copied;
    return TL_OK;
}
static tl_status fake_activate(void *context, uint64_t handle) {
    struct fake_source *fake = context;
    if (handle == fake->closed)
        return TL_STATE;
    fake->activated = handle;
    return TL_OK;
}
static tl_window window(uint64_t handle, const char *instance, const char *class_name,
                        const char *app_id, const char *title) {
    tl_window result = {.handle = handle};
    CHECK(strlen(instance) < sizeof(result.instance) &&
          strlen(class_name) < sizeof(result.class_name));
    CHECK(strlen(app_id) < sizeof(result.app_id) && strlen(title) < sizeof(result.title));
    memcpy(result.instance, instance, strlen(instance) + 1);
    memcpy(result.class_name, class_name, strlen(class_name) + 1);
    memcpy(result.app_id, app_id, strlen(app_id) + 1);
    memcpy(result.title, title, strlen(title) + 1);
    return result;
}
static tl_windows *fake_windows(struct fake_source *fake) {
    tl_window_source source = {fake, fake_list, fake_activate, free};
    tl_windows *windows = NULL;
    CHECK(windows_create(&source, &windows) == TL_OK);
    CHECK(windows_refresh(windows) == TL_OK);
    return windows;
}
tl_windows *test_windows_fixture(const tl_window *windows, size_t count) {
    struct fake_source *fake = calloc(1, sizeof(*fake));
    CHECK(fake != NULL && count <= WINDOWS_MAX);
    memcpy(fake->windows, windows, count * sizeof(*windows));
    fake->count = count;
    return fake_windows(fake);
}
/* Without a window system there are never windows, and nothing can be activated. */
static void test_unsupported(void) {
    tl_windows *windows = NULL;
    CHECK(windows_create(NULL, NULL) == TL_INVALID);
    tl_window_source incomplete = {NULL, fake_list, NULL, free};
    CHECK(windows_create(&incomplete, &windows) == TL_INVALID && windows == NULL);
    CHECK(windows_create(NULL, &windows) == TL_OK);
    CHECK(windows_refresh(windows) == TL_OK && windows_count(windows) == 0);
    CHECK(windows_get(windows, 0) == NULL && windows_activate(windows, 0) == TL_INVALID);
    windows_destroy(windows);
    windows_destroy(NULL);
    CHECK(windows_refresh(NULL) == TL_INVALID && windows_count(NULL) == 0);
}
static void test_snapshot(void) {
    struct fake_source *fake = calloc(1, sizeof(*fake));
    CHECK(fake != NULL);
    fake->windows[0] = window(0x240000c, "google-chrome", "Google-chrome", "", "Docs");
    fake->windows[1] = window(0, "ghost", "Ghost", "", "dropped: zero handle");
    fake->windows[2] = window(0x3e00052, "code", "code", "", "bot - Visual Studio Code");
    fake->count = 3;
    fake->closed = 0x3e00052;
    tl_windows *windows = fake_windows(fake);
    CHECK(windows_count(windows) == 2);
    CHECK(windows_get(windows, 0)->handle == 0x240000c &&
          windows_get(windows, 1)->handle == 0x3e00052);
    CHECK(windows_get(windows, 2) == NULL);
    CHECK(windows_activate(windows, 0) == TL_OK && fake->activated == 0x240000c);
    CHECK(windows_activate(windows, 1) == TL_STATE && windows_activate(windows, 2) == TL_INVALID);
    /* A failed read leaves no stale windows behind. */
    fake->list_status = TL_IO;
    CHECK(windows_refresh(windows) == TL_IO && windows_count(windows) == 0);
    fake->list_status = TL_OK;
    for (size_t i = 0; i <= WINDOWS_MAX; i++)
        fake->windows[i] = window(i + 1, "many", "Many", "", "");
    fake->count = WINDOWS_MAX + 1;
    CHECK(windows_refresh(windows) == TL_OK && windows_count(windows) == WINDOWS_MAX);
    windows_destroy(windows);
}
/* Real WM_CLASS and desktop ids from a Cinnamon session. */
static void test_evidence(void) {
    tl_window chrome = window(1, "google-chrome", "Google-chrome", "", "");
    tl_window web_app = window(2, "crx_gnbkbajf", "Google-chrome", "", "");
    tl_window code = window(3, "code", "code", "", "");
    tl_window terminal =
        window(4, "gnome-terminal-server", "Gnome-terminal", "org.gnome.Terminal", "");
    tl_window zed = window(5, "dev.zed.Zed", "dev.zed.Zed", "", "");
    tl_window spaced = window(6, "My App", "My App", "", "");
    CHECK(windows_evidence(&chrome, "google-chrome.desktop", "google-chrome") ==
          WINDOWS_EVIDENCE_INSTANCE);
    /* Web-app windows share Chrome's class but differ in case: never Chrome's. */
    CHECK(windows_evidence(&web_app, "google-chrome.desktop", "google-chrome") ==
          WINDOWS_EVIDENCE_NONE);
    CHECK(windows_evidence(&web_app, "chrome-gnbkbajf-Default.desktop", "crx_gnbkbajf") ==
          WINDOWS_EVIDENCE_INSTANCE);
    /* VS Code declares StartupWMClass=Code but reports code. */
    CHECK(windows_evidence(&code, "code.desktop", "Code") == WINDOWS_EVIDENCE_DESKTOP_NAME);
    CHECK(windows_evidence(&terminal, "org.gnome.Terminal.desktop", "Gnome-terminal") ==
          WINDOWS_EVIDENCE_CLASS);
    CHECK(windows_evidence(&terminal, "org.gnome.Terminal.desktop", NULL) ==
          WINDOWS_EVIDENCE_APP_ID);
    CHECK(windows_evidence(&zed, "dev.zed.Zed.desktop", "") == WINDOWS_EVIDENCE_DESKTOP_NAME);
    CHECK(windows_evidence(&spaced, "my-app.desktop", NULL) == WINDOWS_EVIDENCE_DESKTOP_NAME);
    /* Names must match whole: no prefixes either way, and no bare suffix. */
    CHECK(windows_evidence(&code, "code-insiders.desktop", NULL) == WINDOWS_EVIDENCE_NONE);
    CHECK(windows_evidence(&code, "cod.desktop", NULL) == WINDOWS_EVIDENCE_NONE);
    CHECK(windows_evidence(&code, "code.desktop.desktop", NULL) == WINDOWS_EVIDENCE_NONE);
    tl_window empty = window(7, "", "", "", "");
    CHECK(windows_evidence(&empty, ".desktop", "") == WINDOWS_EVIDENCE_NONE);
    CHECK(windows_evidence(NULL, "code.desktop", NULL) == WINDOWS_EVIDENCE_NONE);
    CHECK(windows_evidence(&code, NULL, NULL) == WINDOWS_EVIDENCE_NONE);
}
static void expect_title(const char *title, const char *app, const char *expected) {
    char out[WINDOWS_TITLE_BYTES];
    windows_short_title(title, app, out, sizeof(out));
    if (strcmp(out, expected) != 0)
        fprintf(stderr, "short title %s -> %s, expected %s\n", title, out, expected);
    CHECK(strcmp(out, expected) == 0);
}
static void test_titles(void) {
    expect_title("Assembly Language - Algorithmica - Google Chrome", "Google Chrome",
                 "Assembly Language - Algorithmica");
    expect_title("Ming Lan EP24 | WeTV - Brave", "Brave Web Browser", "Ming Lan EP24 | WeTV");
    expect_title("Tab \xe2\x80\x94 Mozilla Firefox", "Firefox Web Browser", "Tab");
    expect_title("Inbox \xe2\x80\x93 Thunderbird", "Thunderbird Mail", "Inbox");
    expect_title("bot - Visual Studio Code", "Visual Studio Code", "bot");
    expect_title("Docs - Chrome", "Google Chrome", "Docs");
    expect_title("Notes - google chrome", "Google Chrome", "Notes");
    expect_title("torchlight \xe2\x80\x94 architecture.md", "Zed",
                 "torchlight \xe2\x80\x94 architecture.md");
    expect_title("Report - Chromebook", "Google Chrome", "Report - Chromebook");
    expect_title("Google Chrome", "Google Chrome", "Google Chrome");
    expect_title(" - Google Chrome", "Google Chrome", " - Google Chrome");
    expect_title("Docs - Google Chrome", NULL, "Docs - Google Chrome");
    expect_title("", "Google Chrome", "");
    char out[4] = "xyz";
    windows_short_title(NULL, "App", out, sizeof(out));
    CHECK(out[0] == 0);
    windows_short_title("h\xc3\xa9llo", "App", out, 3);
    CHECK(strcmp(out, "h") == 0);
    windows_short_title("h\xc3\xa9llo", "App", out, 4);
    CHECK(strcmp(out, "h\xc3\xa9") == 0);
    windows_short_title("title", "App", NULL, 4);
    windows_short_title("title", "App", out, 0);
}
void test_windows(void) {
    test_unsupported();
    test_snapshot();
    test_evidence();
    test_titles();
}
