/* Strict JSON regressions, Unicode escapes and byte-path base64 round trips. */
#include "test.h"
#include "torchlight/json.h"
#include "torchlight/path.h"
#include <string.h>
static tl_status parse_text(const char *text) {
    tl_json_token tokens[128];
    tl_json json;
    return json_parse(text, strlen(text), tokens, 128, &json);
}
static void malformed_json(void) {
    const char *bad[] = {"",
                         " ",
                         "{",
                         "[",
                         "[1,]",
                         "{\"a\":1,}",
                         "{\"a\" 1}",
                         "[01]",
                         "[+1]",
                         "[-]",
                         "[1.]",
                         "[1e]",
                         "[.1]",
                         "true false",
                         "tru",
                         "[null,null",
                         "\"\\x\"",
                         "\"\\u0000\"",
                         "\"\\ud800\"",
                         "\"\\udc00\"",
                         "\"\\ud800\\u0001\"",
                         "\"\xff\"",
                         "\"\n\"",
                         "{\"a\":1,\"\\u0061\":2}",
                         "{\"x\":{\"a\":1,\"a\":2}}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(parse_text(bad[i]) == TL_INVALID);
    CHECK(parse_text("[[[[[[[[[1]]]]]]]]]") == TL_LIMIT);
    CHECK(parse_text("[0,-1,2.5,1e+2,true,false,null,{},[]]") == TL_OK);
    tl_json_token token;
    tl_json json;
    CHECK(json_parse("[1]", 3, &token, 1, &json) == TL_LIMIT && json.count == 0);
    CHECK(json_parse("null\0", 5, &token, 1, &json) == TL_INVALID);
}
static void encoding(void) {
    const char *text =
        "{\"key\":\"\\ud83d\\ude00\\n\\t\\\"\\\\\",\"nested\":[{\"id\":\"18446744073709551615\"}]}";
    tl_json_token tokens[64];
    tl_json json;
    char value[64];
    uint64_t number = 0;
    CHECK(json_parse(text, strlen(text), tokens, 64, &json) == TL_OK);
    CHECK(json_string(&json, json_member(&json, 0, "key"), value, sizeof(value)) == TL_OK);
    CHECK(strcmp(value, "\xf0\x9f\x98\x80\n\t\"\\") == 0);
    size_t nested = json_member(&json, 0, "nested");
    CHECK(json_uint(&json, json_member(&json, nested + 1, "id"), &number) == TL_OK &&
          number == UINT64_MAX);
    CHECK(json_member(&json, 0, "missing") == SIZE_MAX);
    CHECK(json_string(&json, json_member(&json, 0, "key"), value, 2) == TL_LIMIT);
    char encoded[512];
    tl_json_buffer buffer;
    json_buffer_init(&buffer, encoded, sizeof(encoded));
    json_quote(&buffer, "line\n\xff\"\t");
    CHECK(buffer.status == TL_OK && parse_text(encoded) == TL_OK);
    CHECK(json_parse(encoded, strlen(encoded), tokens, 64, &json) == TL_OK);
    CHECK(json_string(&json, 0, value, sizeof(value)) == TL_OK &&
          strcmp(value, "line\n\xef\xbf\xbd\"\t") == 0);
    json_buffer_init(&buffer, encoded, 3);
    json_quote(&buffer, "overflow");
    CHECK(buffer.status == TL_LIMIT);
    CHECK(json_utf8("\xc3\xa9") && !json_utf8("\xc0\xaf") && !json_utf8("\xed\xa0\x80"));
}
static void base64_paths(void) {
    char raw[256], encoded[512], text[512], decoded[512];
    for (size_t i = 0; i < 255; i++)
        raw[i] = (char)(i + 1);
    raw[255] = 0;
    for (size_t length = 1; length <= 255; length++) {
        char saved = raw[length];
        raw[length] = 0;
        tl_json_buffer buffer;
        json_buffer_init(&buffer, encoded, sizeof(encoded));
        json_base64(&buffer, raw);
        tl_json_token tokens[2];
        tl_json json;
        CHECK(buffer.status == TL_OK &&
              json_parse(encoded, strlen(encoded), tokens, 2, &json) == TL_OK);
        CHECK(json_string(&json, 0, text, sizeof(text)) == TL_OK);
        CHECK(json_unbase64(text, decoded, sizeof(decoded)) == TL_OK &&
              memcmp(raw, decoded, length + 1) == 0);
        raw[length] = saved;
    }
    const char *bad[] = {"a", "====", "AA==", "YQ=Z", "YR==", "YWF=", "YQ==AAAA", "YQ==\n", "____"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(json_unbase64(bad[i], decoded, sizeof(decoded)) == TL_INVALID);
    CHECK(json_unbase64("YQ==", decoded, 1) == TL_LIMIT);
    CHECK(path_within("/a/file", "/a") && !path_within("/ab/file", "/a") &&
          path_within("/any", "/"));
}
void test_json(void) {
    malformed_json();
    encoding();
    base64_paths();
}
