/* Strict copied IPC results, open windows beneath applications and stable
 * deliberate selection across response phases. */
#include "torchlight/popup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define POPUP_JSON_TOKENS 16384
#define POPUP_NO_ROW SIZE_MAX
/* Distinct results a request can show: two phases of top-k plus one retained row. */
#define POPUP_CHOICES (2 * POPUP_RESULTS + 1)
typedef enum { EXPANSION_COLLAPSED, EXPANSION_SHOWN, EXPANSION_ALL } expansion_state;
/* A user's expand/collapse choice for one result id, kept for one request. */
struct expansion {
    uint64_t id;
    expansion_state state;
};
/* Item identity that survives reordering: its result, kind and window. */
struct item_key {
    tl_popup_row row;
    uint64_t handle;
    tl_popup_item_kind kind;
};
struct tl_popup_model {
    char request_id[IPC_REQUEST_ID_BYTES + 1], search_id[IPC_HISTORY_ID_BYTES + 1];
    tl_popup_row rows[POPUP_ROWS + 1], incoming[POPUP_ROWS];
    char reason[64];
    tl_popup_item items[POPUP_ITEMS];
    /* position indexes items; count is the number of rows. */
    size_t count, item_count, position;
    const tl_windows *windows;
    /* Row that owns each snapshot window, or POPUP_NO_ROW. */
    size_t owners[WINDOWS_MAX];
    struct expansion choices[POPUP_CHOICES];
    size_t choice_count;
    bool deliberate, ready, indexing, degraded;
    bool allow_visit, prefer_software;
    tl_web_options web_options;
    /* The last response's search time; POPUP_SEARCH_TIME_UNKNOWN when it gave none. */
    uint64_t search_us;
    tl_json_token tokens[POPUP_JSON_TOKENS];
    char scratch[POPUP_PATH_BYTES * 2];
};
static tl_status string_field(const tl_json *json, size_t object, const char *key, char *out,
                              size_t capacity, bool optional) {
    size_t token = json_member(json, object, key);
    if (optional && token == SIZE_MAX) {
        out[0] = 0;
        return TL_OK;
    }
    return json_string(json, token, out, capacity);
}
static tl_status single_line(const char *input, char *out, size_t capacity) {
    static const char HEX[] = "0123456789abcdef";
    size_t length = 0;
    for (size_t i = 0; input[i] != 0; i++) {
        unsigned char byte = (unsigned char)input[i];
        size_t need = byte < 32 || byte == 127 ? 4 : 1;
        if (length + need >= capacity)
            return TL_LIMIT;
        if (need == 4) {
            out[length++] = '\\';
            out[length++] = 'x';
            out[length++] = HEX[byte >> 4];
            out[length++] = HEX[byte & 15];
        } else
            out[length++] = input[i];
    }
    out[length] = 0;
    return TL_OK;
}
static tl_status decode_web(tl_popup_model *model, const tl_json *json, size_t token,
                            tl_popup_row *row) {
    row->target_kind = POPUP_TARGET_WEB;
    char provider[32], uri[SEARCH_ACTION_URI_BYTES];
    if (string_field(json, token, "provider", provider, sizeof(provider), false) != TL_OK ||
        search_actions_engine_parse(provider, &row->web_engine) != TL_OK ||
        string_field(json, token, "query", row->web_query, sizeof(row->web_query), false) !=
            TL_OK ||
        search_actions_uri(row->web_engine, row->web_query, uri, sizeof(uri)) != TL_OK)
        return TL_INVALID;
    if (model->web_options.override_engine)
        row->web_engine = model->web_options.engine;
    memcpy(row->web_browser, model->web_options.browser, sizeof(row->web_browser));
    tl_status status = single_line(row->web_query, model->scratch, sizeof(model->scratch));
    if (status != TL_OK)
        return status;
    bool visit = model->allow_visit && search_actions_address(row->web_query, row->web_uri,
                                                              sizeof(row->web_uri)) == TL_OK;
    int size = visit ? snprintf(row->name, sizeof(row->name), "Open %s", model->scratch)
                     : snprintf(row->name, sizeof(row->name), "Search %s for “%s”",
                                row->web_engine == SEARCH_GOOGLE ? "Google" : "DuckDuckGo",
                                model->scratch);
    if (size < 0 || (size_t)size >= sizeof(row->name))
        return TL_LIMIT;
    memcpy(row->icon, "web-browser-symbolic", sizeof("web-browser-symbolic"));
    const char *browser = model->web_options.browser_name[0] == 0 ? "Default browser"
                                                                  : model->web_options.browser_name;
    return single_line(browser, row->display, sizeof(row->display));
}
static tl_status decode_action(tl_popup_model *model, const tl_json *json, size_t token,
                               tl_popup_row *row, const char *kind) {
    if (json_member(json, token, "path") != SIZE_MAX ||
        json_member(json, token, "path_b64") != SIZE_MAX ||
        json_member(json, token, "id") != SIZE_MAX ||
        json_member(json, token, "desktop_id") != SIZE_MAX)
        return TL_INVALID;
    if (strcmp(kind, "web") == 0)
        return decode_web(model, json, token, row);
    row->target_kind = POPUP_TARGET_SOFTWARE;
    tl_status status = ipc_software_decode(json, json_member(json, token, "target"), &row->software,
                                           &row->software_revision, &row->source_revision);
    if (status != TL_OK)
        return status;
    size_t installed = json_member(json, token, "installed");
    if (installed == SIZE_MAX || json->tokens[installed].type != JSON_BOOL)
        return TL_INVALID;
    row->installed = json->text[json->tokens[installed].start] == 't';
    status = string_field(json, token, "name", model->scratch, SOFTWARE_NAME_BYTES, false);
    if (status == TL_OK)
        status = single_line(model->scratch, row->name, sizeof(row->name));
    if (status == TL_OK)
        status =
            string_field(json, token, "summary", model->scratch, SOFTWARE_SUMMARY_BYTES, false);
    if (status == TL_OK)
        status = single_line(model->scratch, row->summary, sizeof(row->summary));
    if (row->name[0] == 0)
        return TL_INVALID;
    int size = snprintf(row->display, sizeof(row->display), "%s • %s • %s/%s%s",
                        row->software.remote, row->software.installation, row->software.arch,
                        row->software.branch, row->installed ? " • Installed" : "");
    if (size < 0 || (size_t)size >= sizeof(row->display))
        return TL_LIMIT;
    memcpy(row->icon, "system-software-install-symbolic",
           sizeof("system-software-install-symbolic"));
    return status;
}
static tl_status decode_row(tl_popup_model *model, const tl_json *json, size_t token,
                            tl_popup_row *row) {
    *row = (tl_popup_row){0};
    char action_kind[32];
    tl_status action_status =
        string_field(json, token, "kind", action_kind, sizeof(action_kind), true);
    if (action_status != TL_OK)
        return action_status;
    if (strcmp(action_kind, "web") == 0 || strcmp(action_kind, "software") == 0)
        return decode_action(model, json, token, row, action_kind);
    size_t id = json_member(json, token, "id");
    if (id == SIZE_MAX || json->tokens[id].type != JSON_STRING ||
        json_uint(json, id, &row->id) != TL_OK || row->id == 0 || row->id > INT64_MAX)
        return TL_INVALID;
    size_t path = json_member(json, token, "path"), base64 = json_member(json, token, "path_b64");
    if ((path == SIZE_MAX) == (base64 == SIZE_MAX))
        return TL_INVALID;
    tl_status status =
        json_string(json, path == SIZE_MAX ? base64 : path, model->scratch, sizeof(model->scratch));
    if (status == TL_OK && path == SIZE_MAX)
        status = json_unbase64(model->scratch, row->path, sizeof(row->path));
    else if (status == TL_OK) {
        if (strlen(model->scratch) >= sizeof(row->path))
            return TL_LIMIT;
        memcpy(row->path, model->scratch, strlen(model->scratch) + 1);
    }
    if (status != TL_OK || row->path[0] != '/')
        return status == TL_OK ? TL_INVALID : status;
    status = string_field(json, token, "display", model->scratch, sizeof(model->scratch), false);
    if (status == TL_OK)
        status = single_line(model->scratch, row->display, sizeof(row->display));
    char kind[32] = {0};
    if (status == TL_OK)
        status = string_field(json, token, "kind", kind, sizeof(kind), true);
    row->application = strcmp(kind, "application") == 0 || strcmp(kind, "settings") == 0;
    row->settings = strcmp(kind, "settings") == 0;
    row->folder = strcmp(kind, "folder") == 0;
    if (status == TL_OK && kind[0] != 0 && !row->application && !row->folder &&
        strcmp(kind, "file") != 0)
        return TL_INVALID;
    if (status == TL_OK && row->application) {
        size_t revision = json_member(json, token, "desktop_revision");
        if (revision == SIZE_MAX || json->tokens[revision].type != JSON_STRING ||
            json_uint(json, revision, &row->desktop_revision) != TL_OK)
            return TL_INVALID;
        status = string_field(json, token, "name", model->scratch, sizeof(model->scratch), false);
        if (status == TL_OK)
            status = single_line(model->scratch, row->name, sizeof(row->name));
        if (status == TL_OK)
            status = string_field(json, token, "desktop_id", row->desktop_id,
                                  sizeof(row->desktop_id), false);
        if (status == TL_OK)
            status = string_field(json, token, "icon", row->icon, sizeof(row->icon), true);
        if (status == TL_OK)
            status =
                string_field(json, token, "wm_class", row->wm_class, sizeof(row->wm_class), true);
    } else if (status == TL_OK) {
        const char *name = strrchr(row->display, '/');
        name = name == NULL || name[1] == 0 ? row->display : name + 1;
        if (strlen(name) >= sizeof(row->name))
            return TL_LIMIT;
        memcpy(row->name, name, strlen(name) + 1);
    }
    return status;
}
/* Give each window to the displayed application with the strongest evidence;
 * ties go to the higher-ranked row. */
