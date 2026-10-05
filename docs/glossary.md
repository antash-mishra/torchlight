# Glossary

| Term | Meaning |
|---|---|
| **Path entry** | A file or directory known to the index (`files` row). |
| **Filesystem incarnation** | Device/inode plus birth timestamp identifying the observed object at a path; ctime is the conservative fallback without birth time. A changed incarnation retires the old file id and descendants during reconciliation. |
| **Trigram** | A 3-byte substring of a normalized name, used as an index key. |
| **Posting list** | File ids containing a given trigram. |
| **Lexical channel** | One candidate source: `prefix`, `trigram`, `subseq` or `typo`. `lexical` merges them. |
| **Prefix bonus** | Bounded caller-selected addition to each basename/token/initials prefix hit, fixed before sealing. The desktop engine uses it for name relevance; file engines default to zero. |
| **Prefix edit matching** | Comparing a query word to the beginning of an indexed word with a small edit budget; the untyped suffix does not count as an error. Implemented with one-edit sorted-range traversal for five-symbol or longer unfinished words. |
| **Hybrid retrieval** | Combining lexical and semantic result lists with explicit exact-name/path priority. The M4 fusion module is implemented; resident integration is pending. |
| **BM25** | A lexical relevance formula using word occurrence, word rarity and document length. Implemented as an offline exact-token field comparison; the resident engine uses lexical channels. |
| **Implicit token trie** | Lexicographically sorted distinct tokens whose shared prefixes form contiguous ranges; bounded edit rows traverse those ranges without storing additional trie nodes. |
| **Token completeness** | Evidence that a query word consumes an entire indexed token rather than just its beginning; a bounded ranking bonus. |
| **Optimal fuzzy alignment** | Highest-scoring ordered character placement under boundary, consecutive and gap rules; bounded DP replaces earliest-placement scoring for text up to 512 symbols. |
| **FST** | Finite-state transducer: a compact dictionary representation usable for token/prefix lookup. A proposed alternative for indexed prefix typo traversal. |
| **Character mask** | Conservative 64-bit hash of matching symbols; collisions add candidates, never false negatives. |
| **Mask bitmap index** | Resident bitmaps of rows containing each mask bit; intersect required bits and union other complete match sources before scoring. |
| **Incremental narrowing** | Re-checking complete subsequence membership for a normalized query extension with unchanged rules and `catalog_gen`; never narrowing from top-k. |
| **Deletion neighbourhood** | A string plus all its one-character deletions; indexed by `typo` for one-edit lookups. |
| **Candidate set** | Deduplicated ids from exact/prefix, trigram, character/subsequence, and edit fallback channels. |
| **Subsequence** | Query characters occurring in order, with gaps allowed; supports abbreviations. |
| **Edit distance** | Number of insertions, deletions, and substitutions between strings; used for bounded typo matching. |
| **Fuzzy score** | fzf/fzy-style subsequence match quality against a path; edit scoring handles other typos separately. |
| **Embedding** | A fixed-size float vector representing a path's meaning. |
| **Prepared embedding text** | Versioned UTF-8 input for a model, derived from names/context/metadata. Raw byte paths retain their separate exact identity for matching and actions. |
| **Cosine reference** | Exhaustive search of L2-normalized float embeddings by dot product, used to evaluate model relevance and reduced/quantized retrieval loss. |
| **Exact-match tier** | Explicit ranking priority: an exact path precedes an exact basename, and both precede ordinary hybrid scores. |
| **Binary quantization** | Keeping only the sign bit of each embedding dimension, so vectors can be compared with XOR + popcount. |
| **Rescoring** | Re-ranking the top binary hits with more precise int8 vectors. |
| **RRF** | Reciprocal Rank Fusion: merges ranked lists via `Σ 1/(k + rank)`. |
| **Frecency** | A score combining how frequently and how recently a path was opened. |
| **Open** | An accepted launch request, optionally logged to `opens`; does not confirm external application success. |
| **Catalog generation (`catalog_gen`)** | An immutable, consistent resident view of paths, indexes, vectors, and usage summaries pinned by queries. |
| **Reader lease** | Exclusive preallocated query workspace that pins one immutable catalog view until release. |
| **Retired snapshot** | A catalog view replaced by publication, retained until all reader leases release it and background reclamation frees it. |
| **Embedding generation (`emb_gen`)** | Vectors sharing a model revision, tokenizer/preprocessing, dimension, and quantization format. |
| **Generation** | Never used alone in code or docs: always `catalog_gen` or `emb_gen`. |
| **Two-phase response** | A `lexical` result set sent immediately, followed by a fused `final` set for the same request. |
| **Terminal response** | `final` results or a completion error ending an active request, including lexical fallback on semantic failure/deadline. |
| **Resolve** | Validate a result's file id against the current catalog and retrieve its exact current path before launch. |
| **Launch-event id** | Unique id preventing duplicate history records when recording a launch is retried. |
| **Display string** | Valid-UTF-8 rendering of a path (invalid bytes → U+FFFD). Never used to open files. |
| **Directory tree (`dirtree`)** | Every distinct parent directory stored once with its parent link and normalized name, so parent context is matched per directory, not per file. |
| **Usable directory** | A directory that may serve as parent context: anything except the directories above every indexed root (e.g. `/home` for root `/home/user`). |
| **Context mask** | Per-entry conservative mask of its basename and all ancestor directory names; a word with a symbol outside it can only match through a channel hit. |
| **Repeat mask** | Per-entry mask of symbols occurring at least twice, so a word with a repeated symbol (`apps`) skips names that cannot contain it twice. |
| **Strong-hit skip** | Answering from fully evaluated channel hits when the worst kept score beats the proven maximum of every remaining entry; supports single- and multiword queries without truncating candidates. |
| **Word evidence cache** | One of four bounded per-query caches of complete channel hits and nearest matching parent scores for a word. Query epochs prevent reuse after text changes; eviction changes work only. |
| **Parallel scoring batch** | Disjoint entry ranges scored by prestarted workspace workers against immutable directory context; the coordinator records complete membership and orders results. |
| **Deferred candidates** | Multiword candidates matching the first word only through a parent directory; scored only if the results so far cannot already rule them out. |
| **Symbol cache** | Results for the 36 one-symbol queries `a`–`z`/`0`–`9`, computed when an engine is sealed. |
| **Allowlist (`allow`)** | Configured hidden or ignored directories that are indexed anyway; their hidden ancestors are traversed but not indexed. |
| **Configuration sync** | `torchlight index` without roots: scan configured roots, keep unavailable ones, and forget roots no longer configured. |
| **Known-item query** | A benchmark query generated from one target entry (exact, prefix, abbreviation, typo, partial typo or parent + name) and judged by whether that entry is retrieved. |
| **Kept scope** | A path whose saved entries (itself and everything below it) a scan must not prune: an unreadable path, or a registered root nested in the scanned root that the scan did not visit. |
| **Reconciliation** | A successful filesystem scan used to repair the catalog after missed events, startup, or watch exhaustion. |
| **Recovering** | A catalog batch committed but could not publish; the writer must reload committed SQLite before accepting later catalog batches. |
| **Move cookie** | inotify identifier pairing a source and destination rename event so catalog ids can survive the move. |
| **Singleton lock** | Persistent adjacent advisory lock file preventing duplicate daemons or an offline index writer from racing a daemon. |
| **History queue** | Bounded FIFO of copied search/open/clear events written off the query thread; saturation increments a drop counter. |
| **Candidate recall** | Fraction of labeled targets surviving retrieval before final scoring. |
| **Recall@10** | Fraction of relevant labeled results returned in the first ten results. |
| **nDCG@10** | Normalized discounted cumulative gain over ten results: compares graded relevance and position against the ideal order, giving earlier useful results more weight. |
| **p95 latency** | Query duration at or below which 95% of measured requests finish. |
| **Arena** | Bump allocator freed all at once. Used for per-query temporaries. |
| **ADR** | Architecture Decision Record, in `docs/adr/`. |
| **Opaque byte symbol** | Value above the Unicode scalar range identifying one malformed path byte, preserving matching identity. |
| **Grapheme byte offset** | Original start byte of the grapheme cluster contributing a normalized symbol; folded expansions share the offset. |
| **Sealed engine** | M1 catalog/index builder after finish, immutable until destruction; workspaces borrow its lifetime. |
| **Query workspace** | Bounded scratch allocated before querying and bound to one sealed engine, independently reusable per caller. |

- **Desktop id:** XDG application identity derived from its relative `.desktop`
  path, replacing directory separators with hyphens; user entries mask system
  entries with the same id.
- **Desktop result id:** Session-scoped numeric IPC identity for an unchanged,
  visible desktop entry. Changes/removal/restart invalidate it; it is distinct
  from a durable SQLite file id.
- **Desktop revision:** Fingerprint of the selected desktop filename and canonical
  keyfile content, checked before native activation to reject replacement entries.

- **Static embedding:** A trained token lookup table pooled into a text vector;
  Potion uses this without a contextual transformer runtime.
- **Prepared-text cache:** Versioned background storage keyed by exact semantic
  input and emb_gen, allowing unchanged metadata to reuse validated vectors.
- **Sign-bit shortlist:** Experimental Hamming candidate selection before int8
  cosine rescoring; recall must pass evaluation before it becomes a default.
- **desktop_gen:** Session-local immutable desktop metadata sequence, separate
  from catalog_gen and emb_gen; required for coherent hybrid publication.
