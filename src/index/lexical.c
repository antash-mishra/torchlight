/* Lexical engine construction: copies raw paths, interns parent directories,
 * normalizes basenames into shared arenas, then builds the prefix, trigram and
 * typo channels plus path ordering when sealed. Querying is in lexical_query.c. */
#include "lexical_internal.h"
#include <stdlib.h>
#include <string.h>
tl_status lexical_entry(const tl_lexical *engine, size_t position, uint64_t *id, const char **path,
                        bool *is_dir) {
    if (engine == NULL || id == NULL || path == NULL || is_dir == NULL || position >= engine->count)
        return TL_INVALID;
    if (!engine->finished)
        return TL_STATE;
    *id = engine->columns.ids[position];
    *path = engine->columns.paths + engine->columns.path_offsets[position];
    *is_dir = lexical_is_dir(engine, *id);
    return TL_OK;
}
/* Normalized symbols per raw byte never exceed this (see tokenize.h). */
enum { LEXICAL_SYMBOLS_PER_BYTE = 4 };
static tl_status create_columns(tl_lexical *engine) {
    struct {
        tl_vec **vec;
        size_t size;
    } columns[] = {{&engine->fields, sizeof(struct lexical_field)},
                   {&engine->directory_ids, sizeof(uint64_t)},
                   {&engine->ids, sizeof(uint64_t)},
                   {&engine->repeats, sizeof(uint64_t)},
                   {&engine->masks, sizeof(uint64_t)},
                   {&engine->path_offsets, sizeof(uint32_t)},
                   {&engine->name_offsets, sizeof(uint32_t)},
                   {&engine->name_lengths, sizeof(uint32_t)},
                   {&engine->dirs, sizeof(uint32_t)},
                   {&engine->roots, sizeof(uint32_t)},
                   {&engine->paths, 1},
                   {&engine->symbols, sizeof(uint32_t)},
                   {&engine->boundaries, 1},
                   {&engine->scratch_symbols, sizeof(uint32_t)},
                   {&engine->scratch_boundaries, 1},
                   {&engine->scratch_offsets, sizeof(size_t)}};
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++) {
        tl_status status = vec_create(columns[i].size, columns[i].vec);
        if (status != TL_OK)
            return status;
    }
    return TL_OK;
}
tl_status lexical_create(tl_lexical **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_lexical *engine = calloc(1, sizeof(*engine));
    if (engine == NULL)
        return TL_NOMEM;
    tl_status status = create_columns(engine);
    if (status == TL_OK)
        status = dirtree_create(&engine->tree);
    if (status == TL_OK)
        status = prefix_create(&engine->prefix);
    if (status == TL_OK)
        status = prefix_create(&engine->dir_prefix);
    if (status == TL_OK)
        status = prefix_create(&engine->field_prefix);
    if (status == TL_OK)
        status = trigram_create(&engine->trigram);
    if (status == TL_OK)
        status = typo_create(&engine->typo);
    if (status != TL_OK) {
        lexical_destroy(engine);
        return status;
    }
    *out = engine;
    return TL_OK;
}
tl_status lexical_set_prefix_bonus(tl_lexical *engine, int bonus) {
    if (engine == NULL || bonus < 0 || bonus > LEXICAL_PREFIX_BONUS_MAX)
        return TL_INVALID;
    if (engine->finished || engine->failed)
        return TL_STATE;
    engine->prefix_bonus = bonus;
    return TL_OK;
}
void lexical_destroy(tl_lexical *engine) {
    if (engine == NULL)
        return;
    struct lexical_field *fields = vec_data(engine->fields);
    for (size_t i = 0; i < vec_count(engine->fields); i++)
        tokenize_destroy(fields[i].text);
    vec_destroy(engine->fields);
    prefix_destroy(engine->field_prefix);
    tl_vec *columns[] = {engine->directory_ids,   engine->ids,
                         engine->masks,           engine->repeats,
                         engine->path_offsets,    engine->name_offsets,
                         engine->name_lengths,    engine->dirs,
                         engine->roots,           engine->paths,
                         engine->symbols,         engine->boundaries,
                         engine->scratch_symbols, engine->scratch_boundaries,
                         engine->scratch_offsets};
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++)
        vec_destroy(columns[i]);
    dirtree_destroy(engine->tree);
    prefix_destroy(engine->prefix);
    prefix_destroy(engine->dir_prefix);
    trigram_destroy(engine->trigram);
    typo_destroy(engine->typo);
    mask_index_destroy(engine->name_masks);
    free(engine->dir_starts);
    free(engine->dir_entries);
    free(engine->dir_masks);
    free(engine->dir_descendants);
    free(engine->symbol_results);
    free(engine->contexts);
    free(engine->usable);
    free(engine->path_order);
    free(engine->path_rank);
    free(engine);
}
/* Normalize name into the scratch vectors; text borrows them until next use. */
static tl_status normalize_name(tl_lexical *engine, const char *name, size_t length,
                                tl_text *text) {
    if (length > (SIZE_MAX - 1) / LEXICAL_SYMBOLS_PER_BYTE)
        return TL_LIMIT;
    size_t capacity = length * LEXICAL_SYMBOLS_PER_BYTE + 1;
    tl_status status = vec_reserve(engine->scratch_symbols, capacity);
    if (status == TL_OK)
        status = vec_reserve(engine->scratch_boundaries, capacity);
    if (status == TL_OK)
        status = vec_reserve(engine->scratch_offsets, capacity);
    if (status == TL_OK)
        status = tokenize_into(name, length, vec_data(engine->scratch_symbols),
                               vec_data(engine->scratch_boundaries),
                               vec_data(engine->scratch_offsets), capacity, text);
    return status;
}
static tl_status append_columns(tl_lexical *engine, uint64_t id, uint32_t dir, tl_text name,
                                const char *path, size_t length) {
    size_t path_offset = vec_count(engine->paths), name_offset = vec_count(engine->symbols);
    if (path_offset > UINT32_MAX || name_offset > UINT32_MAX || name.length > UINT32_MAX ||
        length >= UINT32_MAX)
        return TL_LIMIT;
    uint32_t path32 = (uint32_t)path_offset, name32 = (uint32_t)name_offset,
             length32 = (uint32_t)name.length;
    tl_status status = vec_append_array(engine->paths, path, length + 1);
    if (status == TL_OK)
        status = vec_append_array(engine->symbols, name.symbols, name.length);
    if (status == TL_OK)
        status = vec_append_array(engine->boundaries, name.boundaries, name.length);
    uint64_t repeats = lexical_repeat_mask(name.symbols, name.length);
    if (status == TL_OK)
        status = vec_append(engine->masks, &name.mask);
    if (status == TL_OK)
        status = vec_append(engine->repeats, &repeats);
    if (status == TL_OK)
        status = vec_append(engine->ids, &id);
    if (status == TL_OK)
        status = vec_append(engine->path_offsets, &path32);
    if (status == TL_OK)
        status = vec_append(engine->name_offsets, &name32);
    if (status == TL_OK)
        status = vec_append(engine->name_lengths, &length32);
    if (status == TL_OK)
        status = vec_append(engine->dirs, &dir);
    return status;
}
static tl_status append_entry(tl_lexical *engine, uint64_t id, const char *path, bool is_root) {
    size_t length = strlen(path), slot = engine->count;
    if (slot >= UINT32_MAX)
        return TL_LIMIT;
    const char *slash = strrchr(path, '/');
    size_t parent_length = (size_t)(slash - path);
    uint32_t dir = DIRTREE_ROOT;
    tl_status status = dirtree_intern(engine->tree, path, parent_length, &dir);
    tl_text name = {0};
    if (status == TL_OK)
        status = normalize_name(engine, slash + 1, length - parent_length - 1, &name);
    if (status == TL_OK)
        status = append_columns(engine, id, dir, name, path, length);
    uint32_t slot32 = (uint32_t)slot;
    if (status == TL_OK && is_root)
        status = vec_append(engine->roots, &slot32);
    if (status == TL_OK)
        engine->count++;
    return status;
}
tl_status lexical_add(tl_lexical *engine, uint64_t id, const char *path, bool is_root) {
    if (engine == NULL)
        return TL_INVALID;
    if (engine->finished || engine->failed)
        return TL_STATE;
    if (path == NULL || path[0] != '/' || id == 0)
        return TL_INVALID;
    /* M1 append contract uses monotonically assigned ids, avoiding O(n^2) checks. */
    const uint64_t *ids = vec_const_data(engine->ids);
    if (engine->count != 0 && ids[engine->count - 1] >= id)
        return TL_INVALID;
    tl_status status = append_entry(engine, id, path, is_root);
    if (status != TL_OK)
        engine->failed = true; /* columns may be partially appended */
    return status;
}
size_t lexical_count(const tl_lexical *engine) {
    return engine == NULL ? 0 : engine->count;
}
tl_status lexical_add_entry(tl_lexical *engine, uint64_t id, const char *path, bool is_root,
                            bool is_dir) {
    tl_status status = lexical_add(engine, id, path, is_root);
    if (status == TL_OK && is_dir) {
        status = vec_append(engine->directory_ids, &id);
        if (status != TL_OK)
            engine->failed = true;
    }
    return status;
}
static tl_status append_field(tl_lexical *engine, const char *text, int weight) {
    if (text == NULL || text[0] == 0)
        return TL_OK;
    if (vec_count(engine->fields) >= UINT32_MAX)
        return TL_LIMIT;
    struct lexical_field field = {.slot = (uint32_t)(engine->count - 1), .weight = weight};
    tl_status status = tokenize_create(text, &field.text);
    if (status == TL_OK)
        status = vec_append(engine->fields, &field);
    if (status != TL_OK)
        tokenize_destroy(field.text);
    return status;
}
tl_status lexical_add_fields(tl_lexical *engine, uint64_t id, const char *path,
                             const char *generic_name, const char *keywords) {
    tl_status status = lexical_add(engine, id, path, false);
    if (status != TL_OK)
        return status;
    status = append_field(engine, generic_name, LEXICAL_GENERIC_SCORE);
    if (status == TL_OK)
        status = append_field(engine, keywords, LEXICAL_KEYWORD_SCORE);
    if (status != TL_OK)
        engine->failed = true;
    return status;
}
bool lexical_is_dir(const tl_lexical *engine, uint64_t id) {
    if (engine == NULL || !engine->finished)
        return false;
    const uint64_t *ids = vec_const_data(engine->directory_ids);
    size_t low = 0, high = vec_count(engine->directory_ids);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (ids[middle] < id)
            low = middle + 1;
        else
            high = middle;
    }
    return low < vec_count(engine->directory_ids) && ids[low] == id;
}
tl_status lexical_resolve(const tl_lexical *engine, uint64_t id, const char **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (engine == NULL || id == 0)
        return TL_INVALID;
    if (!engine->finished || engine->failed)
        return TL_STATE;
    size_t low = 0, high = engine->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (engine->columns.ids[middle] < id)
            low = middle + 1;
        else
            high = middle;
    }
    if (low == engine->count || engine->columns.ids[low] != id)
        return TL_STATE;
    *out = engine->columns.paths + engine->columns.path_offsets[low];
    return TL_OK;
}
/* Intern each root's own directory, then mark directories strictly above all
 * roots unusable as parent context, unless they are inside another root. */