static void assign_windows(tl_popup_model *model) {
    size_t count = windows_count(model->windows);
    for (size_t w = 0; w < count; w++) {
        const tl_window *window = windows_get(model->windows, w);
        tl_windows_evidence best = WINDOWS_EVIDENCE_NONE;
        model->owners[w] = POPUP_NO_ROW;
        for (size_t r = 0; r < model->count; r++) {
            const tl_popup_row *row = &model->rows[r];
            if (!row->application || row->settings)
                continue;
            tl_windows_evidence evidence = windows_evidence(window, row->desktop_id, row->wm_class);
            if (evidence > best) {
                best = evidence;
                model->owners[w] = r;
            }
        }
    }
}
/* Index of id's choice, or choice_count when it has none. */
static size_t expansion_index(const tl_popup_model *model, uint64_t id) {
    size_t index = 0;
    while (index < model->choice_count && model->choices[index].id != id)
        index++;
    return index;
}
/* Remember a choice for this request; false when no slot is left for a new id. */
static bool set_expansion(tl_popup_model *model, uint64_t id, expansion_state state) {
    size_t index = expansion_index(model, id);
    if (index == POPUP_CHOICES)
        return false;
    if (index == model->choice_count)
        model->choice_count++;
    model->choices[index] = (struct expansion){id, state};
    return true;
}
static void append_item(tl_popup_model *model, tl_popup_item item) {
    /* POPUP_ITEMS covers every row, each window once and two children per row. */
    if (model->item_count < POPUP_ITEMS)
        model->items[model->item_count++] = item;
}
/* An expanded row's windows (most recent first), then more windows, then new window. */
static void append_children(tl_popup_model *model, size_t row, size_t windows, bool all) {
    size_t shown = 0, count = windows_count(model->windows);
    for (size_t w = 0; w < count && (all || shown < POPUP_WINDOWS_SHOWN); w++) {
        if (model->owners[w] != row)
            continue;
        append_item(model, (tl_popup_item){.kind = POPUP_ITEM_WINDOW,
                                           .row = row,
                                           .window = w,
                                           .handle = windows_get(model->windows, w)->handle});
        shown++;
    }
    if (shown < windows)
        append_item(model, (tl_popup_item){.kind = POPUP_ITEM_MORE_WINDOWS,
                                           .row = row,
                                           .windows = windows - shown});
    append_item(model, (tl_popup_item){.kind = POPUP_ITEM_NEW_WINDOW, .row = row});
}
/* Row r's item, then its children when its application lists its windows. */
static void append_result(tl_popup_model *model, size_t r) {
    size_t windows = 0, first = 0;
    for (size_t w = windows_count(model->windows); w-- > 0;)
        if (model->owners[w] == r) {
            windows++;
            first = w;
        }
    /* Every application lists its windows, so a running one is visible at
     * any rank; a collapse choice hides them for this request. */
    size_t choice = expansion_index(model, model->rows[r].id);
    expansion_state state =
        choice < model->choice_count ? model->choices[choice].state : EXPANSION_SHOWN;
    bool expanded = windows != 0 && state != EXPANSION_COLLAPSED;
    append_item(model, (tl_popup_item){.kind = POPUP_ITEM_RESULT,
                                       .row = r,
                                       .window = first,
                                       .windows = windows,
                                       .expanded = expanded});
    if (expanded)
        append_children(model, r, windows, state == EXPANSION_ALL);
}
/* Groups in the order of their best row, so the strongest result stays first. */
static size_t group_order(const tl_popup_model *model, tl_popup_group order[POPUP_GROUPS]) {
    size_t count = 0;
    for (size_t r = 0; r < model->count; r++)
        if (model->rows[r].target_kind == POPUP_TARGET_WEB) {
            order[count++] = POPUP_GROUP_WEB;
            break;
        }
    if (model->prefer_software)
        for (size_t r = 0; r < model->count; r++)
            if (model->rows[r].target_kind == POPUP_TARGET_SOFTWARE) {
                order[count++] = POPUP_GROUP_SOFTWARE;
                break;
            }
    for (size_t r = 0; r < model->count; r++) {
        tl_popup_group group = popup_row_group(&model->rows[r]);
        bool seen = false;
        for (size_t i = 0; i < count; i++)
            seen = seen || order[i] == group;
        if (!seen)
            order[count++] = group;
    }
    return count;
}
static void build_items(tl_popup_model *model) {
    assign_windows(model);
    model->item_count = 0;
    tl_popup_group order[POPUP_GROUPS];
    size_t groups = group_order(model, order);
    for (size_t g = 0; g < groups; g++)
        for (size_t r = 0; r < model->count; r++)
            if (popup_row_group(&model->rows[r]) == order[g])
                append_result(model, r);
}
static struct item_key item_key(const tl_popup_model *model, size_t index) {
    const tl_popup_item *item = &model->items[index];
    return (struct item_key){model->rows[item->row], item->handle, item->kind};
}
/* Index of the item with key, else its result's item, else the first item. */
static size_t find_item(const tl_popup_model *model, const struct item_key *key) {
    size_t fallback = 0;
    for (size_t i = 0; i < model->item_count; i++) {
        const tl_popup_item *item = &model->items[i];
        if (!popup_row_same(&model->rows[item->row], &key->row))
            continue;
        if (item->kind == key->kind && item->handle == key->handle)
            return i;
        if (item->kind == POPUP_ITEM_RESULT)
            fallback = i;
    }
    return fallback;
}
/* Rebuild items after the windows or a choice changed, keeping the selected item. */
static void relayout(tl_popup_model *model) {
    if (model->item_count == 0) {
        build_items(model);
        model->position = 0;
        return;
    }
    struct item_key key = item_key(model, model->position);
    build_items(model);
    model->position = find_item(model, &key);
}
tl_status popup_model_create(tl_popup_model **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = calloc(1, sizeof(**out));
    if (*out == NULL)
        return TL_NOMEM;
    (*out)->search_us = POPUP_SEARCH_TIME_UNKNOWN;
    return TL_OK;
}
tl_status popup_model_set_web_options(tl_popup_model *model, const tl_web_options *options,
                                      bool allow_visit) {
    if (model == NULL || options == NULL ||
        memchr(options->browser, 0, sizeof(options->browser)) == NULL ||
        memchr(options->browser_name, 0, sizeof(options->browser_name)) == NULL ||
        !json_utf8(options->browser_name) || !search_actions_browser(options->browser) ||
        (options->override_engine && search_actions_engine(options->engine) == NULL))
        return TL_INVALID;
    model->web_options = *options;
    model->allow_visit = allow_visit;
    return TL_OK;
}
static size_t initial_position(const tl_popup_model *model) {
    if (model->item_count < 2)
        return 0;
    const tl_popup_row *first = &model->rows[model->items[0].row];
    return first->target_kind == POPUP_TARGET_WEB && first->web_uri[0] == 0 ? 1 : 0;
}
void popup_model_destroy(tl_popup_model *model) {
    free(model);
}
void popup_model_clear(tl_popup_model *model) {
    if (model == NULL)
        return;
    model->request_id[0] = 0;
    model->search_id[0] = 0;
    model->reason[0] = 0;
    model->count = 0;
    model->item_count = 0;
    model->choice_count = 0;
    model->position = 0;
    model->ready = false;
    model->deliberate = false;
    model->indexing = false;
    model->degraded = false;
    model->search_us = POPUP_SEARCH_TIME_UNKNOWN;
}
tl_status popup_model_begin(tl_popup_model *model, const char *id) {
    if (model == NULL || id == NULL || id[0] == 0)
        return TL_INVALID;
    if (strlen(id) >= sizeof(model->request_id))
        return TL_LIMIT;
    memcpy(model->request_id, id, strlen(id) + 1);
    model->ready = false;
    model->deliberate = false;
    model->choice_count = 0;
    build_items(model);
    model->position = 0;
    return TL_OK;
}
static tl_status decode_suggestions(tl_popup_model *model, const tl_json *json, tl_popup_row *rows,
                                    size_t capacity, size_t *count) {
    size_t array = json_member(json, 0, "software_suggestions");
    size_t preference = json_member(json, 0, "prefer_software");
    if (preference != SIZE_MAX && json->tokens[preference].type != JSON_BOOL)
        return TL_INVALID;
    if (array == SIZE_MAX)
        return TL_OK;
    if (json->tokens[array].type != JSON_ARRAY)
        return TL_INVALID;
    size_t found = 0;
    for (size_t i = array + 1; i < json->tokens[array].next; i = json->tokens[i].next) {
        if (found++ == IPC_SOFTWARE_SUGGESTIONS || *count == capacity)
            return TL_LIMIT;
        tl_status status = decode_row(model, json, i, &rows[*count]);
        if (status != TL_OK)
            return status;
        if (rows[*count].target_kind != POPUP_TARGET_SOFTWARE)
            return TL_INVALID;
        for (size_t j = 0; j < *count; j++)
            if (popup_row_same(&rows[j], &rows[*count]))
                return TL_INVALID;
        (*count)++;
    }
    return TL_OK;
}
static bool boolean_field(const tl_json *json, size_t object, const char *key) {
    size_t token = json_member(json, object, key);
    return token != SIZE_MAX && json->tokens[token].type == JSON_BOOL &&
           json->text[json->tokens[token].start] == 't';
}
tl_status popup_model_decode(tl_popup_model *model, const char *response, size_t length,
                             tl_popup_row *rows, size_t capacity, size_t *count,
                             char search_id[IPC_HISTORY_ID_BYTES + 1]) {
    if (model == NULL || rows == NULL || count == NULL || search_id == NULL || capacity == 0)
        return TL_INVALID;
    *count = 0;
    search_id[0] = 0;
    tl_json json;
    tl_status status = json_parse(response, length, model->tokens, POPUP_JSON_TOKENS, &json);
    if (status != TL_OK)
        return status;
    uint64_t version = 0;
    char state[32], phase[16];
    if (json_uint(&json, json_member(&json, 0, "version"), &version) != TL_OK ||
        (version != IPC_VERSION && version != IPC_ACTION_VERSION) ||
        string_field(&json, 0, "status", state, sizeof(state), false) != TL_OK ||
        string_field(&json, 0, "phase", phase, sizeof(phase), false) != TL_OK ||
        (strcmp(phase, "final") != 0 && strcmp(phase, "lexical") != 0))
        return TL_INVALID;
    if (strcmp(state, "ok") != 0) {
        char reason[64];
        if (string_field(&json, 0, "reason", reason, sizeof(reason), false) != TL_OK)
            return TL_INVALID;
        return strcmp(reason, "stale_result") == 0 ? TL_STATE : TL_IO;
    }
    size_t search = json_member(&json, 0, "search_id");
    if (search != SIZE_MAX && json.tokens[search].type != JSON_NULL) {
        status = json_string(&json, search, search_id, IPC_HISTORY_ID_BYTES + 1);
        if (status != TL_OK)
            return status;
    }
    size_t results = json_member(&json, 0, "results");
    if (results == SIZE_MAX || json.tokens[results].type != JSON_ARRAY)
        return TL_INVALID;
    size_t found = 0;
    for (size_t i = results + 1; i < json.tokens[results].next; i = json.tokens[i].next) {
        if (found == capacity || found == POPUP_RESULTS)
            return TL_LIMIT;
        status = decode_row(model, &json, i, &rows[found]);
        if (status != TL_OK)
            return status;
        if (rows[found].target_kind == POPUP_TARGET_WEB ||
            (version == IPC_VERSION && rows[found].target_kind != POPUP_TARGET_LOCAL))
            return TL_INVALID;
        for (size_t j = 0; j < found; j++)
            if (popup_row_same(&rows[j], &rows[found]))
                return TL_INVALID;
        found++;
    }
    if (version == IPC_ACTION_VERSION) {
        size_t actions = json_member(&json, 0, "actions");
        if (actions == SIZE_MAX || json.tokens[actions].type != JSON_ARRAY)
            return TL_INVALID;
        size_t action_count = 0;
        for (size_t i = actions + 1; i < json.tokens[actions].next; i = json.tokens[i].next) {
            if (action_count++ == POPUP_ACTIONS || found == capacity)
                return TL_LIMIT;
            status = decode_row(model, &json, i, &rows[found]);
            if (status != TL_OK)
                return status;
            if (rows[found].target_kind != POPUP_TARGET_WEB)
                return TL_INVALID;
            found++;
        }
        status = decode_suggestions(model, &json, rows, capacity, &found);
        if (status != TL_OK)
            return status;
    }
    *count = found;
    return TL_OK;
}
/* Take count incoming rows. A deliberate selection keeps its row at the same
 * position (even outside the new top-k) and stays on the same item. */
