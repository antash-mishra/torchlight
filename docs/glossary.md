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
| **Hybrid retrieval** | Combining lexical and semantic result lists with explicit exact-name/path priority. M4 integrates native Potion and RRF through opt-in resident two-phase search. |
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
| **Rescoring** | Re-ranking shortlisted rows with the full int8 vectors (after a sign-bit or prefix first pass). |
| **Nested prefixes** | A model property (`tl_emb_model.nested_prefixes`): the leading components of an embedding are themselves a usable lower-dimensional embedding, as with Matryoshka training. Not part of `emb_gen`. |
| **Prefix shortlist** | Two-pass vector search: rank every row by int8 cosine over its first components (here 128), keep the best rows (here 8000), then rescore those with the full int8 cosine. Approximate top-k; rescored cosines equal the exhaustive ones (ADR 0031). |
| **RRF** | Reciprocal Rank Fusion: merges ranked lists via `Σ 1/(k + rank)`. |
| **Frecency** | A score combining how frequently and how recently a path was opened. |
| **Open** | An accepted launch request, optionally logged to `opens`; does not confirm external application success. |
| **Catalog generation (`catalog_gen`)** | An immutable, consistent resident view of paths, indexes and vectors pinned by queries. Usage summaries are separate (ADR 0033). |
| **Usage summary** | Capped resident frecency and query-to-open data, owned in plain memory by the search thread and rebuilt from retained history at startup by the persistence thread (M5, ADR 0033). |
| **Query-to-open history** | Pairs of a normalized typed query and the target opened from its results; a later query boosts the targets of stored queries that start with it (M5). |
| **Used-set side query** | M5 ranking: the normal lexical query excludes entries with usage, a second query scores only those entries and adds their boosts, and the lists merge into the exact personalized top k without changing pruning bounds (ADR 0033). Implemented as the *side pass* pushing into the same result heap. |
| **Sparse evidence** | Word evidence gathered for a few entries only (their name and field keys, their parent folders' keys on demand), used by the side pass for one-symbol queries answered from the seal-time cache; dropped when the query ends. |
| **Reader lease** | Exclusive preallocated query workspace that pins one immutable catalog view until release. |
| **Retired snapshot** | A catalog view replaced by publication, retained until all reader leases release it and background reclamation frees it. |
| **Embedding generation (`emb_gen`)** | Vectors sharing a model revision, tokenizer/preprocessing, dimension, and quantization format. |
| **Generation** | Never used alone in code or docs: always `catalog_gen` or `emb_gen`. |
| **Newest-pending slot** | Of a client's bounded request queue, only the newest query is searched in full; a newer query marks the older queued ones superseded and cancels a running one, and they answer as cancelled (M6 step 1). |
| **Persistence thread** | The writer thread that owns the SQLite write connection and applies catalog batches, history and retention; the indexing thread crawls and builds engines without holding a write transaction (M6 step 1). |
| **Base segment / delta segment** | M6 step 3 engine layout: the large immutable engine shared by every snapshot derived from it (with its reader workspaces), plus a small engine holding all entries changed since, queried together and merged in one order. |
| **Tombstone** | A base position hidden by a derived snapshot because its entry was changed (its new version lives in the delta) or removed. |
| **Reference engine** | The base a delta segment is built against (`lexical_set_reference`): its roots bound parent context and its trigram index decides frequent trigrams, so an entry scores the same in either segment. |
| **Change set** | The ids of files rows one committed catalog transaction inserted, updated or deleted, recorded by the store's SQLite update hook; the input of a delta publication. |
| **Compaction** | Full rebuild of a new base segment from the committed catalog once the delta exceeds max(4096, base/32) entries (or the change set is incomplete); runs on the indexing thread while queries continue on the old view, and publishes like any other `catalog_gen`. |
| **Scoped reconcile** | Crawling and upserting only the directories named by coalesced watch events, instead of every root (M6 step 3a). |
| **Event scope** | A directory to rescan after watch events: its direct children (the parent of a changed entry), or its whole subtree when it appeared with unseen contents. |
| **Fuzzy matcher** | A word compiled for Frizbee's SIMD Smith-Waterman (`fuzzy_matcher_create`), weighted to score on the portable scorer's scale; one per word and scoring thread (M6 step 2). |
| **Word evidence cache** | Per-workspace cache of one word's channel hits, directory scores and matchers, keyed by the word's symbols and reused across queries (M6 step 2). |
| **Incremental semantic stage** | A semantic snapshot built by copying the previous snapshot's vector for every row whose prepared text is unchanged, embedding only the rest (M6 step 3c). |
| **Derived semantic snapshot** | A semantic snapshot that shares a full snapshot's rows and vectors by reference, owns only rows changed since it, and hides the base rows they replace or remove; the semantic counterpart of a delta segment (M6 step 3c). |
| **Two-phase response** | A `lexical` result set sent immediately, followed by a fused `final` set for the same request. |
| **Terminal response** | `final` results or a completion error ending an active request, including lexical fallback on semantic failure/deadline. |
| **Response backpressure** | Keeping a semantic final pending while earlier socket output drains, so individually valid phases share a bounded client queue. |
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
- **Cache write contention:** A cache transaction cannot acquire SQLite's writer
  lock while reconciliation or another writer holds it. Unstarted cache batches
  retry without losing the partial resident embedding build.
- **Sign-bit shortlist:** Experimental Hamming candidate selection before int8
  cosine rescoring; recall must pass evaluation before it becomes a default.
- **desktop_gen:** Session-local immutable desktop metadata sequence, separate
  from catalog_gen and emb_gen; required for coherent hybrid publication.
- **Distinct prefix:** The leading part of a result's parent path, through the
  first folder where it differs from another visible result of the same kind and
  name. The popup keeps it visible when shortening paths.
- **Selection track:** The popup widget that draws one selection highlight beneath
  the result list and glides it between rows on the frame clock.
- **Torch sweep:** The popup's opening effect: a band of accent light crosses the
  search field once each time the popup is shown.
