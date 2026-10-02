/* Benchmark corpus construction. The synthetic generator imitates a home
 * folder: nested project/document directories, camelCase and separated names,
 * numbered photos, duplicate README/index files and mixed extensions. */
#include "corpus.h"
#include "torchlight/vec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum {
    CORPUS_PATH_BYTES = 512,
    CORPUS_FILES_PER_DIR = 12,
    CORPUS_MAX_DEPTH = 7,
    CORPUS_HOT_PERCENT = 30, /* share of files placed in a few large directories */
    CORPUS_HOT_DIVISOR = 20,
    CORPUS_NAME_PATTERNS = 12
};
static const char *const WORDS[] = {
    "project",   "notes",    "report",   "invoice",  "budget",    "meeting",   "draft",
    "final",     "resume",   "letter",   "photo",    "holiday",   "family",    "music",
    "album",     "video",    "backup",   "config",   "data",      "test",      "build",
    "source",    "docs",     "assets",   "images",   "scripts",   "utils",     "server",
    "client",    "model",    "thesis",   "paper",    "slides",    "recipe",    "travel",
    "receipt",   "contract", "tax",      "bank",     "statement", "school",    "course",
    "lecture",   "homework", "game",     "save",     "design",    "logo",      "website",
    "blog",      "post",     "chapter",  "book",     "novel",     "journal",   "diary",
    "todo",      "plan",     "schedule", "calendar", "payroll",   "salary",    "insurance",
    "medical",   "health",   "garden",   "house",    "manual",    "guide",     "license",
    "changelog", "index",    "main",     "app",      "module",    "component", "widget",
    "service",   "handler",  "parser",   "lexer",    "engine",    "search",    "query",
    "result",    "cache",    "store",    "table",    "schema",    "migration", "fixture",
    "sample",    "demo",     "example",  "template", "archive",   "export",    "import",
    "sync",      "upload",   "temp",     "old",      "copy",      "version",   "release",
    "summary",   "analysis", "chart",    "graph",    "map",       "list",      "sheet",
    "form",      "survey",   "wedding",  "birthday", "concert",   "lyrics",    "podcast",
    "episode"};
static const char *const EXTENSIONS[] = {"pdf",  "txt", "md",  "jpg", "png", "mp3",   "docx",
                                         "xlsx", "c",   "h",   "py",  "js",  "json",  "html",
                                         "css",  "zip", "csv", "log", "odt", "tar.gz"};
static const char *const COMMON_NAMES[] = {"README.md", "index.js", "main.c",   "__init__.py",
                                           "Makefile",  "LICENSE",  "notes.txt"};
static const char *const TOP_DIRS[] = {"Documents", "Downloads", "Pictures", "Music", "Videos",
                                       "Desktop",   "projects",  "src",      "work",  "books"};
/* Fixed fixtures used by the benchmark's sanity checks. */
static const char *const FIXTURES[] = {
    "/home/user/Documents/finance",         "/home/user/projects/torchlight",
    "/home/user/Documents/projectNotes.md", "/home/user/projects/torchlight/README.md",
    "/home/user/Documents/Caf\xc3\xa9.pdf", "/home/user/Documents/finance/invoice2024.pdf"};
