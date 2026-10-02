/* Strict bounded JSON and byte-path encoding; all memory belongs to callers. */
#include "torchlight/json.h"
#include <stdio.h>
#include <string.h>
#include <utf8proc.h>
struct parser {
    const char *text;
    size_t length, offset, capacity, count;
    tl_json_token *tokens;
};
static void whitespace(struct parser *p) {
    while (p->offset < p->length && strchr(" \t\r\n", p->text[p->offset]) != NULL)
        p->offset++;
}
static int hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static tl_status scalar(const char *text, size_t length, size_t *offset, uint32_t *out) {
    if (length - *offset < 4)
        return TL_INVALID;
    uint32_t code = 0;
    for (size_t i = 0; i < 4; i++) {
        int digit = hex(text[(*offset)++]);
        if (digit < 0)
            return TL_INVALID;
        code = code * 16 + (uint32_t)digit;
    }
    if (code >= 0xd800 && code <= 0xdbff) {
        if (length - *offset < 6 || text[*offset] != '\\' || text[*offset + 1] != 'u')
            return TL_INVALID;
        *offset += 2;
        uint32_t low = 0;
        for (size_t i = 0; i < 4; i++) {
            int digit = hex(text[(*offset)++]);
            if (digit < 0)
                return TL_INVALID;
            low = low * 16 + (uint32_t)digit;
        }
        if (low < 0xdc00 || low > 0xdfff)
            return TL_INVALID;
        code = 0x10000 + (code - 0xd800) * 1024 + low - 0xdc00;
    } else if (code >= 0xdc00 && code <= 0xdfff)
        return TL_INVALID;
    if (code == 0)
        return TL_INVALID;
    *out = code;
    return TL_OK;
}
static tl_status string_end(struct parser *p) {
    p->offset++;
    while (p->offset < p->length) {
        unsigned char c = (unsigned char)p->text[p->offset++];
        if (c == '"')
            return TL_OK;
        if (c < 32)
            return TL_INVALID;
        if (c != '\\')
            continue;
        if (p->offset == p->length)
            return TL_INVALID;
        char escape = p->text[p->offset++];
        if (escape == 'u') {
            uint32_t code = 0;
            tl_status status = scalar(p->text, p->length, &p->offset, &code);
            if (status != TL_OK)
                return status;
        } else if (strchr("\"\\/bfnrt", escape) == NULL)
            return TL_INVALID;
    }
    return TL_INVALID;
}
static tl_status value(struct parser *p, size_t depth);
static tl_status container(struct parser *p, size_t depth, bool object) {
    char close = object ? '}' : ']';
    p->offset++;
    whitespace(p);
    if (p->offset < p->length && p->text[p->offset] == close) {
        p->offset++;
        return TL_OK;
    }
    for (;;) {
        whitespace(p);
        if (object) {
            if (p->offset == p->length || p->text[p->offset] != '"')
                return TL_INVALID;
            tl_status status = value(p, depth + 1);
            if (status != TL_OK)
                return status;
            whitespace(p);
            if (p->offset == p->length || p->text[p->offset++] != ':')
                return TL_INVALID;
        }
        tl_status status = value(p, depth + 1);
        if (status != TL_OK)
            return status;
        whitespace(p);
        if (p->offset == p->length)
            return TL_INVALID;
        char c = p->text[p->offset++];
        if (c == close)
            return TL_OK;
        if (c != ',')
            return TL_INVALID;
    }
}
static tl_status number_end(struct parser *p) {
    size_t start = p->offset;
    if (p->text[p->offset] == '-')
        p->offset++;
    if (p->offset == p->length)
        return TL_INVALID;
    if (p->text[p->offset] == '0')
        p->offset++;
    else {
        if (p->text[p->offset] < '1' || p->text[p->offset] > '9')
            return TL_INVALID;
        while (p->offset < p->length && p->text[p->offset] >= '0' && p->text[p->offset] <= '9')
            p->offset++;
    }
    if (p->offset < p->length && p->text[p->offset] == '.') {
        size_t digits = ++p->offset;
        while (p->offset < p->length && p->text[p->offset] >= '0' && p->text[p->offset] <= '9')
            p->offset++;
        if (p->offset == digits)
            return TL_INVALID;
    }
    if (p->offset < p->length && strchr("eE", p->text[p->offset]) != NULL) {
        p->offset++;
        if (p->offset < p->length && strchr("+-", p->text[p->offset]) != NULL)
            p->offset++;
        size_t digits = p->offset;
        while (p->offset < p->length && p->text[p->offset] >= '0' && p->text[p->offset] <= '9')
            p->offset++;
        if (p->offset == digits)
            return TL_INVALID;
    }
    return p->offset > start ? TL_OK : TL_INVALID;
}
static tl_status value(struct parser *p, size_t depth) {
    whitespace(p);
    if (depth > JSON_MAX_DEPTH)
        return TL_LIMIT;
    if (p->offset == p->length)
        return TL_INVALID;
    if (p->count == p->capacity)
        return TL_LIMIT;
    size_t index = p->count++;
    tl_json_token *token = &p->tokens[index];
    token->start = p->offset;
    char c = p->text[p->offset];
    tl_status status = TL_OK;
    if (c == '{' || c == '[') {
        token->type = c == '{' ? JSON_OBJECT : JSON_ARRAY;
        status = container(p, depth, c == '{');
    } else if (c == '"') {
        token->type = JSON_STRING;
        status = string_end(p);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        token->type = JSON_NUMBER;
        status = number_end(p);
    } else {
        const char *literal = c == 't' ? "true" : c == 'f' ? "false" : "null";
        size_t length = strlen(literal);
        token->type = c == 'n' ? JSON_NULL : JSON_BOOL;
        if (p->length - p->offset < length || memcmp(p->text + p->offset, literal, length) != 0)
            return TL_INVALID;
        p->offset += length;
    }
    token->end = p->offset;
    token->next = p->count;
    return status;
}
bool json_utf8(const char *text) {
    if (text == NULL)
        return false;
    size_t length = strlen(text), offset = 0;
    while (offset < length) {
        utf8proc_int32_t code = 0;
        utf8proc_ssize_t used = utf8proc_iterate((const utf8proc_uint8_t *)text + offset,
                                                 (utf8proc_ssize_t)(length - offset), &code);
        if (used <= 0)
            return false;
        offset += (size_t)used;
    }
    return true;
}
tl_status json_string(const tl_json *json, size_t token, char *out, size_t capacity) {
    if (json == NULL || token >= json->count || out == NULL || capacity == 0 ||
        json->tokens[token].type != JSON_STRING)
        return TL_INVALID;
    size_t offset = json->tokens[token].start + 1, end = json->tokens[token].end - 1, written = 0;
    while (offset < end) {
        unsigned char bytes[4];
        size_t count = 1;
        bytes[0] = (unsigned char)json->text[offset++];
        if (bytes[0] == '\\') {
            char escape = json->text[offset++];
            if (escape == 'u') {
                uint32_t code = 0;
                tl_status status = scalar(json->text, end, &offset, &code);
                if (status != TL_OK)
                    return status;
                count = (size_t)utf8proc_encode_char((utf8proc_int32_t)code, bytes);
            } else {
                const char *keys = "bfnrt", *found = strchr(keys, escape);
                const char controls[] = "\b\f\n\r\t";
                bytes[0] = (unsigned char)(found == NULL ? escape : controls[found - keys]);
            }
        }
        if (count >= capacity - written)
            return TL_LIMIT;
        memcpy(out + written, bytes, count);
        written += count;
    }
    out[written] = 0;
    return TL_OK;
}
size_t json_member(const tl_json *json, size_t object, const char *key) {
    if (json == NULL || key == NULL || object >= json->count ||
        json->tokens[object].type != JSON_OBJECT)
        return SIZE_MAX;
    char decoded[JSON_KEY_BYTES + 1];
    for (size_t i = object + 1; i < json->tokens[object].next;) {
        if (json_string(json, i, decoded, sizeof(decoded)) == TL_OK && strcmp(decoded, key) == 0)
            return i + 1;
        i = json->tokens[i + 1].next;
    }
    return SIZE_MAX;
}
tl_status json_parse(const char *text, size_t length, tl_json_token *tokens, size_t capacity,
                     tl_json *out) {
    if (out == NULL)
        return TL_INVALID;
    *out = (tl_json){0};
    if (text == NULL || tokens == NULL || memchr(text, 0, length) != NULL || length > INT32_MAX)
        return TL_INVALID;
    /* Validate UTF-8 without assuming a terminator at length. */
    for (size_t i = 0; i < length;) {
        utf8proc_int32_t code = 0;
        utf8proc_ssize_t used = utf8proc_iterate((const utf8proc_uint8_t *)text + i,
                                                 (utf8proc_ssize_t)(length - i), &code);
        if (used <= 0)
            return TL_INVALID;
        i += (size_t)used;
    }
    struct parser p = {.text = text, .length = length, .capacity = capacity, .tokens = tokens};
    tl_status status = value(&p, 0);
    whitespace(&p);
    if (status == TL_OK && p.offset != length)
        status = TL_INVALID;
    tl_json parsed = {text, tokens, p.count};
    for (size_t i = 0; i < p.count && status == TL_OK; i++) {
        if (tokens[i].type != JSON_OBJECT)
            continue;
        for (size_t key = i + 1; key < tokens[i].next;) {
            char decoded[JSON_KEY_BYTES + 1];
            status = json_string(&parsed, key, decoded, sizeof(decoded));
            if (status != TL_OK)
                break;
            if (json_member(&parsed, i, decoded) != key + 1) {
                status = TL_INVALID;
                break;
            }
            key = tokens[key + 1].next;
        }
    }
    if (status == TL_OK)
        *out = parsed;
    return status;
}
tl_status json_uint(const tl_json *json, size_t token, uint64_t *out) {
    if (json == NULL || token >= json->count || out == NULL)
        return TL_INVALID;
    tl_json_token t = json->tokens[token];
    if (t.type != JSON_NUMBER && t.type != JSON_STRING)
        return TL_INVALID;
    size_t begin = t.start + (t.type == JSON_STRING ? 1U : 0U),
           end = t.end - (t.type == JSON_STRING ? 1U : 0U);
    if (begin == end)
        return TL_INVALID;
    uint64_t number = 0;
    for (size_t i = begin; i < end; i++) {
        char c = json->text[i];
        if (c < '0' || c > '9')
            return TL_INVALID;
        if (number > (UINT64_MAX - (uint64_t)(c - '0')) / 10)
            return TL_LIMIT;
        number = number * 10 + (uint64_t)(c - '0');
    }
    *out = number;
    return TL_OK;
}
void json_buffer_init(tl_json_buffer *b, char *data, size_t capacity) {
    *b = (tl_json_buffer){data, capacity, 0, data == NULL || capacity == 0 ? TL_INVALID : TL_OK};
    if (b->status == TL_OK)
        data[0] = 0;
}
static void append(tl_json_buffer *b, const char *text, size_t length) {
    if (b->status != TL_OK)
        return;
    if (length >= b->capacity - b->length) {
        b->status = TL_LIMIT;
        return;
    }
    memcpy(b->data + b->length, text, length);
    b->length += length;
    b->data[b->length] = 0;
}
void json_raw(tl_json_buffer *b, const char *text) {
    append(b, text, strlen(text));
}
void json_number(tl_json_buffer *b, uint64_t value) {
    char number[32];
    int count = snprintf(number, sizeof(number), "%llu", (unsigned long long)value);
    if (count < 0) {
        b->status = TL_IO;
        return;
    }
    append(b, number, (size_t)count);
}
void json_quote(tl_json_buffer *b, const char *text) {
    json_raw(b, "\"");
    size_t length = strlen(text);
    for (size_t i = 0; i < length;) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32 || c == '"' || c == '\\') {
            char escaped[7];
            int count = snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)c);
            if (count < 0) {
                b->status = TL_IO;
                return;
            }
            append(b, escaped, (size_t)count);
            i++;
            continue;
        }
        utf8proc_int32_t code = 0;
        utf8proc_ssize_t used = utf8proc_iterate((const utf8proc_uint8_t *)text + i,
                                                 (utf8proc_ssize_t)(length - i), &code);
        if (used <= 0) {
            json_raw(b, "\xef\xbf\xbd");
            i++;
        } else {
            append(b, text + i, (size_t)used);
            i += (size_t)used;
        }
    }
    json_raw(b, "\"");
}
static const char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
void json_base64(tl_json_buffer *b, const char *bytes) {
    json_raw(b, "\"");
    size_t length = strlen(bytes);
    for (size_t i = 0; i < length; i += 3) {
        uint32_t bits = (uint32_t)(unsigned char)bytes[i] << 16;
        if (i + 1 < length)
            bits |= (uint32_t)(unsigned char)bytes[i + 1] << 8;
        if (i + 2 < length)
            bits |= (unsigned char)bytes[i + 2];
        char encoded[4] = {BASE64[bits >> 18], BASE64[(bits >> 12) & 63],
                           i + 1 < length ? BASE64[(bits >> 6) & 63] : '=',
                           i + 2 < length ? BASE64[bits & 63] : '='};
        append(b, encoded, 4);
    }
    json_raw(b, "\"");
}
tl_status json_unbase64(const char *text, char *out, size_t capacity) {
    if (text == NULL || out == NULL || capacity == 0)
        return TL_INVALID;
    size_t length = strlen(text), written = 0;
    if (length % 4 != 0)
        return TL_INVALID;
    for (size_t i = 0; i < length; i += 4) {
        uint32_t bits = 0;
        size_t padding = 0;
        for (size_t j = 0; j < 4; j++) {
            const char *digit = text[i + j] == 0 ? NULL : strchr(BASE64, text[i + j]);
            if (text[i + j] == '=' && j >= 2 && i + 4 == length) {
                padding++;
                bits <<= 6;
            } else if (digit != NULL && padding == 0)
                bits = (bits << 6) | (uint32_t)(digit - BASE64);
            else
                return TL_INVALID;
        }
        if ((padding == 1 && (bits & 255) != 0) || (padding == 2 && (bits & 65535) != 0))
            return TL_INVALID;
        for (size_t j = 0; j < 3 - padding; j++) {
            unsigned char c = (unsigned char)(bits >> (16 - j * 8));
            if (c == 0)
                return TL_INVALID;
            if (written + 1 >= capacity)
                return TL_LIMIT;
            out[written++] = (char)c;
        }
    }
    out[written] = 0;
    return TL_OK;
}
