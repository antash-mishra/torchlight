/* Socket CLI client: validate terminal envelopes and never act on display text. */
#include "torchlight/client.h"
#include "torchlight/tokenize.h"
#include <stdlib.h>
#include <string.h>
#define CLIENT_JSON_TOKENS 16384
static tl_status envelope(const tl_json *json, const tl_ipc_request *request, bool *success) {
    uint64_t version = 0;
    char id[IPC_REQUEST_ID_BYTES + 1], phase[16], status[32];
    if (json_uint(json, json_member(json, 0, "version"), &version) != TL_OK ||
        version != IPC_VERSION)
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
    if (request == NULL || output == NULL || (json_output && null_output))
        return TL_INVALID;
    char *response = malloc(IPC_RESPONSE_BYTES), *scratch = malloc(IPC_RESPONSE_BYTES),
         *decoded = malloc(IPC_RESPONSE_BYTES);
    tl_json_token *tokens = calloc(CLIENT_JSON_TOKENS, sizeof(*tokens));
    tl_status status =
        response == NULL || scratch == NULL || decoded == NULL || tokens == NULL ? TL_NOMEM : TL_OK;
    size_t length = 0;
    tl_json json;
    bool success = false;
    if (status == TL_OK)
        status = ipc_call(socket_path, request, response, IPC_RESPONSE_BYTES, &length);
    if (status == TL_OK)
        status = json_parse(response, length, tokens, CLIENT_JSON_TOKENS, &json);
    if (status == TL_OK)
        status = envelope(&json, request, &success);
    if (status == TL_OK) {
        bool paths = request->operation == IPC_QUERY || request->operation == IPC_RESOLVE;
        if (json_output || !paths) {
            if (fwrite(response, 1, length, output) != length)
                status = TL_IO;
        } else if (success)
            status = print_results(&json, null_output, output, scratch, decoded);
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
