/* Embedder adapter ownership, role dispatch and invalid inference regressions. */
#include "test.h"
#include "torchlight/embed.h"
#include "torchlight/vector.h"
#include <math.h>
#include <string.h>

struct backend_fixture {
    size_t calls, destroys;
    tl_status status;
    float values[2];
    tl_embed_input role;
    char text[64];
};

static tl_status encode_fixture(void *context, tl_embed_input role, const char *text, float *out,
                                size_t dimensions) {
    struct backend_fixture *fixture = context;
    CHECK(dimensions == 2);
    fixture->calls++;
    fixture->role = role;
    if (strlen(text) < sizeof(fixture->text))
        memcpy(fixture->text, text, strlen(text) + 1);
    memcpy(out, fixture->values, sizeof(fixture->values));
    return fixture->status;
}

static void destroy_fixture(void *context) {
    struct backend_fixture *fixture = context;
    fixture->destroys++;
}

static const tl_embedder_backend FIXTURE_BACKEND = {encode_fixture, destroy_fixture};
static const tl_emb_model FIXTURE_MODEL = {
    5, 2, "fixture", "revision-1", "tokenizer-1", "prepared-text-1", "none", "float32-l2-1"};

static void check_adapter(void) {
    struct backend_fixture fixture = {.status = TL_OK, .values = {3, 4}};
    tl_embedder *embedder = NULL;
    char revision[] = "revision-1";
    tl_emb_model model = FIXTURE_MODEL;
    model.model_revision = revision;
    CHECK(embedder_create(&model, &FIXTURE_BACKEND, &fixture, &embedder) == TL_OK);
    revision[0] = 'X';
    CHECK(strcmp(embedder_model(embedder)->model_revision, "revision-1") == 0);
    CHECK(embedder_model(embedder)->emb_gen == 5 && embedder_model(embedder)->dimensions == 2);
    float out[2];
    CHECK(embedder_encode(embedder, EMBED_QUERY, "tax receipts", out, 2) == TL_OK);
    CHECK(fixture.calls == 1 && fixture.role == EMBED_QUERY);
    CHECK(strcmp(fixture.text, "tax receipts") == 0);
    CHECK(fabsf(out[0] - 0.6F) < 1e-6F && fabsf(out[1] - 0.8F) < 1e-6F);
    CHECK(embedder_encode(embedder, EMBED_DOCUMENT, "Caf\xc3\xa9.pdf", out, 2) == TL_OK);
    CHECK(fixture.role == EMBED_DOCUMENT);
    embedder_destroy(embedder);
    CHECK(fixture.destroys == 1);
    embedder_destroy(NULL);
    CHECK(embedder_model(NULL) == NULL);
}

static void check_failed_creation(void) {
    struct backend_fixture fixture = {0};
    tl_embedder *embedder = NULL;
    tl_emb_model model = FIXTURE_MODEL;
    model.model_revision = NULL;
    CHECK(embedder_create(&model, &FIXTURE_BACKEND, &fixture, &embedder) == TL_INVALID);
    CHECK(embedder == NULL && fixture.destroys == 0);
    model = FIXTURE_MODEL;
    model.dimensions = VECTOR_MAX_DIMENSIONS + 1;
    CHECK(embedder_create(&model, &FIXTURE_BACKEND, &fixture, &embedder) == TL_INVALID);
    model = FIXTURE_MODEL;
    model.emb_gen = 0;
    CHECK(embedder_create(&model, &FIXTURE_BACKEND, &fixture, &embedder) == TL_INVALID);
    model = FIXTURE_MODEL;
    model.preprocessing_version = "";
    CHECK(embedder_create(&model, &FIXTURE_BACKEND, &fixture, &embedder) == TL_INVALID);
    model.preprocessing_version = "bad\xff";
    CHECK(embedder_create(&model, &FIXTURE_BACKEND, &fixture, &embedder) == TL_INVALID);
    char long_version[EMBED_METADATA_BYTES + 1];
    memset(long_version, 'x', sizeof(long_version) - 1);
    long_version[sizeof(long_version) - 1] = 0;
    model.preprocessing_version = long_version;
    CHECK(embedder_create(&model, &FIXTURE_BACKEND, &fixture, &embedder) == TL_INVALID);
    tl_embedder_backend incomplete = {.encode = encode_fixture};
    CHECK(embedder_create(&FIXTURE_MODEL, &incomplete, &fixture, &embedder) == TL_INVALID);
    CHECK(embedder_create(NULL, &FIXTURE_BACKEND, &fixture, &embedder) == TL_INVALID);
    CHECK(fixture.destroys == 0 && fixture.calls == 0);
}

static void check_failed_encoding(void) {
    struct backend_fixture fixture = {.status = TL_IO, .values = {3, 4}};
    tl_embedder *embedder = NULL;
    CHECK(embedder_create(&FIXTURE_MODEL, &FIXTURE_BACKEND, &fixture, &embedder) == TL_OK);
    float out[] = {8, 9};
    CHECK(embedder_encode(embedder, EMBED_QUERY, "query", out, 2) == TL_IO);
    CHECK(out[0] == 0 && out[1] == 0);
    fixture.status = TL_OK;
    const float invalid[][2] = {{0, 0}, {NAN, 1}, {1, INFINITY}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        memcpy(fixture.values, invalid[i], sizeof(fixture.values));
        CHECK(embedder_encode(embedder, EMBED_QUERY, "query", out, 2) == TL_STATE);
        CHECK(out[0] == 0 && out[1] == 0);
    }
    size_t calls = fixture.calls;
    CHECK(embedder_encode(embedder, EMBED_QUERY, "", out, 2) == TL_INVALID);
    CHECK(embedder_encode(embedder, EMBED_QUERY, "raw\xff", out, 2) == TL_INVALID);
    CHECK(embedder_encode(embedder, EMBED_QUERY, NULL, out, 2) == TL_INVALID);
    CHECK(embedder_encode(embedder, (tl_embed_input)99, "query", out, 2) == TL_INVALID);
    CHECK(embedder_encode(embedder, EMBED_QUERY, "query", out, 1) == TL_INVALID);
    char long_text[EMBED_TEXT_BYTES + 2];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = 0;
    CHECK(embedder_encode(embedder, EMBED_QUERY, long_text, out, 2) == TL_LIMIT);
    CHECK(fixture.calls == calls);
    fixture.values[0] = 1;
    fixture.values[1] = 0;
    CHECK(embedder_encode(embedder, EMBED_QUERY, "query", out, 2) == TL_OK);
    CHECK(out[0] == 1 && out[1] == 0);
    embedder_destroy(embedder);
}

void test_embed(void) {
    check_adapter();
    check_failed_creation();
    check_failed_encoding();
}