static tl_status build_usable(tl_lexical *engine) {
    const uint32_t *roots = vec_const_data(engine->roots);
    size_t root_count = vec_count(engine->roots);
    tl_vec *root_nodes = NULL;
    tl_status status = vec_create(sizeof(uint32_t), &root_nodes);
    for (size_t i = 0; i < root_count && status == TL_OK; i++) {
        const char *path = (const char *)vec_const_data(engine->paths) +
                           ((const uint32_t *)vec_const_data(engine->path_offsets))[roots[i]];
        uint32_t node = DIRTREE_ROOT;
        status = dirtree_intern(engine->tree, path, strlen(path), &node);
        if (status == TL_OK)
            status = vec_append(root_nodes, &node);
    }
    size_t nodes = dirtree_count(engine->tree);
    uint8_t *inside = calloc(nodes, 1);
    engine->usable = malloc(nodes);
    if (status == TL_OK && (inside == NULL || engine->usable == NULL))
        status = TL_NOMEM;
    if (status == TL_OK) {
        memset(engine->usable, 1, nodes);
        const uint32_t *marked = vec_const_data(root_nodes);
        for (size_t i = 0; i < vec_count(root_nodes); i++) {
            inside[marked[i]] = 1;
            for (uint32_t up = dirtree_parent(engine->tree, marked[i]); up != DIRTREE_NONE;
                 up = dirtree_parent(engine->tree, up))
                engine->usable[up] = 0;
        }
        /* Parents precede children, so one forward pass propagates "inside". */
        for (uint32_t node = 1; node < nodes; node++) {
            inside[node] |= inside[dirtree_parent(engine->tree, node)];
            if (inside[node])
                engine->usable[node] = 1;
        }
    }
    free(inside);
    vec_destroy(root_nodes);
    return status;
}
struct path_key {
    const char *path;
    uint64_t id;
    uint32_t slot;
};
static int compare_paths(const void *left, const void *right) {
    const struct path_key *a = left, *b = right;
    int order = strcmp(a->path, b->path);
    if (order != 0)
        return order;
    return a->id == b->id ? 0 : a->id < b->id ? -1 : 1;
}
/* Sort slots by raw path bytes (then id) once, so queries can find exact raw
 * paths by binary search and break score ties by rank instead of strcmp. */
