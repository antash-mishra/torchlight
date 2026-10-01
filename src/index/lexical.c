/* Prefix/subsequence orchestration with allocation-free bounded top-k selection.
 * Words match a basename, one parent directory name, or (when they contain '/')
 * the full path. */
#include "torchlight/lexical.h"
#include "torchlight/fuzzy.h"
#include "torchlight/prefix.h"
#include "torchlight/vec.h"
#include <stdlib.h>
#include <string.h>
enum {
    LEXICAL_BASENAME_BONUS = 2000,
    LEXICAL_EXACT_BASENAME = 10000000,
    LEXICAL_EXACT_PATH = 20000000,
    LEXICAL_ROOT_SCORE = 1
};
struct entry {
    uint64_t id;
    char *path;
    tl_tokenized *text;
    tl_text view;
    uint64_t basename_mask;
    bool is_root;
};
struct tl_lexical {
    tl_vec *entries;
    tl_prefix *prefix;
    bool finished, failed;
};
struct tl_lexical_workspace {
    const tl_lexical *engine;
    int *scores, *totals;
    uint32_t symbols[LEXICAL_QUERY_BYTES * 4];
    uint8_t boundaries[LEXICAL_QUERY_BYTES * 4];
    size_t offsets[LEXICAL_QUERY_BYTES * 4];
};
tl_status lexical_create(tl_lexical **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_lexical *engine = calloc(1, sizeof(*engine));
    if (engine == NULL)
        return TL_NOMEM;
    tl_status status = vec_create(sizeof(struct entry), &engine->entries);
    if (status == TL_OK)
        status = prefix_create(&engine->prefix);
    if (status != TL_OK) {
        lexical_destroy(engine);
        return status;
    }
    *out = engine;
    return TL_OK;
}
void lexical_destroy(tl_lexical *engine) {
    if (engine == NULL)
        return;
    const struct entry *entries = vec_const_data(engine->entries);
    prefix_destroy(engine->prefix);
    for (size_t i = 0; i < vec_count(engine->entries); i++) {
        tokenize_destroy(entries[i].text);
        free(entries[i].path);
    }
    vec_destroy(engine->entries);
    free(engine);
}
tl_status lexical_add(tl_lexical *engine, uint64_t id, const char *path, bool is_root) {
    if (engine == NULL || path == NULL || path[0] == 0 || id == 0)
        return TL_INVALID;
    if (engine->finished || engine->failed)
        return TL_STATE;
    /* M1 append contract uses monotonically assigned ids, avoiding O(n^2) checks. */
    const struct entry *entries = vec_const_data(engine->entries);
    size_t count = vec_count(engine->entries);
    if (count != 0 && entries[count - 1].id >= id)
        return TL_INVALID;
    struct entry entry = {.id = id, .is_root = is_root};
    entry.path = strdup(path);
    if (entry.path == NULL)
        return TL_NOMEM;
    tl_status status = tokenize_create(path, &entry.text);
    if (status != TL_OK)
        goto cleanup;
    entry.view = tokenize_view(entry.text);
    for (size_t i = entry.view.basename; i < entry.view.length; i++)
        entry.basename_mask |= tokenize_symbol_mask(entry.view.symbols[i]);
    status = vec_append(engine->entries, &entry);
    if (status != TL_OK)
        goto cleanup;
    status = prefix_add(engine->prefix, entry.view, count);
    if (status != TL_OK)
        engine->failed = true;
    return status;
cleanup:
    tokenize_destroy(entry.text);
    free(entry.path);
    return status;
}
tl_status lexical_finish(tl_lexical *engine) {
    if (engine == NULL)
        return TL_INVALID;
    if (engine->finished || engine->failed)
        return TL_STATE;
    tl_status status = prefix_finish(engine->prefix);
    if (status == TL_OK)
        engine->finished = true;
    return status;
}
size_t lexical_count(const tl_lexical *engine) {
    return engine == NULL ? 0 : vec_count(engine->entries);
}
tl_status lexical_workspace_create(const tl_lexical *engine, tl_lexical_workspace **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (engine == NULL)
        return TL_INVALID;
    if (!engine->finished)
        return TL_STATE;
    size_t count = lexical_count(engine), bytes = 0;
    tl_status status = tl_size_multiply(count == 0 ? 1 : count, sizeof(int), &bytes);
    if (status != TL_OK)
        return status;
    tl_lexical_workspace *workspace = calloc(1, sizeof(*workspace));
    if (workspace == NULL)
        return TL_NOMEM;
    workspace->scores = malloc(bytes);
    workspace->totals = malloc(bytes);
    if (workspace->scores == NULL || workspace->totals == NULL) {
        lexical_workspace_destroy(workspace);
        return TL_NOMEM;
    }
    workspace->engine = engine;
    *out = workspace;
    return TL_OK;
}
void lexical_workspace_destroy(tl_lexical_workspace *workspace) {
    if (workspace == NULL)
        return;
    free(workspace->scores);
    free(workspace->totals);
    free(workspace);
}
static tl_text slice(tl_text text, size_t start, size_t end) {
    tl_text view = {.symbols = text.symbols + start,
                    .boundaries = text.boundaries + start,
                    .length = end - start};
    for (size_t i = 0; i < view.length; i++)
        view.mask |= tokenize_symbol_mask(view.symbols[i]);
    return view;
}
static tl_text basename_view(const struct entry *entry) {
    tl_text text = entry->view;
    text.symbols += text.basename;
    text.boundaries += text.basename;
    text.byte_offsets += text.basename;
    text.length -= text.basename;
    text.basename = 0;
    text.mask = entry->basename_mask;
    return text;
}
/* Subsequence score within the nearest parent directory name that matches.
 * Matching across components would let letters scattered over a long absolute
 * path (e.g. "hp" in /home/user/project) match nearly every entry. Stopping at
 * the nearest match favours close context and avoids scanning every ancestor;
 * strong parent matches are already scored higher by parent token prefixes. */
