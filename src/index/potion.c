/* BERT-normalized WordPiece lookup and mean-pooling of a checked static table.
 * Model2Vec drops unknown tokens and does not add CLS/SEP during encoding. */
#include "torchlight/potion.h"
#include "torchlight/hashmap.h"
#include "torchlight/json.h"
#include <fcntl.h>
#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utf8proc.h>

enum {
    HEADER_BYTES = 220,
    MAX_VOCAB = 200000,
    WORD_CHARACTERS = 100,
    NORMALIZED_BYTES = EMBED_TEXT_BYTES * 4 + 1
};
struct potion {
    unsigned char *payload;
    const unsigned char *offsets, *weights;
    const char *vocabulary;
    tl_hashmap *tokens;
    size_t count, dimensions;
    char normalized[NORMALIZED_BYTES];
    uint32_t ids[EMBED_TEXT_BYTES];
};
struct token_key {
    const struct potion *model;
    const char *text;
    size_t length;
};
static uint32_t little_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
           (uint32_t)bytes[3] << 24;
}
static const char *token(const struct potion *model, uint32_t id) {
    return model->vocabulary + little_u32(model->offsets + (size_t)id * 4);
}
static bool token_equal(const void *context, uint32_t id) {
    const struct token_key *key = context;
    const char *word = token(key->model, id);
    return strlen(word) == key->length && memcmp(word, key->text, key->length) == 0;
}
static bool lookup(const struct potion *model, const char *text, size_t length, uint32_t *id) {
    struct token_key key = {model, text, length};
    return hashmap_find(model->tokens, hashmap_hash(HASHMAP_HASH_SEED, text, length), token_equal,
                        &key, id);
}
static void potion_destroy(void *context) {
    struct potion *model = context;
    if (model == NULL)
        return;
    hashmap_destroy(model->tokens);
    free(model->payload);
    free(model);
}
static bool chinese(utf8proc_int32_t value) {
    return (value >= 0x4e00 && value <= 0x9fff) || (value >= 0x3400 && value <= 0x4dbf) ||
           (value >= 0x20000 && value <= 0x2a6df) || (value >= 0x2a700 && value <= 0x2b73f) ||
           (value >= 0x2b740 && value <= 0x2b81f) || (value >= 0x2b820 && value <= 0x2ceaf) ||
           (value >= 0xf900 && value <= 0xfaff) || (value >= 0x2f800 && value <= 0x2fa1f);
}
static bool punctuation(utf8proc_int32_t value) {
    utf8proc_category_t category = utf8proc_category(value);
    return (value >= 33 && value <= 47) || (value >= 58 && value <= 64) ||
           (value >= 91 && value <= 96) || (value >= 123 && value <= 126) ||
           (category >= UTF8PROC_CATEGORY_PC && category <= UTF8PROC_CATEGORY_PO);
}
static bool whitespace(utf8proc_int32_t value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
           utf8proc_category(value) == UTF8PROC_CATEGORY_ZS;
}
static tl_status append_symbol(struct potion *model, size_t *length, utf8proc_int32_t value,
                               bool separate) {
    if (*length + 6 >= sizeof(model->normalized))
        return TL_LIMIT;
    if (separate)
        model->normalized[(*length)++] = ' ';
    utf8proc_ssize_t width =
        utf8proc_encode_char(value, (unsigned char *)model->normalized + *length);
    if (width <= 0)
        return TL_INVALID;
    *length += (size_t)width;
    if (separate)
        model->normalized[(*length)++] = ' ';
    model->normalized[*length] = 0;
    return TL_OK;
}
static tl_status normalize_piece(struct potion *model, const char *text, size_t bytes,
                                 size_t *length) {
    for (size_t offset = 0; offset < bytes;) {
        utf8proc_int32_t value = 0;
        utf8proc_ssize_t width = utf8proc_iterate((const unsigned char *)text + offset,
                                                  (utf8proc_ssize_t)(bytes - offset), &value);
        if (width <= 0)
            return TL_INVALID;
        offset += (size_t)width;
        utf8proc_category_t category = utf8proc_category(value);
        if (whitespace(value) || category == UTF8PROC_CATEGORY_ZL ||
            category == UTF8PROC_CATEGORY_ZP)
            value = ' ';
        else if (value == 0 || value == 0xfffd || category == UTF8PROC_CATEGORY_CC ||
                 category == UTF8PROC_CATEGORY_CF)
            continue;
        utf8proc_int32_t decomposed[16];
        utf8proc_ssize_t count = utf8proc_decompose_char(
            utf8proc_tolower(value), decomposed, 16,
            UTF8PROC_DECOMPOSE | UTF8PROC_STRIPMARK | UTF8PROC_STABLE, NULL);
        if (count < 0 || count > 16)
            return TL_LIMIT;
        for (utf8proc_ssize_t i = 0; i < count; i++) {
            tl_status status = append_symbol(model, length, decomposed[i],
                                             chinese(decomposed[i]) || punctuation(decomposed[i]));
            if (status != TL_OK)
                return status;
        }
    }
    return TL_OK;
}
static tl_status wordpiece(struct potion *model, const char *word, size_t bytes, size_t *count) {
    size_t boundaries[WORD_CHARACTERS + 1], characters = 0;
    for (size_t offset = 0; offset < bytes;) {
        if (characters == WORD_CHARACTERS)
            return TL_OK; /* A long word is one UNK, which Model2Vec discards. */
        boundaries[characters++] = offset;
        utf8proc_int32_t value = 0;
        utf8proc_ssize_t width = utf8proc_iterate((const unsigned char *)word + offset,
                                                  (utf8proc_ssize_t)(bytes - offset), &value);
        if (width <= 0)
            return TL_INVALID;
        offset += (size_t)width;
    }
    boundaries[characters] = bytes;
    size_t original = *count, start = 0;
    while (start < characters) {
        bool found = false;
        for (size_t end = characters; end > start; end--) {
            char candidate[WORD_CHARACTERS * 4 + 3];
            size_t prefix = start == 0 ? 0 : 2, length = boundaries[end] - boundaries[start];
            memcpy(candidate, "##", prefix);
            memcpy(candidate + prefix, word + boundaries[start], length);
            uint32_t id = 0;
            if (!lookup(model, candidate, prefix + length, &id))
                continue;
            if (*count == EMBED_TEXT_BYTES)
                return TL_LIMIT;
            if (id != 1)
                model->ids[(*count)++] = id;
            start = end;
            found = true;
            break;
        }
        if (!found) {
            *count = original; /* Unknown suffix invalidates the complete word. */
            break;
        }
    }
    return TL_OK;
}
static tl_status tokenize_normalized(struct potion *model, size_t *count) {
    const char *cursor = model->normalized;
    while (*cursor != 0) {
        while (*cursor == ' ')
            cursor++;
        const char *end = cursor;
        while (*end != 0 && *end != ' ')
            end++;
        tl_status status = wordpiece(model, cursor, (size_t)(end - cursor), count);
        if (status != TL_OK)
            return status;
        cursor = end;
    }
    return TL_OK;
}
static size_t special_token(const char *text, uint32_t *id) {
    static const char *const SPECIAL[] = {"[PAD]", "[UNK]", "[CLS]", "[SEP]", "[MASK]"};
    for (size_t i = 0; i < 5; i++) {
        size_t length = strlen(SPECIAL[i]);
        if (strncmp(text, SPECIAL[i], length) == 0) {
            *id = (uint32_t)i;
            return length;
        }
    }
    return 0;
}
static tl_status tokenize_text(struct potion *model, const char *text, size_t *count) {
    const char *start = text, *cursor = text;
    *count = 0;
    while (true) {
        uint32_t id = 0;
        size_t special = special_token(cursor, &id);
        if (*cursor == 0 || special != 0) {
            size_t length = 0;
            model->normalized[0] = 0;
            tl_status status = normalize_piece(model, start, (size_t)(cursor - start), &length);
            if (status == TL_OK)
                status = tokenize_normalized(model, count);
            if (status != TL_OK || *cursor == 0)
                return status;
            if (*count == EMBED_TEXT_BYTES)
                return TL_LIMIT;
            if (id != 1)
                model->ids[(*count)++] = id;
            cursor += special;
            start = cursor;
        } else {
            cursor++;
        }
    }
}
static tl_status potion_encode(void *context, tl_embed_input role, const char *text, float *out,
                               size_t dimensions) {
    struct potion *model = context;
    (void)role;
    size_t count = 0;
    tl_status status = tokenize_text(model, text, &count);
    if (status != TL_OK)
        return status;
    if (count == 0)
        return TL_STATE;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *weights = model->weights + (size_t)model->ids[i] * dimensions * 4;
        for (size_t j = 0; j < dimensions; j++) {
            uint32_t bits = little_u32(weights + j * 4);
            float value = 0;
            memcpy(&value, &bits, sizeof(value));
            out[j] += value;
        }
    }
    for (size_t j = 0; j < dimensions; j++)
        out[j] /= (float)count;
    return TL_OK;
}
static tl_status validate_vocabulary(struct potion *model, size_t vocabulary_bytes) {
    if (little_u32(model->offsets) != 0 ||
        little_u32(model->offsets + model->count * 4) != vocabulary_bytes)
        return TL_STATE;
    tl_status status = hashmap_create(&model->tokens);
    for (size_t i = 0; i < model->count && status == TL_OK; i++) {
        uint32_t begin = little_u32(model->offsets + i * 4);
        uint32_t end = little_u32(model->offsets + (i + 1) * 4);
        if (begin >= end || end > vocabulary_bytes || model->vocabulary[end - 1] != 0 ||
            memchr(model->vocabulary + begin, 0, end - begin - 1) != NULL ||
            !json_utf8(model->vocabulary + begin))
            return TL_STATE;
        uint32_t existing = 0;
        if (lookup(model, model->vocabulary + begin, end - begin - 1, &existing))
            return TL_STATE;
        status = hashmap_insert(
            model->tokens,
            hashmap_hash(HASHMAP_HASH_SEED, model->vocabulary + begin, end - begin - 1),
            (uint32_t)i);
    }
    if (status != TL_OK)
        return status;
    static const char *const SPECIAL[] = {"[PAD]", "[UNK]", "[CLS]", "[SEP]", "[MASK]"};
    for (uint32_t i = 0; i < 5; i++)
        if (strcmp(token(model, i), SPECIAL[i]) != 0)
            return TL_STATE;
    return status;
}
static tl_status load_payload(FILE *file, const unsigned char header[HEADER_BYTES], size_t budget,
                              struct potion *model) {
    model->count = little_u32(header + 8);
    model->dimensions = little_u32(header + 12);
    size_t vocabulary_bytes = little_u32(header + 16);
    if (memcmp(header, "TLSTAT01", 8) != 0 || model->count < 5 || model->count > MAX_VOCAB ||
        model->dimensions == 0 || model->dimensions > 512 || vocabulary_bytes > 8U * 1024U * 1024U)
        return TL_STATE;
    size_t offset_bytes = (model->count + 1) * 4;
    size_t weights_bytes = model->count * model->dimensions * 4;
    size_t total = offset_bytes + vocabulary_bytes + weights_bytes;
    if (total > budget || total > POTION_MODEL_BYTES)
        return TL_LIMIT;
    model->payload = malloc(total);
    if (model->payload == NULL)
        return TL_NOMEM;
    if (fread(model->payload, 1, total, file) != total || fgetc(file) != EOF || ferror(file))
        return TL_IO;
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    if (checksum == NULL)
        return TL_NOMEM;
    g_checksum_update(checksum, model->payload, (gssize)total);
    unsigned char digest[32];
    gsize digest_bytes = sizeof(digest);
    g_checksum_get_digest(checksum, digest, &digest_bytes);
    g_checksum_free(checksum);
    if (memcmp(digest, header + 188, sizeof(digest)) != 0)
        return TL_STATE;
    model->offsets = model->payload;
    model->vocabulary = (const char *)model->payload + offset_bytes;
    model->weights = model->payload + offset_bytes + vocabulary_bytes;
    for (size_t i = 0; i < weights_bytes; i += 4) {
        uint32_t bits = little_u32(model->weights + i);
        float value = 0;
        memcpy(&value, &bits, sizeof(value));
        if (!isfinite(value))
            return TL_STATE;
    }
    return validate_vocabulary(model, vocabulary_bytes);
}
static tl_status create_adapter(struct potion *model, const unsigned char header[HEADER_BYTES],
                                tl_embedder **out) {
    for (size_t i = 20; i < 188; i++)
        if (!((header[i] >= '0' && header[i] <= '9') || (header[i] >= 'a' && header[i] <= 'f')))
            return TL_STATE;
    char revision[41], tokenizer_hash[65], projection[32];
    memcpy(revision, header + 20, 40);
    revision[40] = 0;
    memcpy(tokenizer_hash, header + 124, 64);
    tokenizer_hash[64] = 0;
    int written = snprintf(projection, sizeof(projection), "truncate-%zu-1", model->dimensions);
    if (written < 0 || (size_t)written >= sizeof(projection))
        return TL_LIMIT;
    uint64_t emb_gen = hashmap_hash(HASHMAP_HASH_SEED, header, HEADER_BYTES);
    static const char VERSIONS[] = "potion-1/launcher-text-1/int8-l2-1";
    emb_gen = hashmap_hash(emb_gen, VERSIONS, sizeof(VERSIONS) - 1);
    if (emb_gen == 0)
        emb_gen = 1;
    tl_emb_model descriptor = {emb_gen,    model->dimensions, "minishlab/potion-retrieval-32M",
                               revision,   tokenizer_hash,    "launcher-text-1",
                               projection, "int8-l2-1"};
    const tl_embedder_backend backend = {potion_encode, potion_destroy};
    return embedder_create(&descriptor, &backend, model, out);
}
static tl_status open_model(const char *filename, FILE **out) {
    /* Opening a FIFO must not wait for a writer before its type is checked.
     * Inspect the opened descriptor so symlink/replacement races cannot bypass
     * the regular-file requirement. */
    int descriptor = open(filename, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (descriptor < 0)
        return TL_IO;
    struct stat info;
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(descriptor);
        return TL_IO;
    }
    *out = fdopen(descriptor, "rb");
    if (*out == NULL) {
        close(descriptor);
        return TL_IO;
    }
    return TL_OK;
}
tl_status potion_load(const char *filename, size_t budget_bytes, tl_embedder **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (filename == NULL || budget_bytes == 0)
        return TL_INVALID;
    FILE *file = NULL;
    tl_status status = open_model(filename, &file);
    if (status != TL_OK)
        return status;
    struct potion *model = calloc(1, sizeof(*model));
    unsigned char header[HEADER_BYTES];
    status = model == NULL ? TL_NOMEM : TL_OK;
    if (status == TL_OK)
        status = fread(header, 1, sizeof(header), file) == sizeof(header) ? TL_OK : TL_IO;
    if (status == TL_OK)
        status = load_payload(file, header, budget_bytes, model);
    if (fclose(file) != 0 && status == TL_OK)
        status = TL_IO;
    if (status == TL_OK)
        status = create_adapter(model, header, out);
    if (status != TL_OK)
        potion_destroy(model);
    return status;
}
tl_status potion_prepare_path(const char *path, char *out, size_t capacity) {
    if (path == NULL || out == NULL || capacity == 0)
        return TL_INVALID;
    out[0] = 0;
    size_t bytes = strlen(path), separators = 0, begin = 0;
    for (size_t i = bytes; i > 0; i--) {
        if (path[i - 1] == '/' && ++separators == 3) {
            begin = i;
            break;
        }
    }
    size_t length = 0;
    for (size_t i = begin; i < bytes;) {
        utf8proc_int32_t value = 0;
        utf8proc_ssize_t width = utf8proc_iterate((const unsigned char *)path + i,
                                                  (utf8proc_ssize_t)(bytes - i), &value);
        size_t size = width > 0 ? (size_t)width : 1;
        bool space = width <= 0 || value == '/' || value == '_' || value == '-' || value == '.';
        bool camel = i > begin && path[i] >= 'A' && path[i] <= 'Z' && path[i - 1] >= 'a' &&
                     path[i - 1] <= 'z';
        size_t needed = (space ? 1 : size) + (camel ? 1 : 0);
        if (needed >= capacity - length) {
            out[0] = 0;
            return TL_LIMIT;
        }
        if (camel)
            out[length++] = ' ';
        if (space)
            out[length++] = ' ';
        else {
            memcpy(out + length, path + i, size);
            length += size;
        }
        i += size;
    }
    out[length] = 0;
    return TL_OK;
}
