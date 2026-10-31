/* M9 parser, typed wire targets, source identity and bounded worker regressions. */
#include "test.h"
#include "torchlight/ipc.h"
#include "torchlight/popup.h"
#include "torchlight/software_catalog.h"
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
static void prefixes(void) {
    const struct {
        const char *text, *query;
        tl_search_mode mode;
        bool fallback;
    } cases[] = {{"web", "web", SEARCH_LOCAL, true},
                 {"installer", "installer", SEARCH_LOCAL, true},
                 {" web  cats & dogs  ", "cats & dogs", SEARCH_WEB, false},
                 {"WEB\t café  +? ", "café  +?", SEARCH_WEB, false},
                 {"install\nvLc", "vLc", SEARCH_SOFTWARE, false},
                 {"local web cat", "web cat", SEARCH_LOCAL, false},
                 {"web ", "", SEARCH_WEB, false},
                 {"  local  ", "", SEARCH_LOCAL, false},
                 {"web\xc2\xa0"
                  "cats",
                  "web\xc2\xa0"
                  "cats",
                  SEARCH_LOCAL, true},
                 {"notes", "notes", SEARCH_LOCAL, true}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        tl_search_input parsed;
        CHECK(search_actions_parse(cases[i].text, &parsed) == TL_OK);
        CHECK(parsed.mode == cases[i].mode && parsed.web_fallback == cases[i].fallback);
        CHECK(strcmp(parsed.query, cases[i].query) == 0);
    }
    char huge[LEXICAL_QUERY_BYTES + 2];
    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = 0;
    tl_search_input parsed;
    CHECK(search_actions_parse(huge, &parsed) == TL_LIMIT && parsed.query[0] == 0);
    CHECK(search_actions_parse("\xff", &parsed) == TL_INVALID);
    char uri[SEARCH_ACTION_URI_BYTES];
    CHECK(search_actions_uri(SEARCH_DUCKDUCKGO, "café &+?#\n", uri, sizeof(uri)) == TL_OK);
    CHECK(strcmp(uri, "https://duckduckgo.com/?q=caf%C3%A9%20%26%2B%3F%23%0A") == 0);
    CHECK(search_actions_uri(SEARCH_GOOGLE, "-- $(touch /tmp/no) ", uri, sizeof(uri)) == TL_OK);
    CHECK(strstr(uri, "%24%28touch%20%2Ftmp%2Fno%29") != NULL);
    CHECK(search_actions_uri(SEARCH_GOOGLE, "x", uri, 2) == TL_LIMIT && uri[0] == 0);
    CHECK(search_actions_uri(SEARCH_GOOGLE, "", uri, sizeof(uri)) == TL_INVALID);
}
static void website_addresses(void) {
    const struct {
        const char *input, *uri;
    } cases[] = {{"google.com", "https://google.com"},
                 {"example.org/path?q=one&two=2#part", "https://example.org/path?q=one&two=2#part"},
                 {"HTTPS://Example.com/café", "https://Example.com/caf%C3%A9"},
                 {"https://example.com/a%20b", "https://example.com/a%20b"},
                 {"http://localhost:8080/a", "http://localhost:8080/a"},
                 {"localhost:3000", "https://localhost:3000"},
                 {"127.0.0.1:8080", "https://127.0.0.1:8080"},
                 {"https://[::1]:8443/", "https://[::1]:8443/"},
                 {"https://example.museum", "https://example.museum"},
                 {"report.pdf", NULL},
                 {"main.rs", NULL},
                 {"script.sh", NULL},
                 {"archive.zip", NULL},
                 {"../google.com", NULL},
                 {"/home/google.com", NULL},
                 {"google.com notes", NULL},
                 {"javascript:alert(1)", NULL},
                 {"file:///etc/passwd", NULL},
                 {"mailto:a@example.com", NULL},
                 {"https://user@example.com", NULL},
                 {"https://example.com:65536", NULL},
                 {"https://example.com:0", NULL},
                 {"https://example.com:abc", NULL},
                 {"https://[invalid]/", NULL},
                 {"https://-example.com", NULL},
                 {"example..com", NULL},
                 {"https://example.com/\\evil", NULL},
                 {"https://example.com/%", NULL},
                 {"https://example.com/%0x", NULL},
                 {"google.com\n", NULL},
                 {"", NULL}};
    char uri[SEARCH_ACTION_URI_BYTES];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        tl_status status = search_actions_address(cases[i].input, uri, sizeof(uri));
        CHECK(status == (cases[i].uri == NULL ? TL_INVALID : TL_OK));
        CHECK(strcmp(uri, cases[i].uri == NULL ? "" : cases[i].uri) == 0);
    }
    CHECK(search_actions_address("google.com", uri, 2) == TL_LIMIT && uri[0] == 0);
    CHECK(search_actions_browser("google-chrome.desktop"));
    CHECK(search_actions_browser("com.brave.Browser.desktop"));
    CHECK(search_actions_browser("default") && search_actions_browser(""));
    CHECK(!search_actions_browser("../browser.desktop"));
    CHECK(!search_actions_browser("browser --incognito.desktop"));
    CHECK(!search_actions_browser(NULL));
}
static void website_selection(void) {
    const char *response =
        "{\"version\":2,\"request_id\":\"address\",\"phase\":\"final\","
        "\"status\":\"ok\",\"results\":[{\"id\":\"1\",\"path\":\"/google.txt\",\"display\":\"/"
        "google.txt\"}],"
        "\"actions\":[{\"kind\":\"web\",\"provider\":\"duckduckgo\",\"query\":\"google.com\"}]}";
    tl_popup_model *model = NULL;
    CHECK(popup_model_create(&model) == TL_OK);
    tl_web_options options = {.override_engine = true,
                              .engine = SEARCH_GOOGLE,
                              .browser = "google-chrome.desktop",
                              .browser_name = "Google Chrome"};
    CHECK(popup_model_set_web_options(model, &options, true) == TL_OK);
    CHECK(popup_model_begin(model, "address") == TL_OK);
    CHECK(popup_model_apply(model, response, strlen(response)) == TL_OK);
    const tl_popup_row *row = popup_model_selected(model);
    CHECK(row->target_kind == POPUP_TARGET_WEB && strcmp(row->web_uri, "https://google.com") == 0);
    CHECK(strcmp(row->name, "Open google.com") == 0 && row->web_engine == SEARCH_GOOGLE);
    CHECK(strcmp(row->web_browser, "google-chrome.desktop") == 0);
    popup_model_move(model, 1);
    CHECK(popup_model_selected(model)->target_kind == POPUP_TARGET_LOCAL);
    CHECK(popup_model_apply(model, response, strlen(response)) == TL_OK);
    CHECK(popup_model_selected(model)->target_kind == POPUP_TARGET_LOCAL);
    CHECK(popup_model_set_web_options(model, &options, false) == TL_OK);
    CHECK(popup_model_begin(model, "address") == TL_OK);
    CHECK(popup_model_apply(model, response, strlen(response)) == TL_OK);
    popup_model_move(model, -1);
    CHECK(popup_model_selected(model)->web_uri[0] == 0);
    popup_model_destroy(model);
}
static tl_software_entry app(const char *remote) {
    tl_software_entry entry = {.ref = {.installation = "user",
                                       .app_id = "org.example.Editor",
                                       .arch = "x86_64",
                                       .branch = "stable"},
                               .name = "Example Editor",
                               .summary = "Edit source code",
                               .source_revision = 10};
    CHECK(strlen(remote) < sizeof(entry.ref.remote));
    memcpy(entry.ref.remote, remote, strlen(remote) + 1);
    return entry;
}
static void wire(void) {
    tl_ipc_request request = {.version = IPC_ACTION_VERSION,
                              .operation = IPC_QUERY,
                              .mode = SEARCH_WEB,
                              .query = "cats & dogs",
                              .request_id = "new",
                              .limit = 10},
                   decoded;
    char text[IPC_REQUEST_BYTES];
    size_t length;
    CHECK(ipc_encode(&request, text, sizeof(text), &length) == TL_OK);
    CHECK(ipc_decode(text, length - 1, &decoded) == TL_OK && decoded.mode == SEARCH_WEB);
    request.version = IPC_VERSION;
    CHECK(ipc_encode(&request, text, sizeof(text), &length) == TL_INVALID);
    tl_software_entry entry = app("flathub");
    request = (tl_ipc_request){.version = IPC_ACTION_VERSION,
                               .operation = IPC_RESOLVE,
                               .request_id = "resolve",
                               .software_target = true,
                               .software = entry.ref,
                               .software_revision = UINT64_MAX,
                               .source_revision = UINT64_MAX - 1};
    CHECK(ipc_encode(&request, text, sizeof(text), &length) == TL_OK);
    CHECK(ipc_decode(text, length - 1, &decoded) == TL_OK && decoded.software_target);
    CHECK(software_ref_equal(&decoded.software, &request.software));
    CHECK(decoded.software_revision == UINT64_MAX && decoded.source_revision == UINT64_MAX - 1);
    memcpy(request.software.remote, "--install", 10);
    CHECK(ipc_encode(&request, text, sizeof(text), &length) == TL_INVALID);
    const char *bad[] = {"{\"version\":2,\"op\":\"query\",\"request_id\":\"x\",\"query\":\"hi\"}",
                         "{\"version\":2,\"op\":\"query\",\"request_id\":\"x\",\"query\":\"hi\","
                         "\"mode\":\"web\",\"web_fallback\":true}",
                         "{\"version\":1,\"op\":\"software_refresh\",\"request_id\":\"x\"}",
                         "{\"version\":1,\"op\":\"query\",\"request_id\":\"x\",\"query\":\"hi\","
                         "\"mode\":\"local\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(ipc_decode(bad[i], strlen(bad[i]), &decoded) == TL_INVALID);
    bool supported = true;
    const char *old = "{\"version\":1,\"phase\":\"final\",\"status\":\"ok\"}";
    const char *current = "{\"version\":1,\"phase\":\"final\",\"status\":\"ok\",\"capabilities\":{"
                          "\"search_actions\":2}}";
    CHECK(ipc_action_capability(old, strlen(old), &supported) == TL_OK && !supported);
    CHECK(ipc_action_capability(current, strlen(current), &supported) == TL_OK && supported);
    CHECK(ipc_action_capability("{}", 2, &supported) == TL_INVALID && !supported);
    request = (tl_ipc_request){.version = IPC_ACTION_VERSION,
                               .operation = IPC_QUERY,
                               .mode = SEARCH_LOCAL,
                               .software_suggestions = true,
                               .query = "editor",
                               .request_id = "suggest",
                               .limit = 10};
    CHECK(ipc_encode(&request, text, sizeof(text), &length) == TL_OK);
    CHECK(ipc_decode(text, length - 1, &decoded) == TL_OK && decoded.software_suggestions);
    CHECK(decoded.suggestions.count == 0);
    request.mode = SEARCH_WEB;
    CHECK(ipc_encode(&request, text, sizeof(text), &length) == TL_INVALID);
    request.mode = SEARCH_LOCAL;
    request.version = IPC_VERSION;
    CHECK(ipc_encode(&request, text, sizeof(text), &length) == TL_INVALID);
}
static void suggestions(void) {
    tl_software *index = NULL;
    tl_software_reader *reader = NULL;
    CHECK(software_create(&index) == TL_OK);
    const char *names[] = {"Example Editor", "Example Editor", "Installed App",
                           "Different Name", "Café Editor",    "Example Editor Tools"};
    for (size_t i = 0; i < 6; i++) {
        tl_software_entry entry = app(i == 1 ? "testing" : "flathub");
        entry.name = names[i];
        entry.installed = i == 2;
        if (i > 1)
            CHECK(snprintf(entry.ref.app_id, sizeof(entry.ref.app_id), "org.example.App%zu", i) >
                  0);
        CHECK(software_add(index, &entry) == TL_OK);
    }
    CHECK(software_finish(index) == TL_OK && software_reader_create(index, &reader) == TL_OK);
    const tl_software_entry *hits[3] = {0};
    size_t count = 0;
    bool exact = false;
    CHECK(software_suggest(reader, "example editor", NULL, hits, 3, &count, &exact) == TL_OK);
    CHECK(count == 2 && exact && strcmp(hits[0]->name, "Example Editor") == 0);
    CHECK(strcmp(hits[1]->name, "Example Editor Tools") == 0);
    CHECK(software_suggest(reader, "source code", NULL, hits, 3, &count, &exact) == TL_OK);
    CHECK(count == 0 && !exact);
    CHECK(software_suggest(reader, "installed", NULL, hits, 3, &count, &exact) == TL_OK &&
          count == 0);
    CHECK(software_suggest(reader, "CAFE\xcc\x81 EDITOR", NULL, hits, 3, &count, &exact) == TL_OK);
    CHECK(count == 1 && exact && strcmp(hits[0]->name, "Café Editor") == 0);
    CHECK(software_suggest(reader, "e", NULL, hits, 3, &count, &exact) == TL_OK && count == 0);
    atomic_bool cancel = true;
    CHECK(software_suggest(reader, "editor", &cancel, hits, 3, &count, &exact) == TL_CANCELLED);
    CHECK(count == 0 && !exact);
    software_reader_destroy(reader);
    software_destroy(index);
}
static void index_queries(void) {
    tl_software *index = NULL;
    tl_software_reader *reader = NULL;
    CHECK(software_create(&index) == TL_OK);
    tl_software_entry first = app("flathub"), second = app("testing");
    second.installed = true;
    CHECK(software_add(index, &second) == TL_OK && software_add(index, &first) == TL_OK);
    CHECK(software_finish(index) == TL_OK && software_count(index) == 2);
    CHECK(software_reader_create(index, &reader) == TL_OK);
    const tl_software_entry *hits[10] = {0};
    size_t count;
    CHECK(software_query(reader, "Example Editor", NULL, hits, 10, &count) == TL_OK && count == 2);
    CHECK(strcmp(hits[0]->ref.remote, "flathub") == 0 && !hits[0]->installed && hits[1]->installed);
    CHECK(software_resolve(index, &hits[0]->ref, hits[0]->revision) == hits[0]);
    CHECK(software_resolve(index, &hits[0]->ref, hits[0]->revision + 1) == NULL);
    CHECK(!software_ref_equal(&hits[0]->ref, &hits[1]->ref));
    CHECK(software_query(reader, "source code", NULL, hits, 10, &count) == TL_OK && count == 2);
    CHECK(software_query(reader, "", NULL, hits, 10, &count) == TL_OK && count == 0);
    atomic_bool cancelled = true;
    CHECK(software_query(reader, "editor", &cancelled, hits, 10, &count) == TL_CANCELLED &&
          count == 0);
    CHECK(software_query(reader, "editor", NULL, hits, 10, &count) == TL_OK && count == 2);
    bool exact = false;
    CHECK(software_suggest(reader, "Example Editor", NULL, hits, 3, &count, &exact) == TL_OK &&
          count == 0);
    software_reader_destroy(reader);
    software_destroy(index);
    CHECK(software_create(&index) == TL_OK);
    CHECK(software_add(index, &first) == TL_OK && software_add(index, &first) == TL_OK);
    CHECK(software_finish(index) == TL_INVALID);
    software_destroy(index);
}
static void bounded_process(void) {
    char *text = NULL;
    tl_process_budget budget = {.deadline_ms = process_now() + 1000, .remaining_bytes = 10};
    const char *args[] = {"/bin/sh", "-c", "printf hello", NULL};
    CHECK(process_capture(args, &budget, &text) == TL_OK && strcmp(text, "hello") == 0);
    process_output_destroy(text);
    CHECK(budget.remaining_bytes == 5);
    budget.remaining_bytes = 2;
    CHECK(process_capture(args, &budget, &text) == TL_LIMIT && text == NULL);
    const char *sleep[] = {"/bin/sh", "-c", "sleep 10 & wait", NULL};
    budget.deadline_ms = process_now() + 60;
    uint64_t start = process_now();
    CHECK(process_capture(sleep, &budget, &text) == TL_LIMIT && text == NULL);
    CHECK(process_now() - start < 1000);
    const char *nul[] = {"/bin/sh", "-c", "printf '\\000'", NULL};
    budget = (tl_process_budget){.deadline_ms = process_now() + 1000, .remaining_bytes = 20};
    CHECK(process_capture(nul, &budget, &text) == TL_INVALID && text == NULL);
    char directory[] = "/tmp/torchlight-child-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char command[256], marker[128];
    CHECK(snprintf(marker, sizeof(marker), "%s/orphan", directory) > 0);
    CHECK(snprintf(command, sizeof(command), "(sleep .2; touch %s) >/dev/null 2>&1 & exit 1",
                   marker) > 0);
    const char *failed[] = {"/bin/sh", "-c", command, NULL};
    budget.deadline_ms = process_now() + 1000;
    CHECK(process_capture(failed, &budget, &text) == TL_IO && text == NULL);
    usleep(300000);
    CHECK(access(marker, F_OK) != 0);
    CHECK(rmdir(directory) == 0);
}
struct fake_backend {
    atomic_uint calls, behavior;
};
static uint64_t fake_stamp(void *context) {
    (void)context;
    return 1;
}
static tl_status fake_build(void *context, bool download, tl_process_budget *budget,
                            tl_software **out) {
    (void)download;
    (void)budget;
    struct fake_backend *fake = context;
    atomic_fetch_add(&fake->calls, 1);
    *out = NULL;
    unsigned behavior = atomic_load(&fake->behavior);
    if (behavior != 0)
        return behavior == 1 ? TL_IO : TL_STATE;
    tl_software_entry entry = app("flathub");
    tl_status status = software_create(out);
    if (status == TL_OK)
        status = software_add(*out, &entry);
    if (status == TL_OK)
        status = software_finish(*out);
    return status;
}
static void await_state(tl_software_catalog *catalog, tl_software_state expected) {
    for (size_t i = 0; i < 2000; i++) {
        software_catalog_acquire(catalog);
        tl_software_state state = software_catalog_state(catalog);
        software_catalog_release(catalog);
        if (state == expected)
            return;
        usleep(1000);
    }
    CHECK(false);
}
static void catalog_lifecycle(void) {
    struct fake_backend fake = {0};
    tl_software_backend backend = {.context = &fake, .build = fake_build, .stamp = fake_stamp};
    tl_software_catalog *catalog = NULL;
    CHECK(software_catalog_create(true, &backend, &catalog) == TL_OK);
    await_state(catalog, SOFTWARE_READY);
    unsigned calls = atomic_load(&fake.calls);
    const tl_software_entry *hits[10] = {0};
    size_t count;
    software_catalog_acquire(catalog);
    for (size_t i = 0; i < 100; i++)
        CHECK(software_catalog_query(catalog, "editor", NULL, hits, 10, &count) == TL_OK &&
              count == 1);
    software_catalog_release(catalog);
    CHECK(atomic_load(&fake.calls) == calls);
    atomic_store(&fake.behavior, 1);
    software_catalog_refresh(catalog);
    await_state(catalog, SOFTWARE_STALE);
    software_catalog_acquire(catalog);
    CHECK(software_catalog_count(catalog) == 1);
    software_catalog_release(catalog);
    atomic_store(&fake.behavior, 2);
    software_catalog_refresh(catalog);
    await_state(catalog, SOFTWARE_UNAVAILABLE);
    software_catalog_acquire(catalog);
    CHECK(software_catalog_count(catalog) == 0);
    software_catalog_release(catalog);
    software_catalog_destroy(catalog);
    CHECK(software_catalog_create(false, &backend, &catalog) == TL_OK);
    CHECK(software_catalog_state(catalog) == SOFTWARE_DISABLED);
    software_catalog_destroy(catalog);
}
static void popup_targets(void) {
    tl_popup_model *model = NULL;
    CHECK(popup_model_create(&model) == TL_OK && popup_model_begin(model, "typed") == TL_OK);
    char text[16384];
    tl_json_buffer buffer;
    json_buffer_init(&buffer, text, sizeof(text));
    json_raw(&buffer, "{\"version\":2,\"request_id\":\"typed\",\"phase\":\"lexical\",\"status\":"
                      "\"ok\",\"results\":[");
    for (size_t i = 0; i < POPUP_RESULTS; i++) {
        if (i != 0)
            json_raw(&buffer, ",");
        ipc_result(&buffer, i + 1, "/a");
    }
    json_raw(&buffer, "]");
    tl_search_settings settings = {.web = true, .engine = SEARCH_DUCKDUCKGO};
    search_actions_encode(&buffer, &settings, "a", true);
    json_raw(&buffer, "}");
    CHECK(buffer.status == TL_OK && popup_model_apply(model, text, buffer.length) == TL_OK);
    CHECK(popup_model_count(model) == POPUP_RESULTS + 1);
    CHECK(popup_model_position(model) == 1);
    CHECK(popup_model_selected(model)->target_kind == POPUP_TARGET_LOCAL);
    popup_model_move(model, -1);
    CHECK(popup_model_selected(model)->target_kind == POPUP_TARGET_WEB);
    CHECK(popup_model_apply(model, text, buffer.length) == TL_OK);
    CHECK(popup_model_position(model) == 0 && popup_model_count(model) == POPUP_RESULTS + 1);
    CHECK(popup_model_group_count(model, POPUP_GROUP_WEB) == 1);
    CHECK(popup_model_begin(model, "typed") == TL_OK);
    json_buffer_init(&buffer, text, sizeof(text));
    json_raw(&buffer, "{\"version\":2,\"request_id\":\"typed\",\"phase\":\"final\",\"status\":"
                      "\"ok\",\"results\":[");
    tl_software_entry first = app("flathub"), second = app("testing");
    ipc_software_result(&buffer, &first);
    json_raw(&buffer, ",");
    ipc_software_result(&buffer, &second);
    json_raw(&buffer, "],\"actions\":[]}");
    CHECK(popup_model_apply(model, text, buffer.length) == TL_OK);
    CHECK(popup_model_count(model) == 2 &&
          popup_model_selected(model)->target_kind == POPUP_TARGET_SOFTWARE);
    popup_model_move(model, 1);
    CHECK(strcmp(popup_model_selected(model)->software.remote, "testing") == 0);
    CHECK(popup_model_apply(model, text, buffer.length) == TL_OK);
    CHECK(strcmp(popup_model_selected(model)->software.remote, "testing") == 0);
    CHECK(popup_model_selected(model)->path[0] == 0);
    popup_model_destroy(model);
}
static void mixed_suggestions(void) {
    tl_popup_model *model = NULL;
    CHECK(popup_model_create(&model) == TL_OK && popup_model_begin(model, "mixed") == TL_OK);
    tl_ipc_request request = {.software_suggestions = true};
    request.suggestions.count = IPC_SOFTWARE_SUGGESTIONS;
    request.suggestions.preferred = true;
    for (size_t i = 0; i < IPC_SOFTWARE_SUGGESTIONS; i++) {
        tl_software_entry entry = app("flathub");
        tl_ipc_suggestion *copy = &request.suggestions.entries[i];
        copy->ref = entry.ref;
        CHECK(snprintf(copy->ref.app_id, sizeof(copy->ref.app_id), "org.example.App%zu", i) > 0);
        memcpy(copy->name, "Example Editor", sizeof("Example Editor"));
        copy->revision = copy->source_revision = 1;
    }
    for (unsigned preferred = 0; preferred < 2; preferred++) {
        request.suggestions.preferred = preferred != 0;
        char text[16384];
        tl_json_buffer buffer;
        json_buffer_init(&buffer, text, sizeof(text));
        json_raw(&buffer, "{\"version\":2,\"request_id\":\"mixed\",\"phase\":\"final\","
                          "\"status\":\"ok\",\"results\":[");
        for (size_t i = 0; i < POPUP_RESULTS; i++) {
            if (i != 0)
                json_raw(&buffer, ",");
            ipc_result(&buffer, i + 1, "/file");
        }
        json_raw(&buffer, "]");
        tl_search_settings settings = {.web = true};
        search_actions_encode(&buffer, &settings, "example editor", true);
        ipc_suggestions_result(&buffer, &request, true);
        json_raw(&buffer, "}");
        CHECK(popup_model_begin(model, "mixed") == TL_OK);
        CHECK(buffer.status == TL_OK && popup_model_apply(model, text, buffer.length) == TL_OK);
        CHECK(popup_model_count(model) == 14 && popup_model_position(model) == 1);
        CHECK(popup_model_selected(model)->target_kind ==
              (preferred != 0 ? POPUP_TARGET_SOFTWARE : POPUP_TARGET_LOCAL));
        popup_model_move(model, 1);
        tl_popup_row chosen = *popup_model_selected(model);
        CHECK(popup_model_apply(model, text, buffer.length) == TL_OK);
        CHECK(popup_row_same(&chosen, popup_model_selected(model)));
    }
    popup_model_destroy(model);
}
static void settings_validation(void) {
    char directory[] = "/tmp/torchlight-settings-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256], database[256];
    CHECK(snprintf(path, sizeof(path), "%s/config", directory) > 0);
    CHECK(snprintf(database, sizeof(database), "%s/catalog.db", directory) > 0);
    const char *cases[] = {"web_search = false\nweb_engine = google\nsoftware_search = auto\n",
                           "web_engine = arbitrary\n",
                           "software_search = true\n",
                           "web_search = auto\n",
                           "web_search = true\nweb_search = false\n",
                           "unknown = value\n"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        FILE *file = fopen(path, "w");
        CHECK(file != NULL);
        CHECK(fputs(cases[i], file) >= 0 && fclose(file) == 0);
        tl_config *config = NULL;
        CHECK(config_create("torchlight", database, path, NULL, &config) == TL_OK);
        tl_search_settings settings;
        CHECK(search_actions_settings(config, &settings) == (i == 0 ? TL_OK : TL_INVALID));
        if (i == 0)
            CHECK(!settings.web && settings.software && settings.engine == SEARCH_GOOGLE);
        config_destroy(config);
    }
    CHECK(unlink(path) == 0 && rmdir(directory) == 0);
}
void test_search_actions(void) {
    prefixes();
    website_addresses();
    website_selection();
    wire();
    index_queries();
    suggestions();
    bounded_process();
    catalog_lifecycle();
    popup_targets();
    mixed_suggestions();
    settings_validation();
}
