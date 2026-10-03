/* Strict copied IPC results and stable deliberate selection across response phases. */
#include "torchlight/popup.h"
#include <stdlib.h>
#include <string.h>
#define POPUP_JSON_TOKENS 16384
struct tl_popup_model {
    char request_id[IPC_REQUEST_ID_BYTES + 1], search_id[IPC_HISTORY_ID_BYTES + 1];
    tl_popup_row rows[POPUP_RESULTS + 1], incoming[POPUP_RESULTS];
    size_t count, position;
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
    } else if (status == TL_OK) {
        const char *name = strrchr(row->display, '/');
        name = name == NULL || name[1] == 0 ? row->display : name + 1;
        if (strlen(name) >= sizeof(row->name))
            return TL_LIMIT;
        memcpy(row->name, name, strlen(name) + 1);
    }
    return status;
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
    tl_popup_row selected = {0};
    bool retain = model->deliberate && model->ready && model->count != 0;
    if (retain)
        selected = model->rows[model->position];
    model->count = 0;
    for (size_t i = 0; i < count; i++)
        if (!retain || model->incoming[i].id != selected.id)
            model->rows[model->count++] = model->incoming[i];
    if (retain) {
        if (model->position > model->count)
            model->position = model->count;
        for (size_t i = model->count; i > model->position; i--)
            model->rows[i] = model->rows[i - 1];
        model->rows[model->position] = selected;
        model->count++;
    } else
        model->position = 0;
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
    if (model == NULL || model->count == 0 || !model->ready)
        return;
    if (delta < 0 && model->position != 0)
        model->position--;
    if (delta > 0 && model->position + 1 < model->count)
        model->position++;
    model->deliberate = true;
}
const tl_popup_row *popup_model_selected(const tl_popup_model *model) {
    return model != NULL && model->ready && model->count != 0 ? &model->rows[model->position]
                                                              : NULL;
}
const tl_popup_row *popup_model_row(const tl_popup_model *model, size_t index) {
    return model != NULL && index < model->count ? &model->rows[index] : NULL;
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
