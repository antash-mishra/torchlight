/* Directory interning, parent links, normalized names and path metadata. */
#include "test.h"
#include "torchlight/dirtree.h"
#include <string.h>
static uint32_t intern(tl_dirtree *tree, const char *path) {
    uint32_t node = DIRTREE_NONE;
    CHECK(dirtree_intern(tree, path, strlen(path), &node) == TL_OK);
    return node;
}
void test_dirtree(void) {
    tl_dirtree *tree = NULL;
    CHECK(dirtree_create(&tree) == TL_OK && dirtree_count(tree) == 1);
    CHECK(intern(tree, "") == DIRTREE_ROOT && intern(tree, "/") == DIRTREE_ROOT);
    uint32_t notes = intern(tree, "/home/User/projectNotes");
    uint32_t home = dirtree_parent(tree, dirtree_parent(tree, notes));
    CHECK(dirtree_count(tree) == 4 && dirtree_parent(tree, home) == DIRTREE_ROOT);
    /* Repeated paths, repeated slashes and shared prefixes reuse nodes. */
    CHECK(intern(tree, "/home/User/projectNotes") == notes);
    CHECK(intern(tree, "//home///User/projectNotes") == notes);
    uint32_t other = intern(tree, "/home/User/other");
    CHECK(dirtree_parent(tree, other) == dirtree_parent(tree, notes) && dirtree_count(tree) == 5);
    /* Parents are interned before children: node numbers are topological. */
    CHECK(dirtree_parent(tree, notes) < notes && home < dirtree_parent(tree, notes));
    uint32_t node = 0;
    CHECK(dirtree_intern(tree, "relative/path", 13, &node) == TL_INVALID);
    CHECK(dirtree_intern(tree, "/a\0b", 4, &node) == TL_INVALID);
    CHECK(dirtree_finish(tree) == TL_OK && dirtree_finish(tree) == TL_STATE);
    CHECK(dirtree_intern(tree, "/x", 2, &node) == TL_STATE);
    tl_text name = dirtree_name(tree, notes);
    CHECK(name.length == 12 && name.symbols[0] == 'p' && name.symbols[7] == 'n');
    CHECK(name.boundaries[0] && name.boundaries[7]);
    CHECK(dirtree_name(tree, DIRTREE_ROOT).length == 0 && dirtree_name(tree, 99).length == 0);
    /* "/home/User/projectNotes" has 23 symbols including its slashes. */
    CHECK(dirtree_path_length(tree, notes) == 23 && dirtree_max_path_length(tree) == 23);
    uint64_t mask = dirtree_path_mask(tree, notes);
    CHECK((mask & tokenize_symbol_mask('/')) && (mask & tokenize_symbol_mask('u')));
    CHECK(dirtree_parent(tree, DIRTREE_ROOT) == DIRTREE_NONE);
    dirtree_destroy(tree);
    dirtree_destroy(NULL);
}
