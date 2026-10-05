/* Backend lifetime, emb_gen identity and validated normalized inference output. */
#include "torchlight/embed.h"
#include "torchlight/json.h"
#include "torchlight/vector.h"
#include <stdlib.h>
#include <string.h>

enum { EMBED_METADATA_FIELDS = 6 };
struct tl_embedder {
    tl_emb_model model;
    tl_embedder_backend backend;
    void *context;
    char metadata[EMBED_METADATA_FIELDS][EMBED_METADATA_BYTES];
};

static tl_status copy_model(const tl_emb_model *model, tl_embedder *embedder) {
    if (model == NULL || model->emb_gen == 0 || model->dimensions == 0 ||
        model->dimensions > VECTOR_MAX_DIMENSIONS)
        return TL_INVALID;
    const char *fields[] = {model->model_id,           model->model_revision,
                            model->tokenizer_version,  model->preprocessing_version,
                            model->projection_version, model->quantization_version};
    for (size_t i = 0; i < EMBED_METADATA_FIELDS; i++) {
        if (fields[i] == NULL)
            return TL_INVALID;
        size_t length = strnlen(fields[i], EMBED_METADATA_BYTES);
        if (length == 0 || length == EMBED_METADATA_BYTES || !json_utf8(fields[i]))
            return TL_INVALID;
        memcpy(embedder->metadata[i], fields[i], length + 1);
    }
    embedder->model = (tl_emb_model){
        model->emb_gen,        model->dimensions,     embedder->metadata[0], embedder->metadata[1],
        embedder->metadata[2], embedder->metadata[3], embedder->metadata[4], embedder->metadata[5]};
    return TL_OK;
}

tl_status embedder_create(const tl_emb_model *model, const tl_embedder_backend *backend,
                          void *context, tl_embedder **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (backend == NULL || backend->encode == NULL || backend->destroy == NULL)
        return TL_INVALID;
    tl_embedder *embedder = calloc(1, sizeof(*embedder));
    if (embedder == NULL)
        return TL_NOMEM;
    tl_status status = copy_model(model, embedder);
    if (status != TL_OK) {
        free(embedder);
        return status;
    }
    embedder->backend = *backend;
    embedder->context = context;
    *out = embedder;
    return TL_OK;
}

void embedder_destroy(tl_embedder *embedder) {
    if (embedder == NULL)
        return;
    embedder->backend.destroy(embedder->context);
    free(embedder);
}

const tl_emb_model *embedder_model(const tl_embedder *embedder) {
    return embedder == NULL ? NULL : &embedder->model;
}

tl_status embedder_encode(tl_embedder *embedder, tl_embed_input role, const char *text, float *out,
                          size_t dimensions) {
    if (embedder == NULL || out == NULL || dimensions != embedder->model.dimensions)
        return TL_INVALID;
    memset(out, 0, dimensions * sizeof(*out));
    if (text == NULL || (role != EMBED_QUERY && role != EMBED_DOCUMENT))
        return TL_INVALID;
    size_t length = strnlen(text, EMBED_TEXT_BYTES + 1);
    if (length > EMBED_TEXT_BYTES)
        return TL_LIMIT;
    if (length == 0 || !json_utf8(text))
        return TL_INVALID;
    tl_status status = embedder->backend.encode(embedder->context, role, text, out, dimensions);
    if (status == TL_OK && vector_normalize(out, dimensions, out) != TL_OK)
        status = TL_STATE;
    if (status != TL_OK)
        memset(out, 0, dimensions * sizeof(*out));
    return status;
}