static tl_status build_path_order(tl_lexical *engine) {
    size_t count = engine->count, slots = count == 0 ? 1 : count;
    struct path_key *keys = malloc(slots * sizeof(*keys));
    engine->path_order = malloc(slots * sizeof(uint32_t));
    engine->path_rank = malloc(slots * sizeof(uint32_t));
    if (keys == NULL || engine->path_order == NULL || engine->path_rank == NULL) {
        free(keys);
        return TL_NOMEM;
    }
    const struct lexical_columns *columns = &engine->columns;
    for (size_t i = 0; i < count; i++)
        keys[i] = (struct path_key){columns->paths + columns->path_offsets[i], columns->ids[i],
                                    (uint32_t)i};
    if (count > 1)
        qsort(keys, count, sizeof(*keys), compare_paths);
    for (size_t rank = 0; rank < count; rank++) {
        engine->path_order[rank] = keys[rank].slot;
        engine->path_rank[keys[rank].slot] = (uint32_t)rank;
    }
    free(keys);
    return TL_OK;
}
static tl_status build_entry_channels(tl_lexical *engine) {
    tl_status status = TL_OK;
    for (size_t slot = 0; slot < engine->count && status == TL_OK; slot++) {
        tl_text name = lexical_name(engine, slot);
        if (name.length == 0)
            continue;
        status = prefix_add(engine->prefix, name, slot);
        if (status == TL_OK)
            status = trigram_add(engine->trigram, name, slot);
        if (status == TL_OK)
            status = typo_add(engine->typo, name, slot);
    }
    if (status == TL_OK)
        status = prefix_finish(engine->prefix);
    if (status == TL_OK)
        status = trigram_finish(engine->trigram);
    if (status == TL_OK)
        status = typo_finish(engine->typo);
    return status;
}
static tl_status build_field_channel(tl_lexical *engine) {
    const struct lexical_field *fields = vec_const_data(engine->fields);
    tl_status status = TL_OK;
    for (size_t i = 0; i < vec_count(engine->fields) && status == TL_OK; i++) {
        tl_text text = tokenize_view(fields[i].text);
        text.basename = 0;
        if (text.length != 0)
            status = prefix_add(engine->field_prefix, text, i);
    }
    return status == TL_OK ? prefix_finish(engine->field_prefix) : status;
}
static tl_status build_directory_channel(tl_lexical *engine) {
    size_t nodes = dirtree_count(engine->tree);
    tl_status status = TL_OK;
    for (uint32_t node = 1; node < nodes && status == TL_OK; node++) {
        tl_text name = dirtree_name(engine->tree, node);
        if (name.length != 0 && engine->usable[node])
            status = prefix_add(engine->dir_prefix, name, node);
    }
    return status == TL_OK ? prefix_finish(engine->dir_prefix) : status;
}
static void seal_columns(tl_lexical *engine) {
    tl_vec *columns[] = {engine->directory_ids, engine->ids,          engine->masks,
                         engine->repeats,       engine->path_offsets, engine->name_offsets,
                         engine->name_lengths,  engine->dirs,         engine->roots,
                         engine->paths,         engine->symbols,      engine->boundaries};
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++)
        vec_shrink(columns[i]);
    engine->columns = (struct lexical_columns){.ids = vec_const_data(engine->ids),
                                               .masks = vec_const_data(engine->masks),
                                               .repeats = vec_const_data(engine->repeats),
                                               .path_offsets = vec_const_data(engine->path_offsets),
                                               .name_offsets = vec_const_data(engine->name_offsets),
                                               .name_lengths = vec_const_data(engine->name_lengths),
                                               .dirs = vec_const_data(engine->dirs),
                                               .roots = vec_const_data(engine->roots),
                                               .paths = vec_const_data(engine->paths),
                                               .symbols = vec_const_data(engine->symbols),
                                               .boundaries = vec_const_data(engine->boundaries)};
    engine->root_count = vec_count(engine->roots);
    size_t longest = 0;
    for (size_t slot = 0; slot < engine->count; slot++) {
        size_t length = dirtree_path_length(engine->tree, engine->columns.dirs[slot]) + 1 +
                        engine->columns.name_lengths[slot];
        if (length > longest)
            longest = length;
    }
    engine->max_path_symbols = longest;
    vec_destroy(engine->scratch_symbols);
    vec_destroy(engine->scratch_boundaries);
    vec_destroy(engine->scratch_offsets);
    engine->scratch_symbols = engine->scratch_boundaries = engine->scratch_offsets = NULL;
}
/* Run every cached one-symbol query once through the normal query path. */
static tl_status build_symbol_results(tl_lexical *engine) {
    size_t bytes = 0;
    tl_status status =
        tl_size_multiply(LEXICAL_SYMBOL_QUERIES * LEXICAL_MAX_RESULTS, sizeof(tl_result), &bytes);
    if (status != TL_OK)
        return status;
    engine->symbol_results = malloc(bytes);
    if (engine->symbol_results == NULL)
        return TL_NOMEM;
    tl_lexical_workspace *workspace = NULL;
    status = lexical_workspace_create(engine, &workspace);
    for (size_t i = 0; i < LEXICAL_SYMBOL_QUERIES && status == TL_OK; i++) {
        char query[2] = {(char)lexical_symbol_query(i), 0};
        status = lexical_query(engine, workspace, query,
                               engine->symbol_results + i * LEXICAL_MAX_RESULTS,
                               LEXICAL_MAX_RESULTS, &engine->symbol_counts[i]);
    }
    lexical_workspace_destroy(workspace);
    engine->symbols_ready = status == TL_OK;
    return status;
}
/* Per entry, every symbol of its basename or any ancestor directory name. An
 * entry whose context lacks a symbol of a word can match that word only
 * through a channel hit, so scans reject it with one load. */
