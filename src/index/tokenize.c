/* Unicode normalization by grapheme cluster with opaque malformed-byte symbols. */
#include "torchlight/tokenize.h"
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>
struct tl_tokenized {
    uint32_t *symbols;
    uint8_t *boundaries;
    size_t *offsets;
    tl_text view;
};
static size_t decode(const char *bytes, size_t length, uint32_t *symbol) {
    utf8proc_int32_t value = 0;
    utf8proc_ssize_t width =
        utf8proc_iterate((const utf8proc_uint8_t *)bytes, (utf8proc_ssize_t)length, &value);
    if (width > 0) {
        *symbol = (uint32_t)value;
        return (size_t)width;
    }
    *symbol = TOKENIZE_OPAQUE_BASE + (unsigned char)bytes[0];
    return 1;
}
bool tokenize_separator(uint32_t symbol) {
    return symbol == '/' || symbol == '_' || symbol == '-' || symbol == '.' || symbol == ' ' ||
           symbol == '\t' || symbol == '\n' || symbol == '\r';
}
uint64_t tokenize_symbol_mask(uint32_t symbol) {
    /* Hash collisions only admit additional candidates, never lose a match. */
    return UINT64_C(1) << ((symbol * UINT32_C(2654435761)) >> 26);
}
static size_t cluster_end(const char *bytes, size_t length, size_t start, uint32_t first) {
    uint32_t previous = first;
    size_t end = start + decode(bytes + start, length - start, &previous);
    if (first >= TOKENIZE_OPAQUE_BASE)
        return end;
    utf8proc_int32_t state = 0;
    while (end < length) {
        uint32_t next = 0;
        size_t width = decode(bytes + end, length - end, &next);
        if (next >= TOKENIZE_OPAQUE_BASE ||
            utf8proc_grapheme_break_stateful((utf8proc_int32_t)previous, (utf8proc_int32_t)next,
                                             &state))
            break;
        previous = next;
        end += width;
    }
    return end;
}
static tl_status normalize_cluster(const char *bytes, size_t length, uint32_t first,
                                   uint32_t *symbols, size_t capacity, size_t *out_length) {
    if (first >= TOKENIZE_OPAQUE_BASE || (length == 1 && first < 0x80)) {
        if (capacity == 0)
            return TL_LIMIT;
        symbols[0] = first >= 'A' && first <= 'Z' ? first + ('a' - 'A') : first;
        *out_length = 1;
        return TL_OK;
    }
    utf8proc_option_t options = UTF8PROC_CASEFOLD | UTF8PROC_COMPOSE | UTF8PROC_STABLE;
    utf8proc_ssize_t count =
        utf8proc_decompose((const utf8proc_uint8_t *)bytes, (utf8proc_ssize_t)length,
                           (utf8proc_int32_t *)symbols, (utf8proc_ssize_t)capacity, options);
    if (count < 0)
        return TL_INVALID;
    if ((size_t)count > capacity)
        return TL_LIMIT;
    count = utf8proc_normalize_utf32((utf8proc_int32_t *)symbols, count, options);
    if (count < 0)
        return TL_INVALID;
    *out_length = (size_t)count;
    return TL_OK;
}
static bool camel_boundary(uint32_t previous, uint32_t current) {
    if (previous >= TOKENIZE_OPAQUE_BASE || current >= TOKENIZE_OPAQUE_BASE)
        return false;
    return utf8proc_category((utf8proc_int32_t)previous) == UTF8PROC_CATEGORY_LL &&
           utf8proc_category((utf8proc_int32_t)current) == UTF8PROC_CATEGORY_LU;
}
static tl_status required_capacity(const char *path, size_t length, size_t *out) {
    *out = 0;
    if (length > PTRDIFF_MAX)
        return TL_LIMIT;
    for (size_t start = 0; start < length;) {
        uint32_t first = 0;
        (void)decode(path + start, length - start, &first);
        size_t end = cluster_end(path, length, start, first);
        utf8proc_ssize_t count = 1;
        if (first < TOKENIZE_OPAQUE_BASE && end - start > 1) {
            count = utf8proc_decompose((const utf8proc_uint8_t *)path + start,
                                       (utf8proc_ssize_t)(end - start), NULL, 0,
                                       UTF8PROC_CASEFOLD | UTF8PROC_COMPOSE | UTF8PROC_STABLE);
        }
        if (count < 0)
            return TL_INVALID;
        if ((size_t)count > SIZE_MAX - *out)
            return TL_LIMIT;
        *out += (size_t)count;
        start = end;
    }
    if (*out == 0)
        *out = 1;
    return TL_OK;
}
tl_status tokenize_into(const char *bytes, size_t length, uint32_t *symbols, uint8_t *boundaries,
                        size_t *byte_offsets, size_t capacity, tl_text *out) {
    if (bytes == NULL || symbols == NULL || boundaries == NULL || byte_offsets == NULL ||
        out == NULL)
        return TL_INVALID;
    if (length > PTRDIFF_MAX || capacity > PTRDIFF_MAX)
        return TL_LIMIT;
    if (memchr(bytes, 0, length) != NULL)
        return TL_INVALID;
    *out = (tl_text){.symbols = symbols, .boundaries = boundaries, .byte_offsets = byte_offsets};
    uint32_t previous = '/';
    for (size_t start = 0; start < length;) {
        uint32_t first = 0;
        (void)decode(bytes + start, length - start, &first);
        size_t end = cluster_end(bytes, length, start, first), count = 0;
        size_t position = out->length;
        tl_status status = normalize_cluster(bytes + start, end - start, first, symbols + position,
                                             capacity - position, &count);
        if (status != TL_OK)
            return status;
        for (size_t i = 0; i < count; i++) {
            boundaries[position + i] = (uint8_t)(i == 0 && (tokenize_separator(previous) ||
                                                            camel_boundary(previous, first)));
            byte_offsets[position + i] = start;
            out->mask |= tokenize_symbol_mask(symbols[position + i]);
            if (symbols[position + i] == '/')
                out->basename = position + i + 1;
        }
        out->length += count;
        previous = first;
        start = end;
    }
    return TL_OK;
}
tl_status tokenize_create(const char *path, tl_tokenized **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (path == NULL)
        return TL_INVALID;
    size_t length = strlen(path), capacity = 0, bytes = 0;
    tl_status status = required_capacity(path, length, &capacity);
    if (status != TL_OK)
        return status;
    status = tl_size_multiply(capacity, sizeof(size_t), &bytes);
    if (status != TL_OK)
        return status;
    tl_tokenized *text = calloc(1, sizeof(*text));
    if (text == NULL)
        return TL_NOMEM;
    text->symbols = calloc(capacity, sizeof(uint32_t));
    text->boundaries = malloc(capacity);
    text->offsets = malloc(bytes);
    if (text->symbols == NULL || text->boundaries == NULL || text->offsets == NULL) {
        status = TL_NOMEM;
        goto cleanup;
    }
    status = tokenize_into(path, length, text->symbols, text->boundaries, text->offsets, capacity,
                           &text->view);
    if (status != TL_OK)
        goto cleanup;
    *out = text;
    return TL_OK;
cleanup:
    tokenize_destroy(text);
    return status;
}
void tokenize_destroy(tl_tokenized *text) {
    if (text == NULL)
        return;
    free(text->symbols);
    free(text->boundaries);
    free(text->offsets);
    free(text);
}
tl_text tokenize_view(const tl_tokenized *text) {
    if (text == NULL) {
        tl_text empty = {0};
        return empty;
    }
    return text->view;
}
tl_status tokenize_display_create(const char *path, char **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (path == NULL)
        return TL_INVALID;
    size_t length = strlen(path), capacity = 0;
    tl_status status = tl_size_multiply(length, 4, &capacity);
    if (status != TL_OK || capacity == SIZE_MAX)
        return TL_LIMIT;
    char *display = malloc(capacity + 1);
    if (display == NULL)
        return TL_NOMEM;
    size_t position = 0;
    const char hex[] = "0123456789abcdef";
    for (size_t offset = 0; offset < length;) {
        uint32_t symbol = 0;
        size_t width = decode(path + offset, length - offset, &symbol);
        if (symbol >= TOKENIZE_OPAQUE_BASE) {
            memcpy(display + position, "\xef\xbf\xbd", 3);
            position += 3;
        } else if (symbol < 32 || symbol == 127) {
            display[position++] = '\\';
            display[position++] = 'x';
            display[position++] = hex[symbol >> 4];
            display[position++] = hex[symbol & 15];
        } else if (symbol == '\\') {
            display[position++] = '\\';
            display[position++] = '\\';
        } else {
            memcpy(display + position, path + offset, width);
            position += width;
        }
        offset += width;
    }
    display[position] = 0;
    *out = display;
    return TL_OK;
}
void tokenize_display_destroy(char *display) {
    free(display);
}
