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
    popup_model_destroy(model);
}