static tl_status parent_score(const struct entry *entry, tl_text word, int *out) {
    *out = 0;
    tl_text path = entry->view;
    if ((path.mask & word.mask) != word.mask)
        return TL_OK;
    /* basename follows the last '/', so each component ends just before a '/'. */
    for (size_t end = path.basename; end > 0 && *out == 0;) {
        size_t component_end = end - 1, start = component_end;
        while (start > 0 && path.symbols[start - 1] != '/')
            start--;
        /* The whole-path mask is a superset of the component's, so it remains a
         * conservative filter and avoids rebuilding a mask per component. */
        tl_text component = {.symbols = path.symbols + start,
                             .boundaries = path.boundaries + start,
                             .length = component_end - start,
                             .mask = path.mask};
        if (component.length != 0) {
            tl_status status = fuzzy_score(component, word, out);
            if (status != TL_OK)
                return status;
        }
        end = start;
    }
    return TL_OK;
}
static bool contains_slash(tl_text word) {
    for (size_t i = 0; i < word.length; i++) {
        if (word.symbols[i] == '/')
            return true;
    }
    return false;
}
/* Score one word for an entry: basename subsequences first; otherwise a word
 * containing '/' asks for path structure and matches across the full path,
 * while any other word must match within one parent directory name. */
static tl_status entry_word_score(const struct entry *entry, tl_text word, bool path_word,
                                  int *out) {
    tl_status status = fuzzy_score(basename_view(entry), word, out);
    if (status != TL_OK || *out > 0) {
        if (*out > 0)
            *out += LEXICAL_BASENAME_BONUS;
        return status;
    }
    return path_word ? fuzzy_score(entry->view, word, out) : parent_score(entry, word, out);
}
static tl_status score_word(const tl_lexical *engine, tl_lexical_workspace *workspace,
                            tl_text word) {
    size_t count = lexical_count(engine);
    memset(workspace->scores, 0, count * sizeof(int));
    tl_status status = prefix_query(engine->prefix, word, workspace->scores, count);
    if (status != TL_OK)
        return status;
    const struct entry *entries = vec_const_data(engine->entries);
    bool path_word = contains_slash(word);
    for (size_t i = 0; i < count; i++) {
        /* Every word must match, so an entry rejected by an earlier word is done. */
        if (workspace->totals[i] < 0)
            continue;
        int score = 0;
        status = entry_word_score(&entries[i], word, path_word, &score);
        if (status != TL_OK)
            return status;
        if (score > workspace->scores[i])
            workspace->scores[i] = score;
        if (workspace->scores[i] == 0)
            workspace->totals[i] = -1;
        else
            workspace->totals[i] += workspace->scores[i];
    }
    return TL_OK;
}
static bool whitespace(uint32_t symbol) {
    return symbol == ' ' || symbol == '\t' || symbol == '\n' || symbol == '\r';
}
static tl_status score_words(const tl_lexical *engine, tl_lexical_workspace *workspace,
                             tl_text query, size_t *word_count) {
    *word_count = 0;
    memset(workspace->totals, 0, lexical_count(engine) * sizeof(int));
    for (size_t start = 0; start < query.length;) {
        if (whitespace(query.symbols[start])) {
            start++;
            continue;
        }
        size_t end = start + 1;
        while (end < query.length && !whitespace(query.symbols[end]))
            end++;
        tl_status status = score_word(engine, workspace, slice(query, start, end));
        if (status != TL_OK)
            return status;
        (*word_count)++;
        start = end;
    }
    return TL_OK;
}
static bool same_symbols(tl_text a, tl_text b) {
    return a.length == b.length && memcmp(a.symbols, b.symbols, a.length * sizeof(uint32_t)) == 0;
}
static bool better(tl_result a, tl_result b) {
    if (a.score != b.score)
        return a.score > b.score;
    int order = strcmp(a.path, b.path);
    return order == 0 ? a.id < b.id : order < 0;
}
static void insert_result(tl_result result, tl_result *results, size_t capacity, size_t *count) {
    if (*count == capacity && !better(result, results[*count - 1]))
        return;
    size_t position = *count < capacity ? (*count)++ : capacity - 1;
    while (position > 0 && better(result, results[position - 1])) {
        results[position] = results[position - 1];
        position--;
    }
    results[position] = result;
}
static void collect(const tl_lexical *engine, tl_lexical_workspace *workspace,
                    const char *raw_query, tl_text query, size_t words, tl_result *results,
                    size_t capacity, size_t *count) {
    const struct entry *entries = vec_const_data(engine->entries);
    size_t entry_count = lexical_count(engine);
    for (size_t i = 0; i < entry_count; i++) {
        int score = workspace->totals[i];
        if (words == 0) {
            if (!entries[i].is_root)
                continue;
            score = LEXICAL_ROOT_SCORE;
        }
        if (words != 0 && same_symbols(basename_view(&entries[i]), query))
            score = LEXICAL_EXACT_BASENAME;
        if (strcmp(entries[i].path, raw_query) == 0)
            score = LEXICAL_EXACT_PATH;
        if (score <= 0)
            continue;
        insert_result((tl_result){entries[i].id, entries[i].path, score}, results, capacity, count);
    }
}
tl_status lexical_query(const tl_lexical *engine, tl_lexical_workspace *workspace,
                        const char *query, tl_result *results, size_t capacity, size_t *out_count) {
    if (out_count == NULL)
        return TL_INVALID;
    *out_count = 0;
    if (engine == NULL || workspace == NULL || workspace->engine != engine || query == NULL ||
        results == NULL || capacity == 0)
        return TL_INVALID;
    if (!engine->finished)
        return TL_STATE;
    if (capacity > LEXICAL_MAX_RESULTS)
        return TL_LIMIT;
    size_t length = strnlen(query, LEXICAL_QUERY_BYTES + 1);
    if (length > LEXICAL_QUERY_BYTES)
        return TL_LIMIT;
    tl_text text = {0};
    tl_status status = tokenize_into(query, length, workspace->symbols, workspace->boundaries,
                                     workspace->offsets, LEXICAL_QUERY_BYTES * 4, &text);
    if (status != TL_OK)
        return status;
    size_t words = 0;
    status = score_words(engine, workspace, text, &words);
    if (status != TL_OK)
        return status;
    collect(engine, workspace, query, text, words, results, capacity, out_count);
    return TL_OK;
}
