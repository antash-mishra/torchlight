/* Typed non-filesystem protocol targets and explicit capability negotiation. */
#include "torchlight/ipc.h"
#include <string.h>
unsigned ipc_version(const tl_ipc_request *request) {
    return request->version == 0 ? IPC_VERSION : request->version;
}
static void string_field(tl_json_buffer *buffer, const char *key, const char *value) {
    json_raw(buffer, ",");
    json_quote(buffer, key);
    json_raw(buffer, ":");
    json_quote(buffer, value);
}
void ipc_software_target(tl_json_buffer *buffer, const tl_software_ref *ref, uint64_t revision,
                         uint64_t source_revision) {
    json_raw(buffer, "{\"kind\":\"software\",\"backend\":\"flatpak\"");
    string_field(buffer, "installation", ref->installation);
    string_field(buffer, "remote", ref->remote);
    string_field(buffer, "app_id", ref->app_id);
    string_field(buffer, "arch", ref->arch);
    string_field(buffer, "branch", ref->branch);
    json_raw(buffer, ",\"revision\":\"");
    json_number(buffer, revision);
    json_raw(buffer, "\",\"source_revision\":\"");
    json_number(buffer, source_revision);
    json_raw(buffer, "\"}");
}
tl_status ipc_software_decode(const tl_json *json, size_t token, tl_software_ref *ref,
                              uint64_t *revision, uint64_t *source_revision) {
    if (ref == NULL || revision == NULL || source_revision == NULL)
        return TL_INVALID;
    *ref = (tl_software_ref){0};
    *revision = 0;
    *source_revision = 0;
    if (json == NULL || token >= json->count || json->tokens[token].type != JSON_OBJECT)
        return TL_INVALID;
    tl_software_ref decoded = {0};
    char kind[16], backend[16];
    const char *keys[] = {"kind", "backend", "installation", "remote",         "app_id",
                          "arch", "branch",  "revision",     "source_revision"};
    char *fields[] = {kind,           backend,      decoded.installation, decoded.remote,
                      decoded.app_id, decoded.arch, decoded.branch};
    size_t capacities[] = {
        sizeof(kind),           sizeof(backend),        sizeof(decoded.installation),
        sizeof(decoded.remote), sizeof(decoded.app_id), sizeof(decoded.arch),
        sizeof(decoded.branch)};
    for (size_t i = token + 1; i < json->tokens[token].next; i = json->tokens[i + 1].next) {
        char key[JSON_KEY_BYTES + 1];
        if (json_string(json, i, key, sizeof(key)) != TL_OK)
            return TL_INVALID;
        bool known = false;
        for (size_t j = 0; j < sizeof(keys) / sizeof(keys[0]); j++)
            if (strcmp(key, keys[j]) == 0)
                known = true;
        if (!known)
            return TL_INVALID;
    }
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (json_string(json, json_member(json, token, keys[i]), fields[i], capacities[i]) != TL_OK)
            return TL_INVALID;
    if (strcmp(kind, "software") != 0 || strcmp(backend, "flatpak") != 0 ||
        !software_ref_valid(&decoded))
        return TL_INVALID;
    uint64_t values[2];
    for (size_t i = 0; i < 2; i++) {
        size_t number = json_member(json, token, keys[i + 7]);
        if (number == SIZE_MAX || json->tokens[number].type != JSON_STRING ||
            json_uint(json, number, &values[i]) != TL_OK)
            return TL_INVALID;
    }
    *ref = decoded;
    *revision = values[0];
    *source_revision = values[1];
    return TL_OK;
}
void ipc_software_result(tl_json_buffer *buffer, const tl_software_entry *entry) {
    json_raw(buffer, "{\"kind\":\"software\",\"target\":");
    ipc_software_target(buffer, &entry->ref, entry->revision, entry->source_revision);
    string_field(buffer, "name", entry->name);
    string_field(buffer, "summary", entry->summary);
    json_raw(buffer, ",\"installed\":");
    json_raw(buffer, entry->installed ? "true" : "false");
    json_raw(buffer, "}");
}
void ipc_suggestions_result(tl_json_buffer *buffer, const tl_ipc_request *request, bool include) {
    if (!request->software_suggestions)
        return;
    if (request->suggestions.count > IPC_SOFTWARE_SUGGESTIONS) {
        buffer->status = TL_INVALID;
        return;
    }
    json_raw(buffer, ",\"software_suggestions\":[");
    for (size_t i = 0; include && i < request->suggestions.count; i++) {
        if (i != 0)
            json_raw(buffer, ",");
        const tl_ipc_suggestion *copy = &request->suggestions.entries[i];
        tl_software_entry entry = {.ref = copy->ref,
                                   .revision = copy->revision,
                                   .source_revision = copy->source_revision,
                                   .name = copy->name,
                                   .summary = copy->summary};
        ipc_software_result(buffer, &entry);
    }
    json_raw(buffer, "],\"prefer_software\":");
    json_raw(buffer, include && request->suggestions.preferred ? "true" : "false");
}
tl_status ipc_action_capability(const char *response, size_t length, bool *supported) {
    if (supported == NULL)
        return TL_INVALID;
    *supported = false;
    tl_json_token tokens[512];
    tl_json json;
    tl_status status = json_parse(response, length, tokens, 512, &json);
    if (status != TL_OK)
        return status;
    char state[16], phase[16];
    uint64_t version = 0;
    if (json_uint(&json, json_member(&json, 0, "version"), &version) != TL_OK ||
        version != IPC_VERSION ||
        json_string(&json, json_member(&json, 0, "phase"), phase, sizeof(phase)) != TL_OK ||
        strcmp(phase, "final") != 0 ||
        json_string(&json, json_member(&json, 0, "status"), state, sizeof(state)) != TL_OK ||
        strcmp(state, "ok") != 0)
        return TL_INVALID;
    size_t capabilities = json_member(&json, 0, "capabilities");
    if (capabilities == SIZE_MAX)
        return TL_OK;
    if (tokens[capabilities].type != JSON_OBJECT)
        return TL_INVALID;
    size_t actions = json_member(&json, capabilities, "search_actions");
    if (actions == SIZE_MAX)
        return TL_OK;
    if (tokens[actions].type != JSON_NUMBER || json_uint(&json, actions, &version) != TL_OK)
        return TL_INVALID;
    *supported = version >= IPC_ACTION_VERSION;
    return TL_OK;
}
