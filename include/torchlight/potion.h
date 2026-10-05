/* Native static Model2Vec inference with checked, owned model tables. */
#ifndef TORCHLIGHT_POTION_H
#define TORCHLIGHT_POTION_H
#include "torchlight/embed.h"

#define POTION_MODEL_BYTES (256U * 1024U * 1024U)
/** Load a TLSTAT01 model exported by scripts/export_potion.py into owned RAM.
 * No mappings or runtime downloads. Validate sizes, vocabulary, finite weights
 * and SHA256 payload; budget includes the payload (not hash-map overhead).
 * Accept regular files, including symlinks to them; reject FIFOs, directories
 * and devices with TL_IO without waiting for a stream producer.
 * out NULL on failure; TL_INVALID/IO/NOMEM/LIMIT/STATE. Returned adapter is
 * caller-owned and serialized; free with embedder_destroy. emb_gen derives
 * from the complete model payload and explicit preprocessing/backend versions.
 * Encoding performs no allocation or I/O. */
tl_status potion_load(const char *filename, size_t budget_bytes, tl_embedder **out);
/** Prepare raw filename/path bytes as bounded UTF-8 semantic text. Keep the
 * basename and two nearest parents; turn separators/camel-case boundaries into
 * spaces, replace malformed bytes with spaces. Input/output borrowed and must
 * not overlap. TL_INVALID/LIMIT; out empty on failure. Raw identity is untouched.
 * Algorithm version launcher-text-1; no allocation or I/O. */
tl_status potion_prepare_path(const char *path, char *out, size_t capacity);
#endif
