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
    CHECK(lexical_add(engine, 2, "/r/world", false) == TL_OK);
    CHECK(lexical_add(engine, 3, "/r/notes", false) == TL_OK);
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
/* Publish *snapshot, reclaiming retired views the worker no longer pins. */
static void publish(struct semantic_fixture *fixture, tl_catalog_snapshot **snapshot) {
    for (size_t i = 0; i < SEMANTIC_WAIT_ATTEMPTS; i++) {
        catalog_reclaim(fixture->catalog);
        tl_status status = catalog_publish(fixture->catalog, snapshot);
        if (status == TL_OK)
            return;
        CHECK(status == TL_LIMIT);
        pause_worker();
    }
    CHECK(false);
}
/* Wait until the service serves catalog_gen with no stage in progress. */
static tl_semantic_stats published(struct semantic_fixture *fixture, uint64_t catalog_gen) {
    tl_semantic_stats stats = {0};
    for (size_t i = 0; i < SEMANTIC_WAIT_ATTEMPTS; i++) {
        CHECK(semantic_stats(fixture->service, &stats) == TL_OK);
        if (stats.available && stats.catalog_gen == catalog_gen && !stats.building)
            return stats;
        pause_worker();
    }
    CHECK(false);
    return stats;
}
/* Run a hybrid "hello" query with one lexical candidate. The test model maps
 * every embeddable text into one quadrant, so all embedded rows are hits. */
static void hybrid_hello(struct semantic_fixture *fixture, const tl_semantic_stats *stats,
                         uint64_t token, const tl_result *lexical, char *output, size_t capacity) {
    tl_ipc_request request = {
        .operation = IPC_QUERY, .request_id = "hybrid", .query = "hello", .limit = 8};
    uint64_t emb_gen = 0;
    CHECK(semantic_submit(fixture->service, 1, token, &request, "search-hybrid", stats->catalog_gen,
                          stats->desktop_gen, lexical, 1, &emb_gen) == TL_OK);
    size_t length = 0;
    for (size_t i = 0; i < SEMANTIC_WAIT_ATTEMPTS && length == 0; i++) {
        CHECK(semantic_take(fixture->service, 1, token, false, output, capacity, &length) == TL_OK);
        if (length == 0)
            pause_worker();
    }
    CHECK(length > 0 && strstr(output, "\"reason\":\"hybrid\"") != NULL);
}
/* Publish a view derived from the active catalog view's base. ids/paths are
 * every entry changed since that base; retired are the ids this batch touched. */
static void publish_derived(struct semantic_fixture *fixture, uint64_t catalog_gen,
                            const uint64_t *ids, const char *const *paths, size_t count,
                            const uint64_t *retired, size_t retired_count) {
    tl_catalog_snapshot *pin = NULL, *derived = NULL;
    tl_lexical *delta = NULL;
    CHECK(catalog_pin(fixture->catalog, &pin) == TL_OK);
    CHECK(lexical_create(&delta) == TL_OK &&
          lexical_set_reference(delta, catalog_snapshot_base(pin)) == TL_OK);
    for (size_t i = 0; i < count; i++)
        CHECK(lexical_add(delta, ids[i], paths[i], false) == TL_OK);
    CHECK(lexical_finish(delta) == TL_OK);
    CHECK(catalog_snapshot_derive(pin, &delta, retired, retired_count, catalog_gen, 1, &derived) ==
          TL_OK);
    catalog_unpin(pin);
    publish(fixture, &derived);
}
/* A catalog update restages incrementally: unchanged rows copy their vectors
 * from the published snapshot, a renamed row and a new row are embedded. */
