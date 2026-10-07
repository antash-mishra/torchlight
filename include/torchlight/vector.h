/* Owned float/int8 cosine, a two-pass prefix shortlist for large int8 indexes
 * and an experimental binary shortlist; bounded query scratch. */
#ifndef TORCHLIGHT_VECTOR_H
#define TORCHLIGHT_VECTOR_H
#include "torchlight/common.h"

#define VECTOR_MAX_DIMENSIONS 4096
#define VECTOR_MAX_RESULTS 1000
/* Prefix lengths are whole blocks so the first pass needs no remainder loop. */
#define VECTOR_PREFIX_BLOCK 16
typedef struct tl_vector tl_vector;
typedef struct tl_vector_workspace tl_vector_workspace;
/** Create exhaustive int8 cosine index. Same ownership/errors as vector_create;
 * budget includes ids, quantized components and per-row inverse norms. Quantize
 * normalized floats using round(component*127), then normalize stored integers
 * during cosine evaluation. No approximate candidate pruning. Compare recall
 * against float reference before deployment; quantization version int8-l2-1. */
tl_status vector_create_int8(uint64_t emb_gen, size_t dimensions, size_t capacity,
                             size_t budget_bytes, tl_vector **out);
/** Experimental sign-bit shortlist followed by int8 cosine. Same ownership and
 * errors as vector_create_int8; shortlist >= output capacity is required at
 * query time. Store sign bits plus integers and normalize during rescoring.
 * Hamming ties prefer smaller ids. Validate recall/latency against exhaustive
 * search on the target corpus before selecting this approximate format. */
tl_status vector_create_binary_int8(uint64_t emb_gen, size_t dimensions, size_t capacity,
                                    size_t shortlist, size_t budget_bytes, tl_vector **out);
/** Two-pass int8 cosine for large indexes. Rows use vector_create_int8's
 * quantization (int8-l2-1) and full cosines are bit-identical to it; only
 * candidate selection differs. When more than shortlist rows are searchable, a
 * first pass ranks every row by int8 cosine over its first prefix_dimensions
 * components (exact integer arithmetic with an int16-rounded query, AVX2 when
 * the CPU supports it, identical results either way) and keeps the best
 * shortlist rows, ties by position; the second pass scores only those with the
 * full cosine. Otherwise, or for a query whose prefix is all zero, every row is
 * scored. Top-k is therefore approximate: valid only for embeddings whose
 * leading components form a meaningful embedding (Matryoshka truncation or a
 * variance-ordered projection); measure recall against vector_create_int8.
 * prefix_dimensions is a positive multiple of VECTOR_PREFIX_BLOCK below
 * dimensions and shortlist is nonzero, else TL_INVALID. Query output capacity
 * must not exceed shortlist (TL_LIMIT). Budget covers ids, components, full and
 * prefix inverse norms; workspaces add O(prefix + shortlist) scratch. Other
 * ownership/errors as vector_create_int8. */
tl_status vector_create_prefix_int8(uint64_t emb_gen, size_t dimensions, size_t capacity,
                                    size_t prefix_dimensions, size_t shortlist, size_t budget_bytes,
                                    tl_vector **out);
typedef struct {
    uint64_t id;
    double cosine;
} tl_vector_result;

/** L2-normalize dimensions finite floats into caller-owned out. Allows exact
 * in-place use; otherwise arrays must not overlap. TL_INVALID for NULL, zero
 * or excessive dimensions, zero norm or NaN/infinity. Errors leave out unchanged.
 * Double accumulation handles finite float extremes. No allocation or I/O. */
tl_status vector_normalize(const float *values, size_t dimensions, float *out);
/** Create owned builder for one nonzero emb_gen and 1..VECTOR_MAX_DIMENSIONS.
 * Reserve capacity rows (zero allowed) in owned memory; budget_bytes bounds
 * the index struct, ids and float payload, excluding allocator/workspaces.
 * TL_INVALID/NOMEM/LIMIT; out NULL on failure. SIZE_MAX permits any fitting size.
 * The caller ensures emb_gen identifies the complete model/transform/format. */