#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))
uint64_t corpus_random(uint64_t *state) {
    uint64_t value = (*state += UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}
static size_t pick(uint64_t *state, size_t count) {
    return (size_t)(corpus_random(state) % count);
}
static const char *word(uint64_t *state) {
    return WORDS[pick(state, COUNT_OF(WORDS))];
}
struct builder {
    tl_vec *bytes, *offsets, *roots;
};
static tl_status emit(struct builder *builder, const char *path, bool is_root) {
    size_t offset = vec_count(builder->bytes);
    tl_status status = vec_append_array(builder->bytes, path, strlen(path) + 1);
    if (status == TL_OK)
        status = vec_append(builder->offsets, &offset);
    if (status == TL_OK)
        status = vec_append(builder->roots, &is_root);
    return status;
}
/* Write a capitalised copy of text (ASCII) into out. */
static void capitalised(const char *text, char *out, size_t size) {
    int code = snprintf(out, size, "%s", text);
    if (code > 0 && out[0] >= 'a' && out[0] <= 'z')
        out[0] = (char)(out[0] - 'a' + 'A');
}
static void directory_name(uint64_t *state, char *out, size_t size) {
    char upper[32];
    capitalised(word(state), upper, sizeof(upper));
    switch (pick(state, 6)) {
    case 0:
        snprintf(out, size, "%s", word(state));
        break;
    case 1:
        snprintf(out, size, "%s", upper);
        break;
    case 2:
        snprintf(out, size, "%s_%s", word(state), word(state));
        break;
    case 3:
        snprintf(out, size, "%u", 2010 + (unsigned)pick(state, 16));
        break;
    case 4:
        snprintf(out, size, "%s-%s", word(state), word(state));
        break;
    default:
        snprintf(out, size, "%s%u", word(state), 1 + (unsigned)pick(state, 9));
    }
}
static void file_name(uint64_t *state, char *out, size_t size) {
    const char *ext = EXTENSIONS[pick(state, COUNT_OF(EXTENSIONS))];
    const char *a = word(state), *b = word(state);
    char upper_a[32], upper_b[32];
    capitalised(a, upper_a, sizeof(upper_a));
    capitalised(b, upper_b, sizeof(upper_b));
    unsigned n = (unsigned)pick(state, 10000);
    switch (pick(state, CORPUS_NAME_PATTERNS)) {
    case 0:
        snprintf(out, size, "%s_%s.%s", a, b, ext);
        break;
    case 1:
        snprintf(out, size, "%s%s.%s", a, upper_b, ext);
        break;
    case 2:
        snprintf(out, size, "%s-%s-%u.%s", a, b, n % 100, ext);
        break;
    case 3:
        snprintf(out, size, "IMG_%04u.jpg", n);
        break;
    case 4:
        snprintf(out, size, "%s %s %u.%s", upper_a, upper_b, n % 50, ext);
        break;
    case 5:
        snprintf(out, size, "%s", COMMON_NAMES[pick(state, COUNT_OF(COMMON_NAMES))]);
        break;
    case 6:
        snprintf(out, size, "%s.%s", a, ext);
        break;
    case 7:
        snprintf(out, size, "%s%u.%s", a, n % 1000, ext);
        break;
    case 8:
        snprintf(out, size, "Screenshot from 20%02u-%02u-%02u %02u-%02u.png", 18 + n % 7,
                 1 + n % 12, 1 + n % 28, n % 24, n % 60);
        break;
    case 9:
        snprintf(out, size, "%s_%u_%s.%s", upper_a, 2010 + n % 16, b, ext);
        break;
    case 10:
        snprintf(out, size, "%s_v%u.%s", a, 1 + n % 9, ext);
        break;
    default:
        snprintf(out, size, "DSC%05u.JPG", n);
    }
}
/* Directories: top-level folders, fixture folders, then random nesting. */
static tl_status emit_directories(struct builder *builder, uint64_t *state, size_t target,
                                  tl_vec *dirs, tl_vec *depths) {
    char path[CORPUS_PATH_BYTES], name[128];
    tl_status status = TL_OK;
    for (size_t i = 0; i < COUNT_OF(TOP_DIRS) + 2 && status == TL_OK; i++) {
        if (i < COUNT_OF(TOP_DIRS))
            snprintf(path, sizeof(path), "/home/user/%s", TOP_DIRS[i]);
        else
            snprintf(path, sizeof(path), "%s", FIXTURES[i - COUNT_OF(TOP_DIRS)]);
        size_t offset = vec_count(builder->bytes), depth = 1;
        status = emit(builder, path, false);
        if (status == TL_OK)
            status = vec_append(dirs, &offset);
        if (status == TL_OK)
            status = vec_append(depths, &depth);
    }
    while (vec_count(dirs) < target && status == TL_OK) {
        size_t parent = pick(state, vec_count(dirs));
        size_t depth = ((const size_t *)vec_const_data(depths))[parent] + 1;
        if (depth > CORPUS_MAX_DEPTH)
            continue;
        const char *above = (const char *)vec_const_data(builder->bytes) +
                            ((const size_t *)vec_const_data(dirs))[parent];
        directory_name(state, name, sizeof(name));
        snprintf(path, sizeof(path), "%s/%s", above, name);
        size_t offset = vec_count(builder->bytes);
        status = emit(builder, path, false);
        if (status == TL_OK)
            status = vec_append(dirs, &offset);
        if (status == TL_OK)
            status = vec_append(depths, &depth);
    }
    return status;
}
static tl_status emit_files(struct builder *builder, uint64_t *state, size_t count,
                            const tl_vec *dirs) {
    char path[CORPUS_PATH_BYTES + 128], name[128];
    size_t dir_count = vec_count(dirs), hot = dir_count / CORPUS_HOT_DIVISOR + 1;
    tl_status status = TL_OK;
    for (size_t i = 2; i < COUNT_OF(FIXTURES) && status == TL_OK; i++)
        status = emit(builder, FIXTURES[i], false);
    while (vec_count(builder->offsets) < count && status == TL_OK) {
        bool in_hot = pick(state, 100) < CORPUS_HOT_PERCENT;
        size_t dir = pick(state, in_hot ? hot : dir_count);
        const char *above = (const char *)vec_const_data(builder->bytes) +
                            ((const size_t *)vec_const_data(dirs))[dir];
        file_name(state, name, sizeof(name));
        snprintf(path, sizeof(path), "%s/%s", above, name);
        status = emit(builder, path, false);
    }
    return status;
}
/* Turn the builder's offsets into a corpus that owns the byte buffer. */
static tl_status finish(struct builder *builder, bench_corpus *out) {
    size_t count = vec_count(builder->offsets);
    *out = (bench_corpus){0};
    out->paths = malloc((count == 0 ? 1 : count) * sizeof(char *));
    out->is_root = malloc((count == 0 ? 1 : count) * sizeof(bool));
    out->buffer = malloc(vec_count(builder->bytes) == 0 ? 1 : vec_count(builder->bytes));
    if (out->paths == NULL || out->is_root == NULL || out->buffer == NULL) {
        corpus_free(out);
        return TL_NOMEM;
    }
    memcpy(out->buffer, vec_const_data(builder->bytes), vec_count(builder->bytes));
    const size_t *offsets = vec_const_data(builder->offsets);
    memcpy(out->is_root, vec_const_data(builder->roots), count * sizeof(bool));
    for (size_t i = 0; i < count; i++) {
        out->paths[i] = out->buffer + offsets[i];
        out->bytes += strlen(out->paths[i]);
    }
    out->count = count;
    return TL_OK;
}
static tl_status create_builder(struct builder *builder) {
    tl_status status = vec_create(1, &builder->bytes);
    if (status == TL_OK)
        status = vec_create(sizeof(size_t), &builder->offsets);
    if (status == TL_OK)
        status = vec_create(sizeof(bool), &builder->roots);
    return status;
}
static void destroy_builder(struct builder *builder) {
    vec_destroy(builder->bytes);
    vec_destroy(builder->offsets);
    vec_destroy(builder->roots);
}
tl_status corpus_synthetic(size_t count, uint64_t seed, bench_corpus *out) {
    if (out == NULL || count < 16)
        return TL_INVALID;
    struct builder builder = {0};
    tl_vec *dirs = NULL, *depths = NULL;
    uint64_t state = seed;
    tl_status status = create_builder(&builder);
    if (status == TL_OK)
        status = vec_create(sizeof(size_t), &dirs);
    if (status == TL_OK)
        status = vec_create(sizeof(size_t), &depths);
    if (status == TL_OK)
        status = emit(&builder, "/home/user", true);
    if (status == TL_OK)
        status = emit_directories(&builder, &state, count / CORPUS_FILES_PER_DIR + 1, dirs, depths);
    if (status == TL_OK)
        status = emit_files(&builder, &state, count, dirs);
    if (status == TL_OK)
        status = finish(&builder, out);
    vec_destroy(dirs);
    vec_destroy(depths);
    destroy_builder(&builder);
    return status;
}
static tl_status read_file(const char *file, char **out, size_t *length) {
    FILE *stream = fopen(file, "rb");
    if (stream == NULL)
        return TL_IO;
    tl_vec *bytes = NULL;
    tl_status status = vec_create(1, &bytes);
    char chunk[65536];
    size_t got = 0;
    while (status == TL_OK && (got = fread(chunk, 1, sizeof(chunk), stream)) > 0)
        status = vec_append_array(bytes, chunk, got);
    if (status == TL_OK && ferror(stream))
        status = TL_IO;
    char terminator = 0;
    if (status == TL_OK)
        status = vec_append(bytes, &terminator);
    fclose(stream);
    *length = vec_count(bytes);
    *out = status == TL_OK ? malloc(*length) : NULL;
    if (status == TL_OK && *out == NULL)
        status = TL_NOMEM;
    if (status == TL_OK)
        memcpy(*out, vec_const_data(bytes), *length);
    vec_destroy(bytes);
    return status;
}
tl_status corpus_load(const char *file, size_t limit, bench_corpus *out) {
    if (file == NULL || out == NULL)
        return TL_INVALID;
    *out = (bench_corpus){0};
    char *data = NULL;
    size_t length = 0;
    tl_status status = read_file(file, &data, &length);
    struct builder builder = {0};
    if (status == TL_OK)
        status = create_builder(&builder);
    size_t shortest = SIZE_MAX, root = 0;
    for (size_t start = 0; status == TL_OK && start < length;) {
        size_t size = strlen(data + start);
        if (size != 0 && data[start] == '/' && (limit == 0 || vec_count(builder.offsets) < limit)) {
            if (size < shortest) {
                shortest = size;
                root = vec_count(builder.offsets);
            }
            status = emit(&builder, data + start, false);
        }
        start += size + 1;
    }
    if (status == TL_OK && vec_count(builder.offsets) == 0)
        status = TL_IO;
    if (status == TL_OK)
        ((bool *)vec_data(builder.roots))[root] = true;
    if (status == TL_OK)
        status = finish(&builder, out);
    destroy_builder(&builder);
    free(data);
    return status;
}
void corpus_free(bench_corpus *corpus) {
    if (corpus == NULL)
        return;
    free(corpus->paths);
    free(corpus->is_root);
    free(corpus->buffer);
    *corpus = (bench_corpus){0};
}
