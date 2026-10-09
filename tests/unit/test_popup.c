/* Selection retention, stale phases, exact byte paths and malformed result checks. */
#include "test.h"
#include "torchlight/popup.h"
#include <string.h>
static void apply(tl_popup_model *model, const char *response) {
    CHECK(popup_model_apply(model, response, strlen(response)) == TL_OK);
}
static void test_cleared(tl_popup_model *model, const char *response) {
    CHECK(popup_model_count(model) != 0 && popup_model_selected(model) != NULL);
    CHECK(popup_model_indexing(model) && popup_model_degraded(model));
    CHECK(strcmp(popup_model_search_id(model), "byte-search") == 0);
    popup_model_clear(model);
    CHECK(popup_model_count(model) == 0 && popup_model_position(model) == 0);
    CHECK(popup_model_row(model, 0) == NULL && popup_model_selected(model) == NULL);
    CHECK(!popup_model_indexing(model) && !popup_model_degraded(model));
    CHECK(popup_model_search_id(model)[0] == 0);
    CHECK(popup_model_apply(model, response, strlen(response)) == TL_STATE);
    popup_model_move(model, 1);
    CHECK(popup_model_selected(model) == NULL);
    CHECK(popup_model_begin(model, "2") == TL_OK);
    apply(model, response);
    CHECK(popup_model_count(model) == 1 && popup_model_selected(model)->id == 4);
    popup_model_clear(NULL);
}
/* Same-named rows of the same kind keep the folder where their parents first differ. */
static void test_distinct(tl_popup_model *model) {
    const char *response =
        "{\"version\":1,\"request_id\":\"3\",\"phase\":\"final\",\"status\":\"ok\",\"results\":["
        "{\"id\":\"1\",\"kind\":\"file\",\"display\":\"/a/ndk/27.0/tool/set\",\"path\":\"/1\"},"
        "{\"id\":\"2\",\"kind\":\"file\",\"display\":\"/a/ndk/27.1/tool/set\",\"path\":\"/2\"},"
        "{\"id\":\"3\",\"kind\":\"folder\",\"display\":\"/b/set\",\"path\":\"/3\"},"
        "{\"id\":\"4\",\"kind\":\"file\",\"display\":\"/x/y/z/n\",\"path\":\"/4\"},"
        "{\"id\":\"5\",\"kind\":\"file\",\"display\":\"/x/yy/z/n\",\"path\":\"/5\"},"
        "{\"id\":\"6\",\"kind\":\"file\",\"display\":\"/p/q/m\",\"path\":\"/6\"},"
        "{\"id\":\"7\",\"kind\":\"file\",\"display\":\"/p/q/r/m\",\"path\":\"/7\"},"
        "{\"id\":\"8\",\"kind\":\"application\",\"display\":\"/apps/set.desktop\",\"path\":\"/8\","
        "\"name\":\"set\",\"desktop_id\":\"set.desktop\",\"desktop_revision\":\"1\"},"
        "{\"id\":\"9\",\"kind\":\"file\",\"display\":\"/solo/file\",\"path\":\"/9\"}]}";
    CHECK(popup_model_begin(model, "3") == TL_OK);
    apply(model, response);
    CHECK(popup_model_distinct_prefix(model, 0) == strlen("/a/ndk/27.0"));
    CHECK(popup_model_distinct_prefix(model, 1) == strlen("/a/ndk/27.1"));
    CHECK(popup_model_distinct_prefix(model, 2) == 0); /* folders never collide with files */
    CHECK(popup_model_distinct_prefix(model, 3) == strlen("/x/y"));
    CHECK(popup_model_distinct_prefix(model, 4) == strlen("/x/yy"));
    CHECK(popup_model_distinct_prefix(model, 5) == strlen("/p/q"));
    CHECK(popup_model_distinct_prefix(model, 6) == strlen("/p/q/r"));
    CHECK(popup_model_distinct_prefix(model, 7) == 0);
    CHECK(popup_model_distinct_prefix(model, 8) == 0);
    CHECK(popup_model_distinct_prefix(model, 99) == 0);
    CHECK(popup_model_distinct_prefix(NULL, 0) == 0);
}
/* Rows for "chrome": Chrome, a file, VS Code, a settings panel and a Chrome web app. */
#define HIERARCHY_ROWS(phase, first, second)                                                       \
    "{\"version\":1,\"request_id\":\"7\",\"phase\":\"" phase                                       \
    "\",\"status\":\"ok\",\"results\":[" first "," second ","                                      \
    "{\"id\":\"13\",\"kind\":\"application\",\"display\":\"/apps/code.desktop\","                  \
    "\"path\":\"/apps/code.desktop\",\"name\":\"Visual Studio Code\","                             \
    "\"desktop_id\":\"code.desktop\",\"desktop_revision\":\"1\",\"wm_class\":\"Code\"},"           \
    "{\"id\":\"14\",\"kind\":\"settings\",\"display\":\"/apps/display.desktop\","                  \
    "\"path\":\"/apps/display.desktop\",\"name\":\"Display\","                                     \
    "\"desktop_id\":\"display.desktop\",\"desktop_revision\":\"1\"},"                              \
    "{\"id\":\"15\",\"kind\":\"application\",\"display\":\"/apps/crx.desktop\","                   \
    "\"path\":\"/apps/crx.desktop\",\"name\":\"Web App\",\"desktop_id\":\"crx.desktop\","          \
    "\"desktop_revision\":\"1\",\"wm_class\":\"crx_app\"}]}"