static void incremental_stage(struct semantic_fixture *fixture) {
    tl_lexical *engine = NULL;
    tl_catalog_snapshot *snapshot = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_add(engine, 1, "/r/hello", false) == TL_OK);
    CHECK(lexical_add(engine, 2, "/r/planet", false) == TL_OK);
    CHECK(lexical_add(engine, 3, "/r/notes", false) == TL_OK);
    CHECK(lexical_add(engine, 9, "/r/fresh", false) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    CHECK(catalog_snapshot_create(&engine, 2, 1, &snapshot) == TL_OK);
    publish(fixture, &snapshot);
    tl_semantic_stats stats = published(fixture, 2);
    CHECK(stats.entries == 4 && stats.reused == 2);
    /* "hello" reuses its vector; the renamed id resolves through new metadata. */
    tl_result lexical[] = {{2, "/r/planet", 1}};
    char output[2048];
    hybrid_hello(fixture, &stats, 7, lexical, output, sizeof(output));
    CHECK(strstr(output, "\"path\":\"/r/hello\"") != NULL);
    CHECK(strstr(output, "\"path\":\"/r/planet\"") != NULL && strstr(output, "/r/world") == NULL);
}
/* Views derived from one catalog base stage derived semantic snapshots: only
 * changed rows are staged, replaced and removed base rows are hidden from
 * search and metadata, and a later full stage reuses rows of both segments.
 * The base (catalog_gen 2) is 1 hello, 2 planet, 3 notes and 9 fresh. */
static void derived_stage(struct semantic_fixture *fixture) {
    /* Rename 1 to "world", remove 3, add 10 "cafe": two rows are embedded. */
    const uint64_t first_ids[] = {1, 10}, first_retired[] = {1, 3, 10};
    const char *const first_paths[] = {"/r/world", "/r/cafe"};
    publish_derived(fixture, 3, first_ids, first_paths, 2, first_retired, 3);
    tl_semantic_stats stats = published(fixture, 3);
    CHECK(stats.entries == 4 && stats.reused == 2);
    CHECK(stats.derived_stages == 1 && stats.full_stages == 2);
    tl_result planet[] = {{2, "/r/planet", 1}};
    char output[2048];
    hybrid_hello(fixture, &stats, 8, planet, output, sizeof(output));
    CHECK(strstr(output, "\"path\":\"/r/world\"") != NULL);
    CHECK(strstr(output, "\"path\":\"/r/cafe\"") != NULL);
    CHECK(strstr(output, "\"path\":\"/r/planet\"") != NULL); /* base metadata */
    CHECK(strstr(output, "/r/hello") == NULL && strstr(output, "/r/notes") == NULL);
    /* From the derived snapshot: remove 9, add 11 "play". Own rows 1 and 10
     * keep their vectors; only 11 is embedded. */
    const uint64_t second_ids[] = {1, 10, 11}, second_retired[] = {9, 11};
    const char *const second_paths[] = {"/r/world", "/r/cafe", "/r/play"};
    publish_derived(fixture, 4, second_ids, second_paths, 3, second_retired, 2);
    stats = published(fixture, 4);
    CHECK(stats.entries == 4 && stats.reused == 3);
    CHECK(stats.derived_stages == 2 && stats.full_stages == 2);
    hybrid_hello(fixture, &stats, 9, planet, output, sizeof(output));
    CHECK(strstr(output, "\"path\":\"/r/play\"") != NULL);
    CHECK(strstr(output, "\"path\":\"/r/world\"") != NULL);
    CHECK(strstr(output, "/r/hello") == NULL && strstr(output, "/r/fresh") == NULL);
    /* A new catalog base forces a full stage that reuses own and base rows and
     * frees the derived snapshot and its base. */
    tl_lexical *engine = NULL;
    tl_catalog_snapshot *snapshot = NULL;
    const uint64_t ids[] = {1, 2, 10, 11, 12};
    const char *const paths[] = {"/r/world", "/r/planet", "/r/cafe", "/r/play", "/r/hello"};
    CHECK(lexical_create(&engine) == TL_OK);
    for (size_t i = 0; i < 5; i++)
        CHECK(lexical_add(engine, ids[i], paths[i], false) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    CHECK(catalog_snapshot_create(&engine, 5, 1, &snapshot) == TL_OK);
    publish(fixture, &snapshot);
    stats = published(fixture, 5);
    CHECK(stats.entries == 5 && stats.reused == 4);
    CHECK(stats.derived_stages == 2 && stats.full_stages == 3);
    hybrid_hello(fixture, &stats, 10, planet, output, sizeof(output));
    CHECK(strstr(output, "\"path\":\"/r/hello\"") != NULL);
    CHECK(strstr(output, "\"path\":\"/r/play\"") != NULL);
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
    CHECK(stats.reused == 0); /* the first stage embedded everything */
    incremental_stage(&fixture);
    derived_stage(&fixture);
    destroy_fixture(&fixture);
}