tl_status vector_create(uint64_t emb_gen, size_t dimensions, size_t capacity, size_t budget_bytes,
                        tl_vector **out);
/** Free index and vectors; finish queries and destroy workspaces first.
 * NULL allowed, no errors. */
void vector_destroy(tl_vector *index);
/** Copy and normalize a finite nonzero vector with strictly increasing nonzero
 * id. dimensions and emb_gen must match the builder. Caller retains values.
 * TL_INVALID for arguments/id/vector, TL_STATE if sealed or emb_gen differs,
 * TL_LIMIT when reserved rows are full. Errors leave the builder unchanged. */
tl_status vector_add(tl_vector *index, uint64_t id, uint64_t emb_gen, const float *values,
                     size_t dimensions);
/** Find the position of id among an index's rows (built or sealed, rows are
 * ascending by id). TL_INVALID for NULL arguments, TL_STATE when absent;
 * *position untouched then. No allocation/I/O. */
tl_status vector_position(const tl_vector *index, uint64_t id, size_t *position);
/** Append source's stored row at position, under its id, to an unsealed
 * builder of the same emb_gen, dimensions and storage format (including the
 * prefix length; shortlists may differ), copying the
 * normalized or quantized data exactly so the row scores bit-identically
 * without re-embedding. Ids must still strictly increase. TL_INVALID for
 * arguments, ids or a format mismatch, TL_STATE if sealed or emb_gen
 * differs, TL_LIMIT when reserved rows are full. No allocation. */
tl_status vector_add_row(tl_vector *index, const tl_vector *source, size_t position);
/** Seal builder for immutable searches. TL_INVALID for NULL, TL_STATE if sealed;
 * TL_OK otherwise, including empty indexes. No allocation/ownership transfer. */
tl_status vector_finish(tl_vector *index);
/** Create owned query scratch for sealed index; index must outlive workspace.
 * TL_INVALID/STATE/NOMEM/LIMIT; out NULL on failure. Each concurrent query needs
 * a separate workspace, used by one coordinator at a time. */
tl_status vector_workspace_create(const tl_vector *index, tl_vector_workspace **out);
/** Free scratch without touching the borrowed index; NULL allowed, no errors. */
void vector_workspace_destroy(tl_vector_workspace *workspace);
/** Attach an optional bitmap of excluded row positions (bit p of word p/64)
 * for later queries on this workspace, or NULL to clear. Excluded rows are
 * never scored or returned; with the sign-bit shortlist they may still take
 * shortlist slots. Borrowed until replaced; it must cover vector_count rows.
 * NULL workspace ignored. No allocation/I/O/errors. */
void vector_workspace_exclude(tl_vector_workspace *workspace, const uint64_t *excluded);
/** Search every row by cosine of normalized floats, best first, ties by id.
 * capacity is 1..VECTOR_MAX_RESULTS; queries/dimensions/emb_gen must match this
 * workspace's index. Copy at most capacity results to caller-owned results;
 * out_count zero on error. TL_INVALID/STATE for contracts/invalid vectors.
 * No I/O or heap allocation; scratch remains reusable after failures. Output
 * size changes never affect the ordering of retained results. */
tl_status vector_query(const tl_vector *index, tl_vector_workspace *workspace, uint64_t emb_gen,
                       const float *query, size_t dimensions, tl_vector_result *results,
                       size_t capacity, size_t *out_count);
/** Read row count; zero for NULL, no ownership transfer or errors. */
size_t vector_count(const tl_vector *index);
/** Read the index's immutable emb_gen; zero for NULL, no ownership/errors. */
uint64_t vector_emb_gen(const tl_vector *index);
/** Read reserved index bytes (excludes workspaces/allocator); zero for NULL. */
size_t vector_bytes(const tl_vector *index);
#endif