#define HIERARCHY_CHROME                                                                           \
    "{\"id\":\"11\",\"kind\":\"application\",\"display\":\"/apps/google-chrome.desktop\","         \
    "\"path\":\"/apps/google-chrome.desktop\",\"name\":\"Google Chrome\","                         \
    "\"desktop_id\":\"google-chrome.desktop\",\"desktop_revision\":\"1\","                         \
    "\"wm_class\":\"google-chrome\"}"
#define HIERARCHY_FILE                                                                             \
    "{\"id\":\"12\",\"kind\":\"file\",\"display\":\"/tmp/chrome.md\",\"path\":\"/tmp/chrome.md\"}"
static tl_window hierarchy_window(uint64_t handle, const char *instance, const char *class_name) {
    tl_window window = {.handle = handle};
    memcpy(window.instance, instance, strlen(instance) + 1);
    memcpy(window.class_name, class_name, strlen(class_name) + 1);
    return window;
}
/* Eight Chrome windows (most recent first), one VS Code, one web app and one window
 * whose instance names the settings panel, which never owns windows. */
static tl_windows *hierarchy_windows(bool with_first) {
    tl_window windows[11];
    size_t count = 0;
    if (with_first)
        windows[count++] = hierarchy_window(100, "google-chrome", "Google-chrome");
    windows[count++] = hierarchy_window(200, "code", "code");
    windows[count++] = hierarchy_window(101, "google-chrome", "Google-chrome");
    windows[count++] = hierarchy_window(300, "crx_app", "Google-chrome");
    windows[count++] = hierarchy_window(400, "display", "Display");
    for (uint64_t handle = 102; handle <= 107; handle++)
        windows[count++] = hierarchy_window(handle, "google-chrome", "Google-chrome");
    return test_windows_fixture(windows, count);
}
static void expect_item(const tl_popup_model *model, size_t index, tl_popup_item_kind kind,
                        size_t row, uint64_t handle) {
    const tl_popup_item *item = popup_model_item(model, index);
    CHECK(item != NULL && item->kind == kind && item->row == row && item->handle == handle);
}
static void select_item(tl_popup_model *model, size_t index) {
    while (popup_model_position(model) < index)
        popup_model_move(model, 1);
    while (popup_model_position(model) > index)
        popup_model_move(model, -1);
    CHECK(popup_model_position(model) == index);
}
/* Every application lists its windows wherever it ranks: three, more windows
 * and new window for Chrome; VS Code and the web app at rows 2 and 4 too. */