static void replace_rows(tl_popup_model *model, size_t count) {
    tl_popup_row selected = {0};
    struct item_key key = {0};
    size_t row = 0;
    bool retain = model->deliberate && model->ready && model->item_count != 0;
    if (retain) {
        key = item_key(model, model->position);
        row = model->items[model->position].row;
        selected = model->rows[row];
    }
    model->count = 0;
    for (size_t i = 0; i < count; i++)
        if (!retain || !popup_row_same(&model->incoming[i], &selected))
            model->rows[model->count++] = model->incoming[i];
    if (retain) {
        if (row > model->count)
            row = model->count;
        for (size_t i = model->count; i > row; i--)
            model->rows[i] = model->rows[i - 1];
        model->rows[row] = selected;
        model->count++;
    }
    build_items(model);
    model->position = retain ? find_item(model, &key) : initial_position(model);
}
tl_status popup_model_apply(tl_popup_model *model, const char *response, size_t length) {
    if (model == NULL)
        return TL_INVALID;
    tl_json json;
    tl_status status = json_parse(response, length, model->tokens, POPUP_JSON_TOKENS, &json);
    char id[IPC_REQUEST_ID_BYTES + 1];
    if (status != TL_OK || string_field(&json, 0, "request_id", id, sizeof(id), false) != TL_OK)
        return TL_INVALID;
    if (strcmp(id, model->request_id) != 0)
        return TL_STATE;
    size_t count = 0;
    char search_id[IPC_HISTORY_ID_BYTES + 1];
    status =
        popup_model_decode(model, response, length, model->incoming, POPUP_ROWS, &count, search_id);
    if (status != TL_OK)
        return status;
    /* decode reused token scratch; parse again for supplementary index state. */
    status = json_parse(response, length, model->tokens, POPUP_JSON_TOKENS, &json);
    if (status != TL_OK)
        return status;
    model->prefer_software = boolean_field(&json, 0, "prefer_software");
    replace_rows(model, count);
    memcpy(model->search_id, search_id, strlen(search_id) + 1);
    status = string_field(&json, 0, "reason", model->reason, sizeof(model->reason), true);
    if (status != TL_OK)
        return status;
    size_t indexing = json_member(&json, 0, "indexing");
    model->indexing = boolean_field(&json, indexing, "active");
    model->degraded = boolean_field(&json, indexing, "degraded") ||
                      boolean_field(&json, indexing, "watch_degraded");
    size_t timing = json_member(&json, 0, "timing");
    uint64_t search_us = 0;
    model->search_us =
        json_uint(&json, json_member(&json, timing, "engine_us"), &search_us) == TL_OK
            ? search_us
            : POPUP_SEARCH_TIME_UNKNOWN;
    model->ready = true;
    return TL_OK;
}
void popup_model_move(tl_popup_model *model, int delta) {
    if (model == NULL || model->item_count == 0 || !model->ready)
        return;
    if (delta < 0 && model->position != 0)
        model->position--;
    if (delta > 0 && model->position + 1 < model->item_count)
        model->position++;
    model->deliberate = true;
}
const tl_popup_item *popup_model_selected_item(const tl_popup_model *model) {
    return model != NULL && model->ready && model->item_count != 0 ? &model->items[model->position]
                                                                   : NULL;
}
const tl_popup_row *popup_model_selected(const tl_popup_model *model) {
    const tl_popup_item *item = popup_model_selected_item(model);
    return item == NULL ? NULL : &model->rows[item->row];
}
void popup_model_set_windows(tl_popup_model *model, const tl_windows *windows) {
    if (model == NULL)
        return;
    model->windows = windows;
    relayout(model);
}
size_t popup_model_item_count(const tl_popup_model *model) {
    return model == NULL ? 0 : model->item_count;
}
const tl_popup_item *popup_model_item(const tl_popup_model *model, size_t index) {
    return model != NULL && index < model->item_count ? &model->items[index] : NULL;
}
bool popup_model_expand(tl_popup_model *model) {
    const tl_popup_item *item = popup_model_selected_item(model);
    if (item == NULL)
        return false;
    uint64_t id = model->rows[item->row].id;
    if (item->kind == POPUP_ITEM_MORE_WINDOWS) {
        if (!set_expansion(model, id, EXPANSION_ALL))
            return false;
        /* The first window the item revealed takes its place in the list. */
        size_t position = model->position;
        build_items(model);
        model->position = position < model->item_count ? position : model->item_count - 1;
        model->deliberate = true;
        return true;
    }
    if (item->kind != POPUP_ITEM_RESULT || item->windows == 0 || item->expanded ||
        !set_expansion(model, id, EXPANSION_SHOWN))
        return false;
    relayout(model);
    model->deliberate = true;
    return true;
}
bool popup_model_collapse(tl_popup_model *model) {
    const tl_popup_item *item = popup_model_selected_item(model);
    if (item == NULL || (item->kind == POPUP_ITEM_RESULT && !item->expanded))
        return false;
    struct item_key key = {model->rows[item->row], 0, POPUP_ITEM_RESULT};
    if (!set_expansion(model, key.row.id, EXPANSION_COLLAPSED))
        return false;
    build_items(model);
    model->position = find_item(model, &key);
    model->deliberate = true;
    return true;
}
tl_popup_group popup_row_group(const tl_popup_row *row) {
    return row->target_kind == POPUP_TARGET_WEB        ? POPUP_GROUP_WEB
           : row->target_kind == POPUP_TARGET_SOFTWARE ? POPUP_GROUP_SOFTWARE
           : row->settings                             ? POPUP_GROUP_SETTINGS
           : row->application                          ? POPUP_GROUP_APPS
           : row->folder                               ? POPUP_GROUP_FOLDERS
                                                       : POPUP_GROUP_FILES;
}
size_t popup_model_group_count(const tl_popup_model *model, tl_popup_group group) {
    size_t count = 0;
    for (size_t r = 0; model != NULL && r < model->count; r++)
        count += popup_row_group(&model->rows[r]) == group;
    return count;
}
const tl_popup_row *popup_model_row(const tl_popup_model *model, size_t index) {
    return model != NULL && index < model->count ? &model->rows[index] : NULL;
}
static size_t parent_length(const char *display) {
    const char *slash = strrchr(display, '/');
    return slash == NULL ? 0 : (size_t)(slash - display);
}
/* End offset in a of the first folder that differs from b; 0 when the parents match. */
static size_t first_difference(const char *a, size_t a_length, const char *b, size_t b_length) {
    size_t same = 0;
    while (same < a_length && same < b_length && a[same] == b[same])
        same++;
    if (same == a_length)
        return same == b_length ? 0 : a_length;
    /* a's folder ended where b's name continued ("y" against "yy"): that folder differs. */
    if (same < b_length && a[same] == '/')
        return same;
    /* b ended at a boundary, so a's next folder is the first one b lacks. */
    size_t from = same == b_length && a[same] == '/' ? same + 1 : same;
    const char *slash = memchr(a + from, '/', a_length - from);
    return slash == NULL ? a_length : (size_t)(slash - a);
}
/* The last POPUP_SHORT_FOLDERS folders of a parent of length bytes: what its
 * row shows. Sets tail_length; the whole parent when it has fewer folders. */
