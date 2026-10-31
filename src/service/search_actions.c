/* Domain feature settings and pure browser-search construction; never launches anything. */
#include "torchlight/search_actions.h"
#include <string.h>

static bool whitespace(unsigned char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}
static bool word_equal(const char *text, size_t length, const char *word) {
    if (strlen(word) != length)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + ('a' - 'A'));
        if (c != (unsigned char)word[i])
            return false;
    }
    return true;
}
const char *search_actions_engine(tl_search_engine engine) {
    return engine == SEARCH_DUCKDUCKGO ? "duckduckgo" : engine == SEARCH_GOOGLE ? "google" : NULL;
}
tl_status search_actions_engine_parse(const char *name, tl_search_engine *out) {
    if (name == NULL || out == NULL)
        return TL_INVALID;
    if (strcmp(name, "duckduckgo") == 0)
        *out = SEARCH_DUCKDUCKGO;
    else if (strcmp(name, "google") == 0)
        *out = SEARCH_GOOGLE;
    else
        return TL_INVALID;
    return TL_OK;
}
bool search_actions_key(const char *key) {
    return key != NULL && (strcmp(key, "root") == 0 || strcmp(key, "allow") == 0 ||
                           strcmp(key, "exclude") == 0 || strcmp(key, "web_search") == 0 ||
                           strcmp(key, "web_engine") == 0 || strcmp(key, "software_search") == 0);
}
static tl_status setting(tl_search_settings *settings, const char *key, const char *value,
                         unsigned *seen) {
    unsigned bit = strcmp(key, "web_search") == 0 ? 1U : strcmp(key, "web_engine") == 0 ? 2U : 4U;
    if ((*seen & bit) != 0)
        return TL_INVALID;
    *seen |= bit;
    if (bit == 2)
        return search_actions_engine_parse(value, &settings->engine);
    if (strcmp(value, "false") != 0 && strcmp(value, bit == 1 ? "true" : "auto") != 0)
        return TL_INVALID;
    if (bit == 1)
        settings->web = strcmp(value, "true") == 0;
    else
        settings->software = strcmp(value, "auto") == 0;
    return TL_OK;
}
tl_status search_actions_settings(const tl_config *config, tl_search_settings *out) {
    if (out == NULL)
        return TL_INVALID;
    tl_search_settings settings = {.web = true, .software = true, .engine = SEARCH_DUCKDUCKGO};
    unsigned seen = 0;
    for (size_t i = 0; i < config_entry_count(config); i++) {
        const char *key, *value;
        tl_status status = config_entry(config, i, &key, &value);
        if (status != TL_OK || !search_actions_key(key))
            return TL_INVALID;
        if (strcmp(key, "root") == 0 || strcmp(key, "allow") == 0 || strcmp(key, "exclude") == 0)
            continue;
        status = setting(&settings, key, value, &seen);
        if (status != TL_OK)
            return status;
    }
    *out = settings;
    return TL_OK;
}
tl_status search_actions_parse(const char *text, tl_search_input *out) {
    if (out == NULL)
        return TL_INVALID;
    *out = (tl_search_input){0};
    if (text == NULL || !json_utf8(text))
        return TL_INVALID;
    while (whitespace((unsigned char)*text))
        text++;
    const char *word_end = text;
    while (*word_end != 0 && !whitespace((unsigned char)*word_end))
        word_end++;
    tl_search_input parsed = {.web_fallback = true};
    size_t word_length = (size_t)(word_end - text);
    if (*word_end != 0 &&
        (word_equal(text, word_length, "web") || word_equal(text, word_length, "install") ||
         word_equal(text, word_length, "local"))) {
        parsed.mode = word_equal(text, word_length, "web")       ? SEARCH_WEB
                      : word_equal(text, word_length, "install") ? SEARCH_SOFTWARE
                                                                 : SEARCH_LOCAL;
        parsed.web_fallback = false;
        text = word_end;
        while (whitespace((unsigned char)*text))
            text++;
    }
    size_t length = strlen(text);
    while (length > 0 && whitespace((unsigned char)text[length - 1]))
        length--;
    if (length > LEXICAL_QUERY_BYTES)
        return TL_LIMIT;
    memcpy(parsed.query, text, length);
    parsed.query[length] = 0;
    *out = parsed;
    return TL_OK;
}
tl_status search_actions_uri(tl_search_engine engine, const char *query, char *out,
                             size_t capacity) {
    if (out == NULL || capacity == 0)
        return TL_INVALID;
    out[0] = 0;
    if (query == NULL || query[0] == 0 || !json_utf8(query) ||
        strlen(query) > LEXICAL_QUERY_BYTES || search_actions_engine(engine) == NULL)
        return TL_INVALID;
    const char *endpoint =
        engine == SEARCH_GOOGLE ? "https://www.google.com/search?q=" : "https://duckduckgo.com/?q=";
    char uri[SEARCH_ACTION_URI_BYTES];
    size_t length = strlen(endpoint);
    memcpy(uri, endpoint, length);
    static const char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0; query[i] != 0; i++) {
        unsigned char c = (unsigned char)query[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            uri[length++] = (char)c;
        else {
            uri[length++] = '%';
            uri[length++] = HEX[c >> 4];
            uri[length++] = HEX[c & 15];
        }
    }
    if (length >= capacity)
        return TL_LIMIT;
    uri[length] = 0;
    memcpy(out, uri, length + 1);
    return TL_OK;
}
void search_actions_encode(tl_json_buffer *b, const tl_search_settings *settings, const char *query,
                           bool include) {
    json_raw(b, ",\"actions\":[");
    if (include && settings->web && query[0] != 0) {
        json_raw(b, "{\"kind\":\"web\",\"provider\":");
        json_quote(b, search_actions_engine(settings->engine));
        json_raw(b, ",\"query\":");
        json_quote(b, query);
        json_raw(b, "}");
    }
    json_raw(b, "]");
}
