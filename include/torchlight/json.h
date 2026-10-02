/* Bounded JSON parsing and encoding into caller-owned buffers. */
#ifndef TORCHLIGHT_JSON_H
#define TORCHLIGHT_JSON_H
#include "torchlight/common.h"
#include <stdbool.h>
#define JSON_MAX_DEPTH 8
#define JSON_KEY_BYTES 127
typedef enum {
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_STRING,
    JSON_NUMBER,
    JSON_BOOL,
    JSON_NULL
} tl_json_type;
typedef struct {
    tl_json_type type;
    size_t start, end, next;
} tl_json_token;
typedef struct {
    const char *text;
    tl_json_token *tokens;
    size_t count;
} tl_json;
typedef struct {
    char *data;
    size_t capacity, length;
    tl_status status;
} tl_json_buffer;
/** Parse length bytes without allocating. Tokens borrow text/tokens until reuse.
 * Reject malformed UTF-8, duplicate object keys, NUL and depth > 8. Object keys
 * have at most 127 decoded bytes. TL_LIMIT for
 * token exhaustion, TL_INVALID for syntax/arguments. out is empty on failure. */
tl_status json_parse(const char *text, size_t length, tl_json_token *tokens, size_t capacity,
                     tl_json *out);
/** Find a member in an object; SIZE_MAX when missing/wrong type. No ownership. */
size_t json_member(const tl_json *json, size_t object, const char *key);
/** Decode a string into caller buffer, including terminator. Reject decoded NUL.
 * TL_INVALID for wrong type/arguments, TL_LIMIT for capacity. No allocation. */
tl_status json_string(const tl_json *json, size_t token, char *out, size_t capacity);
/** Read an unsigned integer (no signs/fractions); TL_INVALID/LIMIT. No ownership. */
tl_status json_uint(const tl_json *json, size_t token, uint64_t *out);
/** Initialize borrowed output buffer; capacity includes NUL. No errors. */
void json_buffer_init(tl_json_buffer *buffer, char *data, size_t capacity);
/** Append trusted JSON syntax. Errors retained in buffer.status; no allocation. */
void json_raw(tl_json_buffer *buffer, const char *text);
/** Append escaped valid UTF-8 string. Invalid bytes become U+FFFD; no allocation. */
void json_quote(tl_json_buffer *buffer, const char *text);
/** Append decimal integer. Errors retained in buffer.status; no ownership. */
void json_number(tl_json_buffer *buffer, uint64_t value);
/** Check NUL-terminated bytes for valid UTF-8. false for NULL; no errors. */
bool json_utf8(const char *text);
/** Encode raw NUL-free bytes as a quoted base64 string; no allocation. */
void json_base64(tl_json_buffer *buffer, const char *bytes);
/** Decode canonical base64 into caller buffer with terminator. Reject embedded
 * NUL/noncanonical encodings. TL_INVALID/LIMIT; no allocation or ownership. */
tl_status json_unbase64(const char *text, char *out, size_t capacity);
#endif
