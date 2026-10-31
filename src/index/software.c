/* Immutable source variants, deterministic ordering and resident available-app search. */
#include "torchlight/software.h"
#include "torchlight/hashmap.h"
#include "torchlight/json.h"
#include "torchlight/vec.h"
#include <stdlib.h>
#include <string.h>
struct tl_software {
    tl_vec *entries;
    tl_lexical *engine;
    size_t text_bytes;
    bool sealed;
};
struct tl_software_reader {
    const tl_software *software;
    tl_lexical_workspace *workspace;
    tl_result results[LEXICAL_MAX_RESULTS];
};
static bool identifier(const char *text, size_t capacity, bool dot_required) {
    if (memchr(text, 0, capacity) == NULL || text[0] == 0 || text[0] == '-')
        return false;
    unsigned dots = 0;
    for (size_t i = 0; text[i] != 0; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '.')
            dots++;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return false;
    }
    return (!dot_required || dots >= 2) && strcmp(text, ".") != 0 && strcmp(text, "..") != 0;
}
bool software_ref_valid(const tl_software_ref *ref) {
    if (ref == NULL || memchr(ref->installation, 0, sizeof(ref->installation)) == NULL)
        return false;
    bool scope = strcmp(ref->installation, "user") == 0 || strcmp(ref->installation, "system") == 0;
    if (!scope && strncmp(ref->installation, "system:", 7) == 0)
        scope = identifier(ref->installation + 7, sizeof(ref->installation) - 7, false);
    return scope && identifier(ref->remote, sizeof(ref->remote), false) &&
           identifier(ref->app_id, sizeof(ref->app_id), true) &&
           identifier(ref->arch, sizeof(ref->arch), false) &&
           identifier(ref->branch, sizeof(ref->branch), false);
}
static int compare_ref(const tl_software_ref *a, const tl_software_ref *b) {
    const char *left[] = {a->installation, a->remote, a->app_id, a->arch, a->branch};
    const char *right[] = {b->installation, b->remote, b->app_id, b->arch, b->branch};
    for (size_t i = 0; i < sizeof(left) / sizeof(left[0]); i++) {
        int order = strcmp(left[i], right[i]);
        if (order != 0)
            return order;
    }
    return 0;
}
uint64_t software_ref_hash(const tl_software_ref *ref) {
    if (ref == NULL)
        return 0;
    uint64_t hash = HASHMAP_HASH_SEED;
    const char *fields[] = {ref->installation, ref->remote, ref->app_id, ref->arch, ref->branch};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        hash = hashmap_hash(hash, fields[i], strlen(fields[i]) + 1);
    return hash;
}
tl_status software_create(tl_software **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = calloc(1, sizeof(**out));
    if (*out == NULL)
        return TL_NOMEM;
    tl_status status = vec_create(sizeof(tl_software_entry), &(*out)->entries);
    if (status != TL_OK) {
        software_destroy(*out);
        *out = NULL;
    }
    return status;
}
tl_status software_add(tl_software *software, const tl_software_entry *entry) {
    if (software == NULL || entry == NULL || !software_ref_valid(&entry->ref) ||
        entry->name == NULL || entry->name[0] == 0 || !json_utf8(entry->name) ||
        entry->summary == NULL || !json_utf8(entry->summary))
        return TL_INVALID;
    if (software->sealed)
        return TL_STATE;
    size_t name_bytes = strlen(entry->name), summary_bytes = strlen(entry->summary);
    if (name_bytes >= SOFTWARE_NAME_BYTES || summary_bytes >= SOFTWARE_SUMMARY_BYTES)
        return TL_LIMIT;
    size_t bytes = name_bytes + summary_bytes + 2;
    if (vec_count(software->entries) >= SOFTWARE_MAX_ENTRIES ||
        bytes > SOFTWARE_TEXT_BYTES - software->text_bytes)
        return TL_LIMIT;
    tl_software_entry copy = *entry;
    copy.name = strdup(entry->name);
    copy.summary = strdup(entry->summary);
    uint64_t hash = software_ref_hash(&copy.ref);
    hash = hashmap_hash(hash, &copy.source_revision, sizeof(copy.source_revision));
    hash = hashmap_hash(hash, copy.name == NULL ? "" : copy.name,
                        copy.name == NULL ? 0 : strlen(copy.name));
    hash = hashmap_hash(hash, entry->summary, strlen(entry->summary));
    copy.revision = hashmap_hash(hash, &copy.installed, sizeof(copy.installed));
    tl_status status =
        copy.name == NULL || copy.summary == NULL ? TL_NOMEM : vec_append(software->entries, &copy);
    if (status != TL_OK) {
        free((char *)copy.name);
        free((char *)copy.summary);
    } else
        software->text_bytes += bytes;
    return status;
}
static int compare_entry(const void *a, const void *b) {
    return compare_ref(&((const tl_software_entry *)a)->ref, &((const tl_software_entry *)b)->ref);
}
tl_status software_finish(tl_software *software) {
    if (software == NULL)
        return TL_INVALID;
    if (software->sealed)
        return TL_STATE;
    size_t count = vec_count(software->entries);
    tl_software_entry *entries = vec_data(software->entries);
    if (count > 1)
        qsort(entries, count, sizeof(*entries), compare_entry);
    tl_status status = lexical_create(&software->engine);
    for (size_t i = 0; i < count && status == TL_OK; i++) {
        if (i != 0 && software_ref_equal(&entries[i - 1].ref, &entries[i].ref))
            return TL_INVALID;
        char path[SOFTWARE_NAME_BYTES + 2] = "/";
        size_t length = strlen(entries[i].name);
        memcpy(path + 1, entries[i].name, length + 1);
        /* Names are search data: a slash is punctuation, never a folder. */
        for (size_t j = 1; j <= length; j++)
            if (path[j] == '/')
                path[j] = ' ';
        status = lexical_add_fields(software->engine, i + 1, path, entries[i].ref.app_id,
                                    entries[i].summary);
    }
    if (status == TL_OK)
        status = lexical_finish(software->engine);
    software->sealed = status == TL_OK;
    return status;
}
void software_destroy(tl_software *software) {
    if (software == NULL)
        return;
    tl_software_entry *entries = vec_data(software->entries);
    for (size_t i = 0; i < vec_count(software->entries); i++) {
        free((char *)entries[i].name);
        free((char *)entries[i].summary);
    }
    lexical_destroy(software->engine);
    vec_destroy(software->entries);
    free(software);
}
tl_status software_reader_create(const tl_software *software, tl_software_reader **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (software == NULL || !software->sealed)
        return TL_STATE;
    tl_software_reader *reader = calloc(1, sizeof(*reader));
    if (reader == NULL)
        return TL_NOMEM;
    reader->software = software;
    tl_status status = lexical_workspace_create(software->engine, &reader->workspace);
    if (status != TL_OK)
        software_reader_destroy(reader);
    else
        *out = reader;
    return status;
}
void software_reader_destroy(tl_software_reader *reader) {
    if (reader != NULL) {
        lexical_workspace_destroy(reader->workspace);
        free(reader);
    }
}
tl_status software_query(tl_software_reader *reader, const char *query, const atomic_bool *cancel,
                         const tl_software_entry **out, size_t capacity, size_t *count) {
    if (count == NULL)
        return TL_INVALID;
    *count = 0;
    if (reader == NULL || query == NULL || out == NULL || capacity == 0 ||
        capacity > LEXICAL_MAX_RESULTS)
        return TL_INVALID;
    lexical_workspace_cancel(reader->workspace, cancel);
    tl_status status = lexical_query(reader->software->engine, reader->workspace, query,
                                     reader->results, capacity, count);
    lexical_workspace_cancel(reader->workspace, NULL);
    const tl_software_entry *entries = vec_const_data(reader->software->entries);
    if (status == TL_OK)
        for (size_t i = 0; i < *count; i++)
            out[i] = &entries[reader->results[i].id - 1];
    return status;
}
const tl_software_entry *software_resolve(const tl_software *software, const tl_software_ref *ref,
                                          uint64_t revision) {
    if (software == NULL || !software->sealed || !software_ref_valid(ref))
        return NULL;
    tl_software_entry key = {.ref = *ref};
    size_t count = vec_count(software->entries);
    const tl_software_entry *entry = count == 0 ? NULL
                                                : bsearch(&key, vec_const_data(software->entries),
                                                          count, sizeof(key), compare_entry);
    return entry != NULL && entry->revision == revision ? entry : NULL;
}
size_t software_count(const tl_software *software) {
    return software == NULL ? 0 : vec_count(software->entries);
}
