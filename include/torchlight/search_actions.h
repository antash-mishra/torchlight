/* Shared launcher modes, feature configuration and bounded browser-search targets. */
#ifndef TORCHLIGHT_SEARCH_ACTIONS_H
#define TORCHLIGHT_SEARCH_ACTIONS_H
#include "torchlight/config.h"
#include "torchlight/json.h"
#include "torchlight/lexical.h"
#define SEARCH_ACTION_URI_BYTES (3 * LEXICAL_QUERY_BYTES + 128)
#define SEARCH_BROWSER_BYTES 256
#define SEARCH_BROWSER_NAME_BYTES 256
typedef enum { SEARCH_LOCAL, SEARCH_WEB, SEARCH_SOFTWARE } tl_search_mode;
typedef enum { SEARCH_DUCKDUCKGO, SEARCH_GOOGLE } tl_search_engine;
/* Client preferences; an unset engine inherits the daemon's configured engine. */
typedef struct {
    bool override_engine;
    tl_search_engine engine;
    char browser[SEARCH_BROWSER_BYTES], browser_name[SEARCH_BROWSER_NAME_BYTES];
} tl_web_options;
typedef struct {
    bool web, software;
    tl_search_engine engine;
} tl_search_settings;
typedef struct {
    tl_search_mode mode;
    bool web_fallback;
    char query[LEXICAL_QUERY_BYTES + 1];
} tl_search_input;
/** Validate all domain configuration keys and copy feature settings into out.
 * NULL config uses defaults: web on, DuckDuckGo, optional software auto-enabled.
 * Repeated feature keys, unknown keys or invalid values return TL_INVALID.
 * No ownership transfer or allocation; out is unchanged on error. */
tl_status search_actions_settings(const tl_config *config, tl_search_settings *out);
/** Whether key is a known domain setting (paths or actions); borrowed, NULL false. */
bool search_actions_key(const char *key);
/** Parse popup-only web/install/local prefixes, case-insensitively and only when
 * followed by ASCII whitespace. Trim outer whitespace; preserve internal bytes.
 * Copies valid UTF-8 into out. TL_INVALID/LIMIT, out zero on error; no allocation. */
tl_status search_actions_parse(const char *text, tl_search_input *out);
/** Build a fixed HTTPS search URL with query encoded as one parameter. Inputs
 * borrowed; out caller-owned, empty on TL_INVALID/LIMIT. No I/O or allocation. */
tl_status search_actions_uri(tl_search_engine engine, const char *query, char *out,
                             size_t capacity);
/** Recognize a website address and copy a safe HTTP(S) URI to caller out.
 * Explicit HTTP(S) supports DNS hosts, IPv4/IPv6 and ports. Bare addresses use
 * a conservative suffix list, IPv4, or localhost with a port; other text and
 * filenames remain searches. No DNS, I/O or allocation. TL_INVALID for an
 * unrecognized/unsafe address, TL_LIMIT for capacity; out is empty on failure. */
tl_status search_actions_address(const char *text, char *out, size_t capacity);
/** Validate a desktop-file browser id, or empty/default for the system handler.
 * Borrowed UTF-8 input; false for unsafe paths/commands or NULL, no I/O. */
bool search_actions_browser(const char *id);
/** Return a static engine identifier or NULL for an invalid enum; no ownership. */
const char *search_actions_engine(tl_search_engine engine);
/** Decode a built-in engine identifier into caller out; TL_INVALID otherwise. */
tl_status search_actions_engine_parse(const char *name, tl_search_engine *out);
/** Append the at-most-one Web action as an actions array member. Query and
 * settings are borrowed. No network, allocation or history; errors in buffer. */
void search_actions_encode(tl_json_buffer *buffer, const tl_search_settings *settings,
                           const char *query, bool include);
#endif
