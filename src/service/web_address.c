/* Conservative, allocation-free website recognition; never probes a host or file. */
#include "torchlight/search_actions.h"
#include <arpa/inet.h>
#include <string.h>
#include <strings.h>

static bool ascii_letter(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool digit(unsigned char c) {
    return c >= '0' && c <= '9';
}
static bool hex(unsigned char c) {
    return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool search_actions_browser(const char *id) {
    if (id == NULL || strlen(id) >= SEARCH_BROWSER_BYTES)
        return false;
    if (id[0] == 0 || strcmp(id, "default") == 0)
        return true;
    size_t length = strlen(id);
    if (length <= strlen(".desktop") || strcmp(id + length - strlen(".desktop"), ".desktop") != 0 ||
        !ascii_letter((unsigned char)id[0]))
        return false;
    for (size_t i = 0; i < length; i++)
        if (!ascii_letter((unsigned char)id[i]) && !digit((unsigned char)id[i]) && id[i] != '.' &&
            id[i] != '-' && id[i] != '_')
            return false;
    return true;
}
static bool port_valid(const char *text) {
    unsigned port = 0;
    if (*text == 0 || strlen(text) > 5)
        return false;
    for (; *text != 0; text++) {
        if (!digit((unsigned char)*text))
            return false;
        port = port * 10 + (unsigned)(*text - '0');
    }
    return port > 0 && port <= 65535;
}
static bool hostname_valid(const char *host) {
    size_t length = strlen(host), label = 0;
    if (length == 0 || length > 253)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)host[i];
        if (c == '.') {
            if (label == 0 || host[i - 1] == '-')
                return false;
            label = 0;
        } else {
            if ((!ascii_letter(c) && !digit(c) && c != '-') || (label == 0 && c == '-') ||
                ++label > 63)
                return false;
        }
    }
    return label > 0 && host[length - 1] != '-';
}
static bool common_domain(const char *host) {
    /* Deliberately excludes file suffixes (pdf, py, rs, js, sh, zip, ...).
     * Less common domains remain accessible with an explicit https:// prefix. */
    static const char *const SUFFIXES[] = {"com",  "org", "net",    "edu",  "gov",  "io",    "dev",
                                           "app",  "ai",  "co",     "uk",   "us",   "de",    "fr",
                                           "in",   "ca",  "au",     "jp",   "eu",   "me",    "tv",
                                           "info", "biz", "online", "site", "tech", "cloud", "xyz"};
    const char *dot = strrchr(host, '.');
    if (dot == NULL)
        return false;
    for (size_t i = 0; i < sizeof(SUFFIXES) / sizeof(SUFFIXES[0]); i++)
        if (strcasecmp(dot + 1, SUFFIXES[i]) == 0)
            return true;
    return false;
}
static bool authority_valid(char *host, bool explicit_scheme) {
    unsigned char address[sizeof(struct in6_addr)];
    if (host[0] == '[') {
        char *close = strchr(host, ']');
        if (!explicit_scheme || close == NULL ||
            (close[1] != 0 && (close[1] != ':' || !port_valid(close + 2))))
            return false;
        *close = 0;
        return inet_pton(AF_INET6, host + 1, address) == 1;
    }
    char *port = strchr(host, ':');
    if (port != NULL) {
        *port++ = 0;
        if (!port_valid(port))
            return false;
    }
    if (!hostname_valid(host))
        return false;
    if (explicit_scheme || common_domain(host) || inet_pton(AF_INET, host, address) == 1)
        return true;
    return port != NULL && strcasecmp(host, "localhost") == 0;
}
static bool uri_bytes_valid(const char *text) {
    for (size_t i = 0; text[i] != 0; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c <= 32 || c == 127 || c == '\\' || c == '<' || c == '>' || c == '"' || c == '`')
            return false;
        if (c == '%' && (!hex((unsigned char)text[i + 1]) || text[i + 1] == 0 ||
                         !hex((unsigned char)text[i + 2])))
            return false;
    }
    return true;
}
tl_status search_actions_address(const char *text, char *out, size_t capacity) {
    if (out == NULL || capacity == 0)
        return TL_INVALID;
    out[0] = 0;
    if (text == NULL || !json_utf8(text) || strlen(text) > LEXICAL_QUERY_BYTES ||
        !uri_bytes_valid(text))
        return TL_INVALID;
    size_t scheme = strncasecmp(text, "https://", 8) == 0  ? 8
                    : strncasecmp(text, "http://", 7) == 0 ? 7
                                                           : 0;
    const char *host = text + scheme;
    size_t length = strcspn(host, "/?#");
    char authority[LEXICAL_QUERY_BYTES + 1];
    memcpy(authority, host, length);
    authority[length] = 0;
    if (!authority_valid(authority, scheme != 0))
        return TL_INVALID;
    char uri[SEARCH_ACTION_URI_BYTES];
    const char *prefix = scheme == 7 ? "http://" : "https://";
    size_t used = strlen(prefix);
    memcpy(uri, prefix, used);
    static const char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0; host[i] != 0; i++) {
        unsigned char c = (unsigned char)host[i];
        if (c < 128)
            uri[used++] = (char)c;
        else {
            uri[used++] = '%';
            uri[used++] = HEX[c >> 4];
            uri[used++] = HEX[c & 15];
        }
    }
    if (used >= capacity)
        return TL_LIMIT;
    uri[used] = 0;
    memcpy(out, uri, used + 1);
    return TL_OK;
}
