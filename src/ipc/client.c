/* Socket CLI client: validate terminal envelopes and never act on display text. */
#include "torchlight/client.h"
#include "torchlight/tokenize.h"
#include <stdlib.h>
#include <string.h>
tl_status client_actions_supported(const char *socket_path, bool *supported) {
    if (supported == NULL)
        return TL_INVALID;
    *supported = false;
    char *response = malloc(IPC_RESPONSE_BYTES);
    if (response == NULL)
        return TL_NOMEM;
    tl_ipc_request request = {.operation = IPC_STATUS, .request_id = "capabilities"};
    size_t length = 0;
    tl_status status = ipc_call(socket_path, &request, response, IPC_RESPONSE_BYTES, &length);
    if (status == TL_OK)
        status = ipc_action_capability(response, length, supported);
    free(response);
    return status;
}
static tl_status print_actions(const tl_json *json, const tl_ipc_request *request, FILE *output) {
    const char *key = request->mode == SEARCH_WEB ? "actions" : "results";
    size_t array = json_member(json, 0, key);
    if (array == SIZE_MAX || json->tokens[array].type != JSON_ARRAY)
        return TL_INVALID;
    for (size_t i = array + 1; i < json->tokens[array].next; i = json->tokens[i].next) {
        if (request->mode == SEARCH_WEB) {
            char provider[32], query[LEXICAL_QUERY_BYTES + 1], uri[SEARCH_ACTION_URI_BYTES];
            tl_search_engine engine;
            if (json_string(json, json_member(json, i, "provider"), provider, sizeof(provider)) !=
                    TL_OK ||
                json_string(json, json_member(json, i, "query"), query, sizeof(query)) != TL_OK ||
                search_actions_engine_parse(provider, &engine) != TL_OK ||
                search_actions_uri(engine, query, uri, sizeof(uri)) != TL_OK)
                return TL_INVALID;
            if (fprintf(output, "%s\n", uri) < 0)
                return TL_IO;
        } else {
            tl_software_ref ref;
            uint64_t revision, source;
            size_t installed_field = json_member(json, i, "installed");
            char name[SOFTWARE_NAME_BYTES];
            if (ipc_software_decode(json, json_member(json, i, "target"), &ref, &revision,
                                    &source) != TL_OK ||
                json_string(json, json_member(json, i, "name"), name, sizeof(name)) != TL_OK ||
                installed_field == SIZE_MAX || json->tokens[installed_field].type != JSON_BOOL)
                return TL_INVALID;
            bool installed = json->text[json->tokens[installed_field].start] == 't';
            char *display = NULL;
            tl_status status = tokenize_display_create(name, &display);
            if (status != TL_OK)
                return status;
            int code =
                fprintf(output, "%s\t%s\t%s/%s\t%s/%s\t%s\n", ref.app_id, display, ref.installation,
                        ref.remote, ref.arch, ref.branch, installed ? "installed" : "available");
            tokenize_display_destroy(display);
            if (code < 0)
                return TL_IO;
        }
    }
    return TL_OK;
}
static tl_status envelope(const tl_json *json, const tl_ipc_request *request, bool *success) {
    uint64_t version = 0;
    char id[IPC_REQUEST_ID_BYTES + 1], phase[16], status[32];
    if (json_uint(json, json_member(json, 0, "version"), &version) != TL_OK ||
        version != ipc_version(request))
        return TL_INVALID;
    if (json_string(json, json_member(json, 0, "request_id"), id, sizeof(id)) != TL_OK ||
        strcmp(id, request->request_id) != 0)
        return TL_INVALID;
    if (json_string(json, json_member(json, 0, "phase"), phase, sizeof(phase)) != TL_OK ||
        strcmp(phase, "final") != 0)
        return TL_INVALID;
    if (json_string(json, json_member(json, 0, "status"), status, sizeof(status)) != TL_OK)
        return TL_INVALID;
    *success = strcmp(status, "ok") == 0;
    return TL_OK;
}
static tl_status result_path(const tl_json *json, size_t result, char *scratch, char *decoded) {
    size_t path = json_member(json, result, "path"), base64 = json_member(json, result, "path_b64");
    if ((path == SIZE_MAX) == (base64 == SIZE_MAX))
        return TL_INVALID;
    tl_status status =
        json_string(json, path == SIZE_MAX ? base64 : path, scratch, IPC_RESPONSE_BYTES);
    if (status != TL_OK)
        return status;
    if (path == SIZE_MAX)
        return json_unbase64(scratch, decoded, IPC_RESPONSE_BYTES);
    size_t length = strlen(scratch);
    memcpy(decoded, scratch, length + 1);
    return TL_OK;
}
static tl_status print_results(const tl_json *json, bool null_output, FILE *output, char *scratch,
                               char *decoded) {
    size_t results = json_member(json, 0, "results");
    if (results == SIZE_MAX || json->tokens[results].type != JSON_ARRAY)
        return TL_INVALID;
    for (size_t i = results + 1; i < json->tokens[results].next; i = json->tokens[i].next) {
        tl_status status = result_path(json, i, scratch, decoded);
        if (status != TL_OK)
            return status;
        if (null_output) {
            size_t length = strlen(decoded) + 1;
            if (fwrite(decoded, 1, length, output) != length)
                return TL_IO;
        } else {
            char *display = NULL;
            status = tokenize_display_create(decoded, &display);
            if (status != TL_OK)
                return status;
            int code = fprintf(output, "%s\n", display);
            tokenize_display_destroy(display);
            if (code < 0)
                return TL_IO;
        }
    }
    return TL_OK;
}
tl_status client_request(const char *socket_path, const tl_ipc_request *request, bool json_output,
                         bool null_output, FILE *output) {
    if (request == NULL || output == NULL || (json_output && null_output) ||
        (null_output && (request->mode != SEARCH_LOCAL || request->software_target ||
                         request->operation == IPC_SOFTWARE_REFRESH)))
        return TL_INVALID;
    char *response = malloc(IPC_RESPONSE_BYTES), *scratch = malloc(IPC_RESPONSE_BYTES),
         *decoded = malloc(IPC_RESPONSE_BYTES);
    tl_json_token *tokens = calloc(IPC_RESPONSE_TOKENS, sizeof(*tokens));
    tl_status status =
        response == NULL || scratch == NULL || decoded == NULL || tokens == NULL ? TL_NOMEM : TL_OK;
    size_t length = 0;
    tl_json json;
    bool success = false;
    if (status == TL_OK)
        status = ipc_call(socket_path, request, response, IPC_RESPONSE_BYTES, &length);
    if (status == TL_OK)
        status = json_parse(response, length, tokens, IPC_RESPONSE_TOKENS, &json);
    if (status == TL_OK)
        status = envelope(&json, request, &success);
    if (status == TL_OK) {
        bool paths = request->operation == IPC_QUERY || request->operation == IPC_RESOLVE;
        if (json_output || !paths) {
            if (fwrite(response, 1, length, output) != length)
                status = TL_IO;
        } else if (success) {
            if (request->mode != SEARCH_LOCAL || request->software_target)
                status = print_actions(&json, request, output);
            else
                status = print_results(&json, null_output, output, scratch, decoded);
        }
    }
    if (status == TL_OK && fflush(output) != 0)
        status = TL_IO;
    if (status == TL_OK && !success)
        status = TL_STATE;
    free(response);
    free(scratch);
    free(decoded);
    free(tokens);
    return status;
}
