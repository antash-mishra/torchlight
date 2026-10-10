/* Strict copied IPC results, open windows beneath applications and stable
 * deliberate selection across response phases. */
#include "torchlight/popup.h"
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
    uint64_t id, handle;
    tl_popup_item_kind kind;
};
struct tl_popup_model {
    char request_id[IPC_REQUEST_ID_BYTES + 1], search_id[IPC_HISTORY_ID_BYTES + 1];
    tl_popup_row rows[POPUP_RESULTS + 1], incoming[POPUP_RESULTS];
    tl_popup_item items[POPUP_ITEMS];
    /* position indexes items; count is the number of rows. */
    size_t count, item_count, position;
    const tl_windows *windows;
    /* Row that owns each snapshot window, or POPUP_NO_ROW. */
    size_t owners[WINDOWS_MAX];
    struct expansion choices[POPUP_CHOICES];
    size_t choice_count;
    bool deliberate, ready, indexing, degraded;
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
static tl_status decode_row(tl_popup_model *model, const tl_json *json, size_t token,
                            tl_popup_row *row) {
    *row = (tl_popup_row){0};
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
static void build_items(tl_popup_model *model) {
    assign_windows(model);
    model->item_count = 0;
    size_t count = windows_count(model->windows);
    for (size_t r = 0; r < model->count; r++) {
        size_t windows = 0, first = 0;
        for (size_t w = count; w-- > 0;)
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
}
static struct item_key item_key(const tl_popup_model *model, size_t index) {
    const tl_popup_item *item = &model->items[index];
    return (struct item_key){model->rows[item->row].id, item->handle, item->kind};
}
/* Index of the item with key, else its result's item, else the first item. */
static size_t find_item(const tl_popup_model *model, const struct item_key *key) {
    size_t fallback = 0;
    for (size_t i = 0; i < model->item_count; i++) {
        const tl_popup_item *item = &model->items[i];
        if (model->rows[item->row].id != key->id)
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
    return *out == NULL ? TL_NOMEM : TL_OK;
}
void popup_model_destroy(tl_popup_model *model) {
    free(model);
}
void popup_model_clear(tl_popup_model *model) {
    if (model == NULL)
        return;
    model->request_id[0] = 0;
    model->search_id[0] = 0;
    model->count = 0;
    model->item_count = 0;
    model->choice_count = 0;
    model->position = 0;
    model->ready = false;
    model->deliberate = false;
    model->indexing = false;
    model->degraded = false;
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
        version != IPC_VERSION ||
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
        if (found == capacity)
            return TL_LIMIT;
        status = decode_row(model, &json, i, &rows[found]);
        if (status != TL_OK)
            return status;
        for (size_t j = 0; j < found; j++)
            if (rows[j].id == rows[found].id)
                return TL_INVALID;
        found++;
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
        if (!retain || model->incoming[i].id != selected.id)
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
    model->position = retain ? find_item(model, &key) : 0;
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
    status = popup_model_decode(model, response, length, model->incoming, POPUP_RESULTS, &count,
                                search_id);
    if (status != TL_OK)
        return status;
    replace_rows(model, count);
    memcpy(model->search_id, search_id, strlen(search_id) + 1);
    /* decode reused token scratch; parse again for supplementary index state. */
    status = json_parse(response, length, model->tokens, POPUP_JSON_TOKENS, &json);
    if (status != TL_OK)
        return status;
    size_t indexing = json_member(&json, 0, "indexing");
    model->indexing = boolean_field(&json, indexing, "active");
    model->degraded = boolean_field(&json, indexing, "degraded") ||
                      boolean_field(&json, indexing, "watch_degraded");
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
    struct item_key key = {model->rows[item->row].id, 0, POPUP_ITEM_RESULT};
    if (!set_expansion(model, key.id, EXPANSION_COLLAPSED))
        return false;
    build_items(model);
    model->position = find_item(model, &key);
    model->deliberate = true;
    return true;
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
    if (row == NULL || row->application)
        return 0;
    size_t length = parent_length(row->display), keep = 0;
    for (size_t i = 0; i < model->count; i++) {
        const tl_popup_row *other = &model->rows[i];
        /* Icons already tell folders from files, so only same-kind names can collide. */
        if (i == index || other->application || other->folder != row->folder ||
            strcmp(other->name, row->name) != 0)
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
