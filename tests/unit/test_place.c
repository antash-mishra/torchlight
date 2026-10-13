/* Place prior: dependency markers, level counting, inheritance and the cap. */
#include "test.h"
#include "torchlight/place.h"
static bool marker(const char *name) {
    tl_tokenized *text = test_text(name);
    bool result = place_dependency_name(tokenize_view(text));
    tokenize_destroy(text);
    return result;
}
static tl_place child(tl_place parent, const char *name, bool usable) {
    tl_tokenized *text = test_text(name);
    tl_place result = place_child(parent, tokenize_view(text), usable);
    tokenize_destroy(text);
    return result;
}
static void markers(void) {
    const char *yes[] = {"node_modules",  "bower_components",
                         "site-packages", "dist-packages",
                         "vendor",        "third_party",
                         "third-party",   "thirdparty",
                         "sysroot",       "sdk",
                         "Sdk",           "SDK",
                         "Vendor",        "go-urn@v1.4.0",
                         "x@v0.0.0-2025", "@v",
                         "27.0.12077973", "1.4.335.0",
                         "v1.2",          "V10.0.1",
                         "3.11"};
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
        CHECK(marker(yes[i]));
    /* Near misses: dates, bare numbers, partial names, other '@' uses. */
    const char *no[] = {"2024.05.01", "2024",    "v1",        "1.",         ".1",
                        "1..2",       "1.2a",    "sdkfoo",    "mysdk",      "vendors",
                        "@",          "a@v",     "a@vx",      "me@home",    "workspace",
                        "src",        "scripts", "vulkansdk", "android-35", "python3.11"};
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++)
        CHECK(!marker(no[i]));
    CHECK(!place_dependency_name((tl_text){0}));
}
static void levels(void) {
    tl_place top = {0};
    /* Directories above the indexed root add nothing: /home for ~. */
    tl_place home = child(top, "home", false);
    CHECK(home.level == 0 && !home.dependency);
    tl_place root = child(home, "antash", true);
    CHECK(root.level == 1 && place_penalty(root) == 0);
    tl_place deep = root;
    const char *names[] = {"workspace", "torchlight", "src", "index"};
    for (size_t i = 0; i < 4; i++)
        deep = child(deep, names[i], true);
    /* ~/workspace/torchlight/src/index/x: depth 4, one level beyond free. */
    CHECK(deep.level == 5 && place_penalty(deep) == PLACE_DEPTH_PENALTY);
    tl_place free_depth = child(child(child(root, "a", true), "b", true), "c", true);
    CHECK(place_penalty(free_depth) == 0);
    /* An unusable marker above the root (an explicit root inside a module
     * cache) does not demote the root's own entries. */
    CHECK(!child(top, "site-packages", false).dependency);
}
static void dependency_trees(void) {
    tl_place root = {.level = 1};
    tl_place packages = child(child(root, "lib", true), "site-packages", true);
    /* Entries inside the marker are in a dependency tree, the marker itself
     * (an entry of lib) is not; everything below inherits it. */
    CHECK(packages.dependency && !child(root, "lib", true).dependency);
    CHECK(place_penalty(packages) == PLACE_DEPENDENCY_PENALTY);
    tl_place nested = child(child(packages, "pip", true), "_internal", true);
    CHECK(nested.dependency);
    CHECK(place_penalty(nested) == PLACE_DEPENDENCY_PENALTY + PLACE_DEPTH_PENALTY);
}
static void cap(void) {
    CHECK(place_penalty((tl_place){.level = 1000, .dependency = true}) == PLACE_PENALTY_MAX);
    CHECK(place_penalty((tl_place){.level = UINT32_MAX}) == PLACE_PENALTY_MAX);
    /* The level saturates instead of wrapping. */
    CHECK(child((tl_place){.level = UINT32_MAX}, "x", true).level == UINT32_MAX);
    CHECK(place_penalty((tl_place){0}) == 0);
    /* Three levels beyond the free depth already outweigh the 64-point
     * short-name bonus, and the dependency penalty alone does too. */
    CHECK(place_penalty((tl_place){.level = 1 + PLACE_FREE_DEPTH + 3}) > 64);
    CHECK(PLACE_DEPENDENCY_PENALTY > 64 && PLACE_PENALTY_MAX < 500);
}
void test_place(void) {
    markers();
    levels();
    dependency_trees();
    cap();
}
