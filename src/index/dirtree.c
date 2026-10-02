/* Directory interning keyed by (parent node, raw name bytes). Names are
 * normalized with tokenize_into into shared symbol/boundary arenas. */
#include "torchlight/dirtree.h"
#include "torchlight/hashmap.h"
#include "torchlight/vec.h"
#include <stdlib.h>
#include <string.h>
/* Normalized symbols per raw byte never exceed this (see tokenize.h). */
enum { DIRTREE_SYMBOLS_PER_BYTE = 4 };
struct node {
    uint32_t parent, raw_offset, raw_length, name_offset, name_length, path_length;
    uint64_t name_mask, path_mask;
};
struct tl_dirtree {
    tl_vec *nodes, *raw, *symbols, *boundaries, *scratch_symbols, *scratch_boundaries,
        *scratch_offsets, *last_path;
    tl_hashmap *index;
    uint32_t last_node;
    size_t max_path_length;
    bool finished;
};
struct node_key {
    const tl_dirtree *tree;
    uint32_t parent;
    const char *name;
    size_t length;
};
static bool same_node(const void *context, uint32_t value) {
    const struct node_key *key = context;
    const struct node *node = (const struct node *)vec_const_data(key->tree->nodes) + value;
    const char *raw = vec_const_data(key->tree->raw);
    return node->parent == key->parent && node->raw_length == key->length &&
           memcmp(raw + node->raw_offset, key->name, key->length) == 0;
}
static uint64_t node_hash(uint32_t parent, const char *name, size_t length) {
    return hashmap_hash(hashmap_hash(HASHMAP_HASH_SEED, &parent, sizeof(parent)), name, length);
}
static tl_status create_vectors(tl_dirtree *tree) {
    tl_status status = vec_create(sizeof(struct node), &tree->nodes);
    if (status == TL_OK)
        status = vec_create(1, &tree->raw);
    if (status == TL_OK)
        status = vec_create(sizeof(uint32_t), &tree->symbols);
    if (status == TL_OK)
        status = vec_create(sizeof(uint8_t), &tree->boundaries);
    if (status == TL_OK)
        status = vec_create(sizeof(uint32_t), &tree->scratch_symbols);
    if (status == TL_OK)
        status = vec_create(sizeof(uint8_t), &tree->scratch_boundaries);
    if (status == TL_OK)
        status = vec_create(sizeof(size_t), &tree->scratch_offsets);
    if (status == TL_OK)
        status = vec_create(1, &tree->last_path);
    return status;
}
tl_status dirtree_create(tl_dirtree **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_dirtree *tree = calloc(1, sizeof(*tree));
    if (tree == NULL)
        return TL_NOMEM;
    tl_status status = create_vectors(tree);
    if (status == TL_OK)
        status = hashmap_create(&tree->index);
    struct node root = {
        .parent = DIRTREE_NONE, .path_length = 1, .path_mask = tokenize_symbol_mask('/')};
    if (status == TL_OK)
        status = vec_append(tree->nodes, &root);
    if (status != TL_OK) {
        dirtree_destroy(tree);
        return status;
    }
    tree->max_path_length = 1;
    *out = tree;
    return TL_OK;
}
void dirtree_destroy(tl_dirtree *tree) {
    if (tree == NULL)
        return;
    vec_destroy(tree->nodes);
    vec_destroy(tree->raw);
    vec_destroy(tree->symbols);
    vec_destroy(tree->boundaries);
    vec_destroy(tree->scratch_symbols);
    vec_destroy(tree->scratch_boundaries);
    vec_destroy(tree->scratch_offsets);
    vec_destroy(tree->last_path);
    hashmap_destroy(tree->index);
    free(tree);
}
/* Normalize name into the scratch vectors, then append it to the arenas. */
static tl_status store_name(tl_dirtree *tree, const char *name, size_t length, struct node *node) {
    size_t capacity = length * DIRTREE_SYMBOLS_PER_BYTE + 1;
    tl_status status = vec_reserve(tree->scratch_symbols, capacity);
    if (status == TL_OK)
        status = vec_reserve(tree->scratch_boundaries, capacity);
    if (status == TL_OK)
        status = vec_reserve(tree->scratch_offsets, capacity);
    tl_text text = {0};
    if (status == TL_OK)
        status = tokenize_into(name, length, vec_data(tree->scratch_symbols),
                               vec_data(tree->scratch_boundaries), vec_data(tree->scratch_offsets),
                               capacity, &text);
    size_t offset = vec_count(tree->symbols);
    if (status == TL_OK && (offset > UINT32_MAX || text.length > UINT32_MAX))
        status = TL_LIMIT;
    if (status == TL_OK)
        status = vec_append_array(tree->symbols, text.symbols, text.length);
    if (status == TL_OK)
        status = vec_append_array(tree->boundaries, text.boundaries, text.length);
    node->name_offset = (uint32_t)offset;
    node->name_length = (uint32_t)text.length;
    node->name_mask = text.mask;
    return status;
}
static tl_status add_node(tl_dirtree *tree, uint32_t parent, const char *name, size_t length,
                          uint64_t hash, uint32_t *out) {
    size_t id = vec_count(tree->nodes), raw_offset = vec_count(tree->raw);
    if (id >= UINT32_MAX || raw_offset > UINT32_MAX || length > UINT32_MAX)
        return TL_LIMIT;
    struct node node = {
        .parent = parent, .raw_offset = (uint32_t)raw_offset, .raw_length = (uint32_t)length};
    tl_status status = vec_append_array(tree->raw, name, length);
    if (status == TL_OK)
        status = store_name(tree, name, length, &node);
    if (status != TL_OK)
        return status;
    const struct node *above = (const struct node *)vec_const_data(tree->nodes) + parent;
    /* Root path is "/" (length 1); every other node adds its name and a '/'. */
    size_t path_length =
        (size_t)above->path_length + node.name_length + (parent == DIRTREE_ROOT ? 0 : 1);
    if (path_length > UINT32_MAX)
        return TL_LIMIT;
    node.path_length = (uint32_t)path_length;
    node.path_mask = above->path_mask | node.name_mask;
    if (path_length > tree->max_path_length)
        tree->max_path_length = path_length;
    status = vec_append(tree->nodes, &node);
    if (status == TL_OK)
        status = hashmap_insert(tree->index, hash, (uint32_t)id);
    *out = (uint32_t)id;
    return status;
}
static tl_status intern_child(tl_dirtree *tree, uint32_t parent, const char *name, size_t length,
                              uint32_t *out) {
    struct node_key key = {tree, parent, name, length};
    uint64_t hash = node_hash(parent, name, length);
    if (hashmap_find(tree->index, hash, same_node, &key, out))
        return TL_OK;
    return add_node(tree, parent, name, length, hash, out);
}
/* Consecutive paths usually share a directory (crawl order), so remember the
 * last one and skip the per-component walk when it repeats. */