static void expect_chrome_children(const tl_popup_model *model) {
    const tl_popup_item *chrome = popup_model_item(model, 0);
    CHECK(chrome->kind == POPUP_ITEM_RESULT && chrome->windows == 8 && chrome->expanded);
    CHECK(chrome->window == 0 && popup_model_count(model) == 5);
    const uint64_t shown[] = {100, 101, 102};
    for (size_t i = 0; i < 3; i++)
        expect_item(model, i + 1, POPUP_ITEM_WINDOW, 0, shown[i]);
    expect_item(model, 4, POPUP_ITEM_MORE_WINDOWS, 0, 0);
    CHECK(popup_model_item(model, 4)->windows == 5);
    expect_item(model, 5, POPUP_ITEM_NEW_WINDOW, 0, 0);
    expect_item(model, 6, POPUP_ITEM_RESULT, 1, 0);
    const tl_popup_item *code = popup_model_item(model, 7);
    CHECK(code->kind == POPUP_ITEM_RESULT && code->windows == 1 && code->expanded);
    expect_item(model, 8, POPUP_ITEM_WINDOW, 2, 200);
    expect_item(model, 9, POPUP_ITEM_NEW_WINDOW, 2, 0);
    CHECK(popup_model_item(model, 10)->windows == 0); /* settings */
    CHECK(popup_model_item(model, 11)->windows == 1 && popup_model_item(model, 11)->expanded);
    expect_item(model, 12, POPUP_ITEM_WINDOW, 4, 300);
    expect_item(model, 13, POPUP_ITEM_NEW_WINDOW, 4, 0);
    CHECK(popup_model_item_count(model) == 14);
}
static void test_expansion(tl_popup_model *model) {
    select_item(model, 4);
    CHECK(popup_model_expand(model));
    CHECK(popup_model_item_count(model) == 18 && popup_model_item(model, 4)->handle == 103);
    CHECK(popup_model_selected_item(model)->handle == 103);
    expect_item(model, 9, POPUP_ITEM_NEW_WINDOW, 0, 0);
    /* Collapsing from a child selects its application. */
    CHECK(popup_model_collapse(model) && popup_model_position(model) == 0);
    CHECK(popup_model_item_count(model) == 9 && !popup_model_collapse(model));
    CHECK(popup_model_expand(model) && popup_model_item_count(model) == 14);
    CHECK(!popup_model_expand(model) && popup_model_position(model) == 0);
    select_item(model, 6);
    CHECK(!popup_model_expand(model)); /* files have no windows */
    /* A lower-ranked application collapses and expands in place. */
    select_item(model, 7);
    CHECK(!popup_model_expand(model) && popup_model_collapse(model));
    CHECK(popup_model_position(model) == 7 && popup_model_item_count(model) == 12);
    CHECK(popup_model_expand(model) && popup_model_position(model) == 7);
    expect_item(model, 8, POPUP_ITEM_WINDOW, 2, 200);
    expect_item(model, 9, POPUP_ITEM_NEW_WINDOW, 2, 0);
    select_item(model, 10);
    CHECK(!popup_model_expand(model) && !popup_model_collapse(model));
}
static void test_hierarchy(tl_popup_model *model) {
    const char *first = HIERARCHY_ROWS("lexical", HIERARCHY_CHROME, HIERARCHY_FILE);
    CHECK(popup_model_begin(model, "7") == TL_OK);
    apply(model, first);
    CHECK(popup_model_item_count(model) == 5 && !popup_model_item(model, 0)->expanded);
    CHECK(strcmp(popup_model_row(model, 0)->wm_class, "google-chrome") == 0);
    CHECK(popup_model_row(model, 1)->wm_class[0] == 0);
    tl_windows *windows = hierarchy_windows(true);
    popup_model_set_windows(model, windows);
    expect_chrome_children(model);
    CHECK(popup_model_selected_item(model)->kind == POPUP_ITEM_RESULT);
    popup_model_move(model, 1);
    CHECK(popup_model_selected_item(model)->handle == 100);
    CHECK(popup_model_selected(model)->id == 11);
    /* A final phase that reorders results keeps the selected window. */
    apply(model, HIERARCHY_ROWS("final", HIERARCHY_FILE, HIERARCHY_CHROME));
    expect_chrome_children(model);
    CHECK(popup_model_position(model) == 1 && popup_model_selected_item(model)->handle == 100);
    /* The selected window closed: its application stays selected. */
    tl_windows *later = hierarchy_windows(false);
    popup_model_set_windows(model, later);
    CHECK(popup_model_position(model) == 0 && popup_model_item(model, 0)->windows == 7);
    popup_model_set_windows(model, windows);
    test_expansion(model);
    /* Choices last one request; the next starts from the automatic layout. */
    CHECK(popup_model_begin(model, "7") == TL_OK);
    apply(model, first);
    expect_chrome_children(model);
    popup_model_set_windows(model, NULL);
    CHECK(popup_model_item_count(model) == 5 && popup_model_item(model, 0)->windows == 0);
    popup_model_clear(model);
    CHECK(popup_model_item_count(model) == 0 && popup_model_item(model, 0) == NULL);
    CHECK(popup_model_selected_item(model) == NULL && !popup_model_expand(model));
    CHECK(popup_model_item_count(NULL) == 0 && popup_model_selected_item(NULL) == NULL);
    popup_model_set_windows(NULL, NULL);
    windows_destroy(later);
    windows_destroy(windows);
}
void test_popup(void) {
    tl_popup_model *model = NULL;
    CHECK(popup_model_create(&model) == TL_OK);
    CHECK(popup_model_begin(model, "1") == TL_OK);
    const char *first =
        "{\"version\":1,\"request_id\":\"1\",\"phase\":\"lexical\",\"status\":\"ok\","
        "\"search_id\":\"search\",\"results\":[{\"id\":\"1\",\"display\":\"/tmp/one\",\"path\":\"/"
        "tmp/one\"},"
        "{\"id\":\"2\",\"display\":\"/tmp/two\",\"path\":\"/tmp/two\"}]}";
    apply(model, first);
    CHECK(popup_model_selected(model)->id == 1);
    popup_model_move(model, 1);
    CHECK(popup_model_selected(model)->id == 2 && popup_model_position(model) == 1);
    const char *final =
        "{\"version\":1,\"request_id\":\"1\",\"phase\":\"final\",\"status\":\"ok\","
        "\"results\":[{\"id\":\"3\",\"display\":\"/tmp/three\",\"path\":\"/tmp/three\"},"
        "{\"id\":\"1\",\"display\":\"/tmp/one\",\"path\":\"/tmp/one\"}]}";
    apply(model, final);
    CHECK(popup_model_count(model) == 3 && popup_model_selected(model)->id == 2);
    CHECK(popup_model_position(model) == 1 && popup_model_row(model, 0)->id == 3);
    CHECK(popup_model_begin(model, "2") == TL_OK);
    CHECK(popup_model_selected(model) == NULL);
    CHECK(popup_model_apply(model, first, strlen(first)) == TL_STATE);
    const char *bytes =
        "{\"version\":1,\"request_id\":\"2\",\"phase\":\"final\",\"status\":\"ok\","
        "\"search_id\":\"byte-search\",\"indexing\":{\"active\":true,\"degraded\":true},"
        "\"results\":[{\"id\":\"4\",\"display\":\"/tmp/a\\nb\",\"path_b64\":\"L3RtcC9h/wo=\"}]}";
    apply(model, bytes);
    CHECK(strcmp(popup_model_selected(model)->path, "/tmp/a\xff\n") == 0);
    CHECK(strcmp(popup_model_selected(model)->name, "a\\x0ab") == 0);
    const char *nul = "{\"version\":1,\"request_id\":\"2\",\"phase\":\"final\",\"status\":\"ok\","
                      "\"results\":[{\"id\":\"4\",\"display\":\"x\",\"path_b64\":\"L3RtcC8A\"}]}";
    CHECK(popup_model_apply(model, nul, strlen(nul)) == TL_INVALID);
    CHECK(popup_model_selected(model)->id == 4);
    test_cleared(model, bytes);
    test_distinct(model);
    test_hierarchy(model);
    popup_model_destroy(model);
}
