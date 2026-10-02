/* Interned directory tree: every distinct directory of the indexed paths is
 * stored once, with a link to its parent and its normalized name, so parent
 * context can be matched per directory instead of per file. */
#ifndef TORCHLIGHT_DIRTREE_H
#define TORCHLIGHT_DIRTREE_H
#include "torchlight/tokenize.h"
/* The node for "/"; it has an empty name. */
#define DIRTREE_ROOT UINT32_C(0)
/* Parent of DIRTREE_ROOT. */
#define DIRTREE_NONE UINT32_MAX
typedef struct tl_dirtree tl_dirtree;
/** Create an owned tree containing only DIRTREE_ROOT; out NULL on failure. */
tl_status dirtree_create(tl_dirtree **out);
/** Free the tree and all names; NULL allowed. */
void dirtree_destroy(tl_dirtree *tree);
/** Intern every directory along the absolute directory path bytes[0..length)
 * (e.g. "/home/a"; "" and "/" mean the root) and write the deepest node.
 * Repeated slashes are ignored. A node's parent is always interned before it,
 * so node numbers are a topological order. Before finish only. TL_INVALID for
 * NULL/relative paths or NUL bytes, TL_STATE after finish, TL_LIMIT/TL_NOMEM;
 * on failure discard the tree. bytes are copied. */
tl_status dirtree_intern(tl_dirtree *tree, const char *bytes, size_t length, uint32_t *out);
/** Seal the tree: storage stops moving, so name views stay valid until
 * destroy. TL_INVALID for NULL, TL_STATE if already sealed. */
tl_status dirtree_finish(tl_dirtree *tree);
/** Number of nodes including the root; zero for NULL. */
size_t dirtree_count(const tl_dirtree *tree);
/** Parent node, or DIRTREE_NONE for the root/out-of-range nodes. */
uint32_t dirtree_parent(const tl_dirtree *tree, uint32_t node);
/** Borrow a node's normalized name (no slashes) with boundaries and a mask.
 * Valid until destroy once sealed; an empty view for the root or bad nodes. */
tl_text dirtree_name(const tl_dirtree *tree, uint32_t node);
/** Conservative mask of every symbol in the node's full path, including '/'. */
uint64_t dirtree_path_mask(const tl_dirtree *tree, uint32_t node);
/** Normalized symbol length of the node's full path with its slashes ("/" is 1). */
size_t dirtree_path_length(const tl_dirtree *tree, uint32_t node);
/** Largest dirtree_path_length over all nodes; zero for NULL. */
size_t dirtree_max_path_length(const tl_dirtree *tree);
#endif
