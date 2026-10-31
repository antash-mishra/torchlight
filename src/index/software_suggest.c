/* Conservative name matches for ordinary searches, using a bounded lexical shortlist. */
#include "torchlight/prefix.h"
#include "torchlight/software.h"
#include <string.h>
#define SUGGESTION_CANDIDATES 64
#define SUGGESTION_SYMBOLS (3 * LEXICAL_QUERY_BYTES)
struct normalized {
    uint32_t symbols[SUGGESTION_SYMBOLS];
    uint8_t boundaries[SUGGESTION_SYMBOLS];
    size_t offsets[SUGGESTION_SYMBOLS];
    tl_text text;
};
static tl_status normalize(const char *text, struct normalized *out) {
    tl_status status = tokenize_into(text, strlen(text), out->symbols, out->boundaries,
                                     out->offsets, SUGGESTION_SYMBOLS, &out->text);
    out->text.basename = 0;
    return status;
}
static bool names_app(tl_text name, tl_text query) {
    size_t start = 0, words = 0;
    while (start < query.length) {
        if (tokenize_separator(query.symbols[start])) {
            start++;
            continue;
        }
        size_t end = start + 1;
        while (end < query.length && !tokenize_separator(query.symbols[end]))
            end++;
        tl_text word = {.symbols = query.symbols + start,
                        .boundaries = query.boundaries + start,
                        .length = end - start};
        int score = 0;
        bool complete = false;
        if (prefix_score(name, word, &score, &complete) != TL_OK || score < PREFIX_INITIALS_SCORE)
            return false;
        words++;
        start = end;
    }
    return words != 0;
}
static bool already_installed(const tl_software_entry *entry,
                              const tl_software_entry *const *candidates, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (candidates[i]->installed && strcmp(entry->ref.app_id, candidates[i]->ref.app_id) == 0)
            return true;
    return false;
}
static void append_distinct(const tl_software_entry *entry, const tl_software_entry **out,
                            size_t capacity, size_t *count) {
    for (size_t i = 0; i < *count; i++)
        if (strcmp(entry->ref.app_id, out[i]->ref.app_id) == 0)
            return;
    if (*count < capacity)
        out[(*count)++] = entry;
}
tl_status software_suggest(tl_software_reader *reader, const char *query, const atomic_bool *cancel,
                           const tl_software_entry **out, size_t capacity, size_t *count,
                           bool *first_exact) {
    if (count == NULL || first_exact == NULL)
        return TL_INVALID;
    *count = 0;
    *first_exact = false;
    if (reader == NULL || query == NULL || out == NULL || capacity == 0 ||
        capacity > SOFTWARE_SUGGESTIONS)
        return TL_INVALID;
    struct normalized input = {0};
    tl_status status = normalize(query, &input);
    if (status != TL_OK || input.text.length < 2)
        return status;
    const tl_software_entry *candidates[SUGGESTION_CANDIDATES];
    size_t found = 0;
    status = software_query(reader, query, cancel, candidates, SUGGESTION_CANDIDATES, &found);
    if (status != TL_OK)
        return status;
    bool matched[SUGGESTION_CANDIDATES] = {0}, exact[SUGGESTION_CANDIDATES] = {0};
    for (size_t i = 0; i < found; i++) {
        struct normalized name = {0};
        if (already_installed(candidates[i], candidates, found))
            continue;
        status = normalize(candidates[i]->name, &name);
        if (status != TL_OK)
            return status;
        matched[i] = names_app(name.text, input.text);
        exact[i] = name.text.length == input.text.length &&
                   memcmp(name.symbols, input.symbols, input.text.length * sizeof(uint32_t)) == 0;
    }
    for (unsigned pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < found; i++)
            if (matched[i] && exact[i] == (pass == 0)) {
                if (*count == 0)
                    *first_exact = exact[i];
                append_distinct(candidates[i], out, capacity, count);
            }
    return TL_OK;
}