static const char *short_tail(const char *display, size_t length, size_t *tail_length) {
    size_t folders = 0, start = 0;
    for (size_t i = length; i-- > 0;)
        if (display[i] == '/' && ++folders == POPUP_SHORT_FOLDERS) {
            start = i + 1;
            break;
        }
    *tail_length = length - start;
    return display + start;
}
size_t popup_model_distinct_prefix(const tl_popup_model *model, size_t index) {
    const tl_popup_row *row = popup_model_row(model, index);
    if (row == NULL || row->application || row->target_kind != POPUP_TARGET_LOCAL)
        return 0;
    size_t length = parent_length(row->display), keep = 0;
    for (size_t i = 0; i < model->count; i++) {
        const tl_popup_row *other = &model->rows[i];
        /* Icons already tell folders from files, so only same-kind names can collide. */
        if (i == index || other->target_kind != POPUP_TARGET_LOCAL || other->application ||
            other->folder != row->folder || strcmp(other->name, row->name) != 0)
            continue;
        size_t other_length = parent_length(other->display), tail_length, other_tail_length;
        const char *tail = short_tail(row->display, length, &tail_length);
        const char *other_tail = short_tail(other->display, other_length, &other_tail_length);
        /* Rows whose shown folders already differ need nothing earlier. */
        if (tail_length != other_tail_length || memcmp(tail, other_tail, tail_length) != 0)
            continue;
        size_t end = first_difference(row->display, length, other->display, other_length);
        keep = end > keep ? end : keep;
    }
    return keep;
}
size_t popup_model_count(const tl_popup_model *model) {
    return model == NULL ? 0 : model->count;
}
size_t popup_model_position(const tl_popup_model *model) {
    return model == NULL ? 0 : model->position;
}
bool popup_model_indexing(const tl_popup_model *model) {
    return model != NULL && model->indexing;
}
bool popup_model_degraded(const tl_popup_model *model) {
    return model != NULL && model->degraded;
}
const char *popup_model_search_id(const tl_popup_model *model) {
    return model == NULL ? "" : model->search_id;
}
uint64_t popup_model_search_us(const tl_popup_model *model) {
    return model == NULL ? POPUP_SEARCH_TIME_UNKNOWN : model->search_us;
}

const char *popup_model_hint(const tl_popup_model *model) {
    if (model == NULL)
        return "";
    if (strcmp(model->reason, "web_disabled") == 0)
        return "Web search is disabled in Torchlight’s configuration.";
    if (strcmp(model->reason, "loading") == 0)
        return "Loading available apps… Retry shortly.";
    if (strcmp(model->reason, "unavailable") == 0)
        return "Software search needs Flatpak and an enabled remote.";
    if (strcmp(model->reason, "disabled") == 0)
        return "Software search is disabled in Torchlight’s configuration.";
    if (strcmp(model->reason, "failed") == 0)
        return "Could not refresh available apps. Retry to try again.";
    if (strcmp(model->reason, "stale") == 0)
        return "Showing cached apps. Refresh failed; Retry to try again.";
    return "";
}
