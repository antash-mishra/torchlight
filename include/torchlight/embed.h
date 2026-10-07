/* Swappable local embedding backend and immutable, owned emb_gen metadata. */
#ifndef TORCHLIGHT_EMBED_H
#define TORCHLIGHT_EMBED_H
#include "torchlight/common.h"
#include <stdbool.h>

#define EMBED_METADATA_BYTES 256
#define EMBED_TEXT_BYTES 16384
typedef struct tl_embedder tl_embedder;
typedef enum { EMBED_QUERY, EMBED_DOCUMENT } tl_embed_input;
/* nested_prefixes declares that the leading components of an embedding are
 * themselves a usable lower-dimensional embedding (Matryoshka training or a
 * variance-ordered projection), which lets vector search shortlist rows by a
 * prefix. It describes the model, not the stored vectors, so it is not part of
 * emb_gen. */
typedef struct {
    uint64_t emb_gen;
    size_t dimensions;
    const char *model_id, *model_revision, *tokenizer_version, *preprocessing_version;
    const char *projection_version, *quantization_version;
    bool nested_prefixes;
} tl_emb_model;
typedef struct {
    /** Encode prepared UTF-8 text into exactly dimensions caller-owned floats.
     * All inputs/context are borrowed; return TL_OK or a backend error. The
     * role permits model-specific query/document prompts. Backend may allocate;
     * it must not write outside output, retain text or assume preemption. */
    tl_status (*encode)(void *context, tl_embed_input role, const char *text, float *out,
                        size_t dimensions);
    /** Release owned context/resources once when embedder is destroyed.
     * NULL context is allowed; no errors. */
    void (*destroy)(void *context);
} tl_embedder_backend;

/** Create owned adapter, copying model metadata and callbacks. Transfer context
 * to adapter only on success; caller owns it on failure. Both callbacks required.
 * emb_gen must be nonzero, dimensions 1..VECTOR_MAX_DIMENSIONS; each metadata
 * string is nonempty UTF-8 and fits EMBED_METADATA_BYTES including its terminator.
 * Use explicit versions such as "none" for unused transforms. TL_INVALID/NOMEM;
 * out NULL on failure. Caller assigns emb_gen uniquely for this full descriptor. */
tl_status embedder_create(const tl_emb_model *model, const tl_embedder_backend *backend,
                          void *context, tl_embedder **out);
/** Free adapter and invoke its backend destroy once. Stop all encoding first;
 * NULL allowed, no errors. */
void embedder_destroy(tl_embedder *embedder);
/** Borrow immutable descriptor until embedder destruction; NULL for NULL.
 * No allocation, ownership transfer or errors. */
const tl_emb_model *embedder_model(const tl_embedder *embedder);
/** Encode nonempty prepared UTF-8 text (at most EMBED_TEXT_BYTES bytes) and
 * normalize output into caller-owned floats. Reject malformed bytes; raw paths
 * require separate versioned preprocessing and retain their original identity.
 * dimensions must match model. TL_INVALID for arguments/text, TL_LIMIT for long
 * text, TL_STATE for invalid/zero backend vectors; other backend errors propagate.
 * For valid embedder/out/dimensions, clear output on every failure. One caller at
 * a time per adapter; create independent adapters for concurrent inference.
 * Wrapper allocates nothing; backend allocations are measured separately. */
tl_status embedder_encode(tl_embedder *embedder, tl_embed_input role, const char *text, float *out,
                          size_t dimensions);
#endif
