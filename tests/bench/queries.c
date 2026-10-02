/* Known-item query simulation: derive realistic partial, abbreviated and
 * misspelled queries from a target entry's ASCII basename. */
#include "queries.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum {
    QUERY_ATTEMPTS = 2000, /* tries to find a suitable target per query */
    QUERY_MAX_WORDS = 8,
    QUERY_MIN_TYPO_WORD = 5, /* edits in shorter words are mostly ambiguous */
    QUERY_PREFIX_PERCENT = 60,
    QUERY_PARTIAL_PERCENT = 70,
    QUERY_MAX_TYPO_WORD = 60 /* edit buffers hold this plus an insertion */
};
struct span {
    size_t start, length;
};
const char *query_kind_name(enum query_kind kind) {
    static const char *const names[QUERY_KIND_COUNT] = {"exact", "prefix",       "abbreviation",
                                                        "typo",  "partial_typo", "parent"};
    return kind < QUERY_KIND_COUNT ? names[kind] : "unknown";
}
static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}
/* Parent directory name of path as [start, start + length). */
static struct span parent_name(const char *path) {
    const char *slash = strrchr(path, '/');
    struct span span = {0, 0};
    if (slash == NULL || slash == path)
        return span;
    const char *start = slash;
    while (start > path && start[-1] != '/')
        start--;
    span.start = (size_t)(start - path);
    span.length = (size_t)(slash - start);
    return span;
}
static bool is_lower(char c) {
    return c >= 'a' && c <= 'z';
}
static bool is_upper(char c) {
    return c >= 'A' && c <= 'Z';
}
static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}
static bool is_alpha(char c) {
    return is_lower(c) || is_upper(c);
}
static char lower(char c) {
    return is_upper(c) ? (char)(c - 'A' + 'a') : c;
}
static bool printable_ascii(const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (text[i] < 0x20 || text[i] > 0x7e)
            return false;
    }
    return true;
}
/* Stem length: basename without its final extension. */
static size_t stem_length(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot == NULL || dot == name ? strlen(name) : (size_t)(dot - name);
}
/* Split a stem into alphanumeric words at separators, camelCase and
 * letter/digit changes, like the engine's tokenizer does for ASCII. */
static size_t split_words(const char *stem, size_t length, struct span *words) {
    size_t count = 0;
    for (size_t i = 0; i < length && count < QUERY_MAX_WORDS;) {
        if (!is_alpha(stem[i]) && !is_digit(stem[i])) {
            i++;
            continue;
        }
        size_t end = i + 1;
        while (end < length && (is_alpha(stem[end]) || is_digit(stem[end])) &&
               !(is_lower(stem[end - 1]) && is_upper(stem[end])) &&
               is_digit(stem[end]) == is_digit(stem[end - 1]))
            end++;
        words[count++] = (struct span){i, end - i};
        i = end;
    }
    return count;
}
static void copy_lower(char *out, const char *text, size_t length) {
    for (size_t i = 0; i < length; i++)
        out[i] = lower(text[i]);
    out[length] = 0;
}
static bool make_exact(const char *name, char *out) {
    size_t length = strlen(name);
    if (length < 3 || length > LEXICAL_QUERY_BYTES)
        return false;
    memcpy(out, name, length + 1);
    return true;
}
static bool make_prefix(const char *name, char *out) {
    size_t stem = stem_length(name);
    size_t length = (stem * QUERY_PREFIX_PERCENT + 99) / 100;
    if (stem < 4 || length > LEXICAL_QUERY_BYTES)
        return false;
    copy_lower(out, name, length < 3 ? 3 : length);
    return true;
}
static bool consonant(char c) {
    return is_alpha(c) && strchr("aeiouAEIOU", c) == NULL;
}
static bool make_abbreviation(const char *name, char *out) {
    struct span words[QUERY_MAX_WORDS];
    size_t count = split_words(name, stem_length(name), words), length = 0;
    size_t per_word = count == 1 ? 4 : 2; /* consonants kept after each first letter */
    for (size_t w = 0; w < count && w < 3; w++) {
        const char *word = name + words[w].start;
        out[length++] = lower(word[0]);
        for (size_t i = 1, kept = 0; i < words[w].length && kept < per_word; i++) {
            if (consonant(word[i]) || is_digit(word[i])) {
                out[length++] = lower(word[i]);
                kept++;
            }
        }
    }
    out[length] = 0;
    return length >= 3;
}
/* Apply one random edit at a position >= 1 of text (length >= 2) in place;
 * the first letter stays, as people rarely mistype it. */