static tl_status build_contexts(tl_lexical *engine) {
    engine->contexts = malloc((engine->count == 0 ? 1 : engine->count) * sizeof(uint64_t));
    if (engine->contexts == NULL)
        return TL_NOMEM;
    for (size_t slot = 0; slot < engine->count; slot++)
        engine->contexts[slot] = engine->columns.masks[slot] |
                                 dirtree_path_mask(engine->tree, engine->columns.dirs[slot]);
    engine->columns.contexts = engine->contexts;
    return TL_OK;
}
/* Counting-sort entries by parent node. Each list is immutable and keeps slot
 * order; nodes without entries have an empty range. */
static tl_status build_directory_entries(tl_lexical *engine) {
    size_t nodes = dirtree_count(engine->tree);
    size_t start_bytes = 0, entry_bytes = 0, cursor_bytes = 0;
    if (nodes == SIZE_MAX)
        return TL_LIMIT;
    tl_status status = tl_size_multiply(nodes + 1, sizeof(uint32_t), &start_bytes);
    if (status == TL_OK)
        status = tl_size_multiply(engine->count == 0 ? 1 : engine->count, sizeof(uint32_t),
                                  &entry_bytes);
    if (status == TL_OK)
        status = tl_size_multiply(nodes, sizeof(uint32_t), &cursor_bytes);
    if (status != TL_OK)
        return status;
    engine->dir_starts = calloc(1, start_bytes);
    engine->dir_entries = malloc(entry_bytes);
    uint32_t *cursor = malloc(cursor_bytes);
    if (engine->dir_starts == NULL || engine->dir_entries == NULL || cursor == NULL) {
        free(cursor);
        return TL_NOMEM;
    }
    for (size_t slot = 0; slot < engine->count; slot++)
        engine->dir_starts[engine->columns.dirs[slot] + 1]++;
    for (size_t node = 0; node < nodes; node++) {
        engine->dir_starts[node + 1] += engine->dir_starts[node];
        cursor[node] = engine->dir_starts[node];
    }
    for (size_t slot = 0; slot < engine->count; slot++)
        engine->dir_entries[cursor[engine->columns.dirs[slot]]++] = (uint32_t)slot;
    free(cursor);
    return TL_OK;
}
/* Cheap selectivity estimates: usable directory-name masks and the number of
 * entries below each node. Parents precede children, so a reverse pass counts
 * descendants. Estimates choose a scan order; they never discard candidates. */
