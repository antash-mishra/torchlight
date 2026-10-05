/* Buffer exhaustion must preserve ready/cancelled jobs and their owned snapshots. */
#include "test.h"
#include "torchlight/semantic.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { SEMANTIC_WAIT_ATTEMPTS = 500, SEMANTIC_WAIT_NS = 10000000 };
struct semantic_fixture {
    char directory[64], model[128], database[128];
    tl_catalog *catalog;
    tl_desktop *desktop;
    tl_semantic *service;
};
static void pause_worker(void) {
    struct timespec delay = {.tv_nsec = SEMANTIC_WAIT_NS};
    CHECK(nanosleep(&delay, NULL) == 0);
}
static void create_fixture(struct semantic_fixture *fixture) {
    memcpy(fixture->directory, "/tmp/torchlight-semantic-unit-XXXXXX",
           sizeof("/tmp/torchlight-semantic-unit-XXXXXX"));
    CHECK(mkdtemp(fixture->directory) != NULL);
    int length = snprintf(fixture->model, sizeof(fixture->model), "%s/model", fixture->directory);
    CHECK(length > 0 && (size_t)length < sizeof(fixture->model));
    length = snprintf(fixture->database, sizeof(fixture->database), "%s/db", fixture->directory);
    CHECK(length > 0 && (size_t)length < sizeof(fixture->database));
    test_potion_file(fixture->model);
    tl_lexical *engine = NULL;
    tl_catalog_snapshot *snapshot = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_add(engine, 1, "/r/hello", false) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    CHECK(catalog_create(2, &fixture->catalog) == TL_OK);
    CHECK(catalog_snapshot_create(&engine, 1, 1, &snapshot) == TL_OK);
    CHECK(catalog_publish(fixture->catalog, &snapshot) == TL_OK);
    CHECK(desktop_create(&fixture->desktop) == TL_OK);
    tl_semantic_options options = {.model_path = fixture->model,
                                   .database = fixture->database,
                                   .catalog = fixture->catalog,
                                   .desktop = fixture->desktop,
                                   .vector_budget = SEMANTIC_VECTOR_BYTES,
                                   .metadata_budget = SEMANTIC_VECTOR_BYTES,
                                   .deadline_ms = 60000};
    CHECK(semantic_create(&options, &fixture->service) == TL_OK);
}
static tl_semantic_stats ready(struct semantic_fixture *fixture) {
    tl_semantic_stats stats = {0};
    for (size_t i = 0; i < SEMANTIC_WAIT_ATTEMPTS; i++) {
        CHECK(semantic_stats(fixture->service, &stats) == TL_OK);
        if (stats.available)
            return stats;
        pause_worker();
    }
    CHECK(stats.available);
    return stats;
}
static void wait_for_small_buffer(struct semantic_fixture *fixture, uint64_t token) {
    char output[1];
    size_t length = 1;
    for (size_t i = 0; i < SEMANTIC_WAIT_ATTEMPTS; i++) {
        tl_status status =
            semantic_take(fixture->service, 0, token, false, output, sizeof(output), &length);
        CHECK(length == 0);
        if (status == TL_LIMIT)
            return;
        CHECK(status == TL_OK);
        pause_worker();
    }
    CHECK(false);
}
static void destroy_fixture(struct semantic_fixture *fixture) {
    semantic_destroy(fixture->service);
    desktop_destroy(fixture->desktop);
    CHECK(catalog_destroy(fixture->catalog) == TL_OK);
    CHECK(unlink(fixture->model) == 0 && unlink(fixture->database) == 0);
    CHECK(rmdir(fixture->directory) == 0);
}
void test_semantic(void) {
    struct semantic_fixture fixture = {0};
    create_fixture(&fixture);
    tl_semantic_stats stats = ready(&fixture);
    tl_ipc_request request = {
        .operation = IPC_QUERY, .request_id = "retry", .query = "hello", .limit = 1};
    uint64_t emb_gen = 0;
    CHECK(semantic_submit(fixture.service, 0, 1, &request, "search", stats.catalog_gen,
                          stats.desktop_gen, NULL, 0, &emb_gen) == TL_OK);
    CHECK(emb_gen == stats.emb_gen);
    wait_for_small_buffer(&fixture, 1);
    char output[2048];
    size_t length = 1;
    CHECK(semantic_take(fixture.service, 0, 1, false, output, 1, &length) == TL_LIMIT &&
          length == 0);
    CHECK(semantic_take(fixture.service, 0, 1, false, output, sizeof(output), &length) == TL_OK);
    CHECK(length > 0 && strstr(output, "\"reason\":\"hybrid\"") != NULL);
    CHECK(strstr(output, "\"id\":\"1\"") != NULL);
    CHECK(semantic_take(fixture.service, 0, 1, false, output, sizeof(output), &length) == TL_STATE);
    CHECK(semantic_submit(fixture.service, 0, 2, &request, "search", stats.catalog_gen,
                          stats.desktop_gen, NULL, 0, &emb_gen) == TL_OK);
    CHECK(semantic_take(fixture.service, 0, 2, true, output, 1, &length) == TL_LIMIT &&
          length == 0);
    CHECK(semantic_take(fixture.service, 0, 2, true, output, sizeof(output), &length) == TL_OK);
    CHECK(length > 0 && strstr(output, "\"status\":\"cancelled\"") != NULL);
    destroy_fixture(&fixture);
}