static bool repeat_of_last(const tl_dirtree *tree, const char *bytes, size_t length) {
    return vec_count(tree->last_path) == length && length != 0 &&
           memcmp(vec_const_data(tree->last_path), bytes, length) == 0;
}
tl_status dirtree_intern(tl_dirtree *tree, const char *bytes, size_t length, uint32_t *out) {
    if (tree == NULL || out == NULL || (bytes == NULL && length != 0) ||
        (length != 0 && bytes[0] != '/'))
        return TL_INVALID;
    if (tree->finished)
        return TL_STATE;
    if (repeat_of_last(tree, bytes, length)) {
        *out = tree->last_node;
        return TL_OK;
    }
    uint32_t node = DIRTREE_ROOT;
    for (size_t start = 0; start < length;) {
        if (bytes[start] == '/') {
            start++;
            continue;
        }
        const char *slash = memchr(bytes + start, '/', length - start);
        size_t end = slash == NULL ? length : (size_t)(slash - bytes);
        tl_status status = intern_child(tree, node, bytes + start, end - start, &node);
        if (status != TL_OK)
            return status;
        start = end;
    }
    vec_clear(tree->last_path);
    tl_status status = vec_append_array(tree->last_path, bytes, length);
    tree->last_node = node;
    *out = node;
    return status;
}
tl_status dirtree_finish(tl_dirtree *tree) {
    if (tree == NULL)
        return TL_INVALID;
    if (tree->finished)
        return TL_STATE;
    vec_shrink(tree->nodes);
    vec_shrink(tree->symbols);
    vec_shrink(tree->boundaries);
    hashmap_destroy(tree->index);
    tree->index = NULL;
    tree->finished = true;
    return TL_OK;
}
size_t dirtree_count(const tl_dirtree *tree) {
    return tree == NULL ? 0 : vec_count(tree->nodes);
}
static const struct node *find(const tl_dirtree *tree, uint32_t node) {
    if (tree == NULL || node >= vec_count(tree->nodes))
        return NULL;
    return (const struct node *)vec_const_data(tree->nodes) + node;
}
uint32_t dirtree_parent(const tl_dirtree *tree, uint32_t node) {
    const struct node *found = find(tree, node);
    return found == NULL ? DIRTREE_NONE : found->parent;
}
tl_text dirtree_name(const tl_dirtree *tree, uint32_t node) {
    const struct node *found = find(tree, node);
    tl_text text = {0};
    if (found == NULL || found->name_length == 0)
        return text;
    text.symbols = (const uint32_t *)vec_const_data(tree->symbols) + found->name_offset;
    text.boundaries = (const uint8_t *)vec_const_data(tree->boundaries) + found->name_offset;
    text.length = found->name_length;
    text.mask = found->name_mask;
    return text;
}
uint64_t dirtree_path_mask(const tl_dirtree *tree, uint32_t node) {
    const struct node *found = find(tree, node);
    return found == NULL ? 0 : found->path_mask;
}
size_t dirtree_path_length(const tl_dirtree *tree, uint32_t node) {
    const struct node *found = find(tree, node);
    return found == NULL ? 0 : found->path_length;
}
size_t dirtree_max_path_length(const tl_dirtree *tree) {
    return tree == NULL ? 0 : tree->max_path_length;
}