static void apply_edit(char *text, size_t *length, uint64_t *state) {
    size_t position = 1 + (size_t)(corpus_random(state) % (*length - 1));
    char letter = (char)('a' + corpus_random(state) % 26);
    switch (corpus_random(state) % 4) {
    case 0: /* substitution */
        text[position] = letter == text[position] ? (char)('a' + (letter - 'a' + 1) % 26) : letter;
        break;
    case 1: /* adjacent swap */
        if (position + 1 < *length && text[position] != text[position + 1]) {
            char swap = text[position];
            text[position] = text[position + 1];
            text[position + 1] = swap;
            break;
        }
        text[position] = letter == text[position] ? (char)('a' + (letter - 'a' + 1) % 26) : letter;
        break;
    case 2: /* deletion */
        memmove(text + position, text + position + 1, *length - position);
        (*length)--;
        break;
    default: /* insertion */
        memmove(text + position + 1, text + position, *length - position + 1);
        text[position] = letter;
        (*length)++;
    }
}
static bool longest_alpha_word(const char *name, struct span *out) {
    struct span words[QUERY_MAX_WORDS];
    size_t count = split_words(name, stem_length(name), words);
    *out = (struct span){0, 0};
    for (size_t w = 0; w < count; w++) {
        bool alpha = true;
        for (size_t i = 0; i < words[w].length; i++)
            alpha = alpha && is_alpha(name[words[w].start + i]);
        if (alpha && words[w].length > out->length)
            *out = words[w];
    }
    return out->length >= QUERY_MIN_TYPO_WORD && out->length <= QUERY_MAX_TYPO_WORD;
}
static bool make_typo(const char *name, char *out, uint64_t *state) {
    struct span word;
    size_t stem = stem_length(name);
    if (!longest_alpha_word(name, &word) || stem + 1 > LEXICAL_QUERY_BYTES)
        return false;
    char edited[QUERY_MAX_TYPO_WORD + 2];
    size_t length = word.length;
    copy_lower(edited, name + word.start, length);
    apply_edit(edited, &length, state);
    char before[LEXICAL_QUERY_BYTES + 1], after[LEXICAL_QUERY_BYTES + 1];
    copy_lower(before, name, word.start);
    copy_lower(after, name + word.start + word.length, stem - word.start - word.length);
    int code = snprintf(out, LEXICAL_QUERY_BYTES + 1, "%s%s%s", before, edited, after);
    return code > 0 && code <= LEXICAL_QUERY_BYTES;
}
static bool make_partial_typo(const char *name, char *out, uint64_t *state) {
    struct span word;
    if (!longest_alpha_word(name, &word) || word.length < QUERY_MIN_TYPO_WORD + 1)
        return false;
    size_t length = (word.length * QUERY_PARTIAL_PERCENT + 99) / 100;
    copy_lower(out, name + word.start, length);
    apply_edit(out, &length, state);
    out[length] = 0;
    return length >= 4;
}
static bool make_parent(const char *path, const char *name, char *out) {
    struct span parent = parent_name(path);
    size_t stem = stem_length(name);
    if (parent.length < 3 || parent.length > 64 || stem < 4 ||
        !printable_ascii(path + parent.start, parent.length) ||
        memchr(path + parent.start, ' ', parent.length) != NULL)
        return false;
    char directory[65], prefix[5];
    copy_lower(directory, path + parent.start, parent.length);
    copy_lower(prefix, name, 4);
    int code = snprintf(out, LEXICAL_QUERY_BYTES + 1, "%s %s", directory, prefix);
    return code > 0 && code <= LEXICAL_QUERY_BYTES;
}
static bool make_query(const bench_corpus *corpus, size_t target, enum query_kind kind,
                       uint64_t *state, char *out) {
    const char *path = corpus->paths[target], *name = base_name(path);
    size_t length = strlen(name);
    if (corpus->is_root[target] || length == 0 || !printable_ascii(name, length))
        return false;
    switch (kind) {
    case QUERY_EXACT:
        return make_exact(name, out);
    case QUERY_PREFIX:
        return make_prefix(name, out);
    case QUERY_ABBREVIATION:
        return make_abbreviation(name, out);
    case QUERY_TYPO:
        return make_typo(name, out, state);
    case QUERY_PARTIAL_TYPO:
        return make_partial_typo(name, out, state);
    case QUERY_PARENT:
        return make_parent(path, name, out);
    default:
        return false;
    }
}
tl_status queries_generate(const bench_corpus *corpus, uint64_t seed, size_t per_kind,
                           bench_query **out, size_t *count) {
    if (corpus == NULL || out == NULL || count == NULL || corpus->count == 0)
        return TL_INVALID;
    *count = 0;
    *out = malloc((per_kind * QUERY_KIND_COUNT + 1) * sizeof(bench_query));
    if (*out == NULL)
        return TL_NOMEM;
    uint64_t state = seed;
    for (int kind = 0; kind < QUERY_KIND_COUNT; kind++) {
        for (size_t made = 0; made < per_kind; made++) {
            for (size_t attempt = 0; attempt < QUERY_ATTEMPTS; attempt++) {
                bench_query *query = &(*out)[*count];
                query->kind = (enum query_kind)kind;
                query->target = (size_t)(corpus_random(&state) % corpus->count);
                if (make_query(corpus, query->target, query->kind, &state, query->text)) {
                    (*count)++;
                    break;
                }
            }
        }
    }
    return TL_OK;
}
bool query_relevant(const bench_corpus *corpus, const bench_query *query, const char *path) {
    const char *target = corpus->paths[query->target];
    if (strcmp(base_name(target), base_name(path)) != 0)
        return false;
    if (query->kind != QUERY_PARENT)
        return true;
    struct span a = parent_name(target), b = parent_name(path);
    return a.length == b.length && memcmp(target + a.start, path + b.start, a.length) == 0;
}
