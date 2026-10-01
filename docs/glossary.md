# Glossary

| Term | Meaning |
|---|---|
| **Path entry** | A file or directory known to the index (`files` row). |
| **Trigram** | A 3-byte substring of a normalized name, used as an index key. |
| **Posting list** | File ids containing a given trigram. |
| **Lexical channel** | One candidate source: `prefix`, `trigram`, `subseq` or `typo`. `lexical` merges them. |
| **Character mask** | Conservative 64-bit hash of matching symbols; collisions add candidates, never false negatives. |
| **Incremental narrowing** | Re-checking complete subsequence membership for a normalized query extension with unchanged rules and `catalog_gen`; never narrowing from top-k. |
| **Deletion neighbourhood** | A string plus all its one-character deletions; indexed by `typo` for one-edit lookups. |
| **Candidate set** | Deduplicated ids from exact/prefix, trigram, character/subsequence, and edit fallback channels. |
| **Subsequence** | Query characters occurring in order, with gaps allowed; supports abbreviations. |
| **Edit distance** | Number of insertions, deletions, and substitutions between strings; used for bounded typo matching. |
| **Fuzzy score** | fzf/fzy-style subsequence match quality against a path; edit scoring handles other typos separately. |
| **Embedding** | A fixed-size float vector representing a path's meaning. |
| **Binary quantization** | Keeping only the sign bit of each embedding dimension, so vectors can be compared with XOR + popcount. |
| **Rescoring** | Re-ranking the top binary hits with more precise int8 vectors. |
| **RRF** | Reciprocal Rank Fusion: merges ranked lists via `Σ 1/(k + rank)`. |
| **Frecency** | A score combining how frequently and how recently a path was opened. |
| **Open** | An accepted launch request, optionally logged to `opens`; does not confirm external application success. |
| **Catalog generation (`catalog_gen`)** | An immutable, consistent resident view of paths, indexes, vectors, and usage summaries pinned by queries. |
| **Embedding generation (`emb_gen`)** | Vectors sharing a model revision, tokenizer/preprocessing, dimension, and quantization format. |
| **Generation** | Never used alone in code or docs: always `catalog_gen` or `emb_gen`. |
| **Two-phase response** | A `lexical` result set sent immediately, followed by a fused `final` set for the same request. |
| **Terminal response** | `final` results or a completion error ending an active request, including lexical fallback on semantic failure/deadline. |
| **Resolve** | Validate a result's file id against the current catalog and retrieve its exact current path before launch. |
| **Launch-event id** | Unique id preventing duplicate history records when recording a launch is retried. |
| **Display string** | Valid-UTF-8 rendering of a path (invalid bytes → U+FFFD). Never used to open files. |
| **Kept scope** | A path whose saved entries (itself and everything below it) a scan must not prune: an unreadable path, or a registered root nested in the scanned root that the scan did not visit. |
| **Reconciliation** | A successful filesystem scan used to repair the catalog after missed events, startup, or watch exhaustion. |
| **Candidate recall** | Fraction of labeled targets surviving retrieval before final scoring. |
| **Recall@10** | Fraction of relevant labeled results returned in the first ten results. |
| **p95 latency** | Query duration at or below which 95% of measured requests finish. |
| **Arena** | Bump allocator freed all at once. Used for per-query temporaries. |
| **ADR** | Architecture Decision Record, in `docs/adr/`. |
| **Opaque byte symbol** | Value above the Unicode scalar range identifying one malformed path byte, preserving matching identity. |
| **Grapheme byte offset** | Original start byte of the grapheme cluster contributing a normalized symbol; folded expansions share the offset. |
| **Sealed engine** | M1 catalog/index builder after finish, immutable until destruction; workspaces borrow its lifetime. |
| **Query workspace** | Bounded scratch allocated before querying and bound to one sealed engine, independently reusable per caller. |