static tl_status build_directory_estimates(tl_lexical *engine) {
    size_t nodes = dirtree_count(engine->tree);
    size_t mask_bytes = 0, descendant_bytes = 0;
    tl_status status = tl_size_multiply(nodes, sizeof(uint64_t), &mask_bytes);
    if (status == TL_OK)
        status = tl_size_multiply(nodes, sizeof(uint32_t), &descendant_bytes);
    if (status != TL_OK)
        return status;
    engine->dir_masks = calloc(1, mask_bytes);
    engine->dir_descendants = malloc(descendant_bytes);
    if (engine->dir_masks == NULL || engine->dir_descendants == NULL)
        return TL_NOMEM;
    for (uint32_t node = 0; node < nodes; node++) {
        if (engine->usable[node])
            engine->dir_masks[node] = dirtree_name(engine->tree, node).mask;
        engine->dir_descendants[node] = engine->dir_starts[node + 1] - engine->dir_starts[node];
    }
    for (size_t node = nodes; node > 1; node--) {
        uint32_t parent = dirtree_parent(engine->tree, (uint32_t)(node - 1));
        engine->dir_descendants[parent] += engine->dir_descendants[node - 1];
    }
    return TL_OK;
}
tl_status lexical_finish(tl_lexical *engine) {
    if (engine == NULL)
        return TL_INVALID;
    if (engine->finished || engine->failed)
        return TL_STATE;
    tl_status status = build_usable(engine);
    if (status == TL_OK)
        status = dirtree_finish(engine->tree);
    if (status == TL_OK) {
        seal_columns(engine);
        status = build_path_order(engine);
    }
    if (status == TL_OK) {
        status = build_contexts(engine);
    }
    if (status == TL_OK)
        status = mask_index_create(engine->columns.masks, engine->count, &engine->name_masks);
    if (status == TL_OK)
        status = build_directory_entries(engine);
    if (status == TL_OK)
        status = build_directory_estimates(engine);
    if (status == TL_OK)
        status = build_entry_channels(engine);
    if (status == TL_OK)
        status = build_directory_channel(engine);
    if (status == TL_OK)
        status = build_field_channel(engine);
    if (status != TL_OK) {
        engine->failed = true;
        return status;
    }
    engine->columns.path_order = engine->path_order;
    engine->columns.path_rank = engine->path_rank;
    engine->finished = true;
    status = build_symbol_results(engine);
    if (status != TL_OK) {
        engine->finished = false;
        engine->failed = true;
    }
    return status;
}

const char *lexical_context_path(const tl_lexical *engine, size_t position) {
    if (engine == NULL || !engine->finished || position >= engine->count)
        return NULL;
    const char *path = engine->columns.paths + engine->columns.path_offsets[position];
    size_t nearest = 0, begin = 0;
    for (size_t i = 0; i < engine->root_count; i++) {
        uint32_t slot = engine->columns.roots[i];
        const char *root = engine->columns.paths + engine->columns.path_offsets[slot];
        size_t length = strlen(root);
        if (length < nearest || strncmp(path, root, length) != 0 ||
            (length != 1 && path[length] != 0 && path[length] != '/'))
            continue;
        nearest = length;
        const char *separator = strrchr(root, '/');
        begin = separator == NULL ? 0 : (size_t)(separator + 1 - root);
    }
    return path + begin;
}
