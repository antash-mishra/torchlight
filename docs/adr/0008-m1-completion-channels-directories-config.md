# 0008. M1 completion: channels, directory interning, bounded scans and configuration

- **Status:** Accepted
- **Date:** 2026-10-02
- **Refines:** 0004, 0005, 0006 and 0007

## Context

After the first M1 increment, the remaining M1 work was trigram and typo
retrieval with edit scoring, strict parent matching and ranking evaluation,
complete subsequence narrowing, configuration with hidden allowlists and root
deduplication, and latency/memory work on representative corpora. The baseline
scanned every entry with fuzzy matching on every keystroke and stored every
parent token for every file: 709 MB and about 94 ms p95 at 500k synthetic paths.

## Decision

**Retrieval channels**
- Trigram keys are three normalized symbols packed exactly into 63 bits, rather
  than hashed UTF-8 byte trigrams: no collisions, and the same symbols as the
  subsequence and edit checks. A name qualifies when it shares at least half of
  the query's informative trigrams. Trigrams in more than 1/8 of names are
  ignored, and queries need at least three distinct trigrams.
- The typo dictionary holds distinct basename *tokens* of 3–32 symbols, not
  whole basenames or stems, and excludes all-digit tokens. A query word is
  matched as a whole. Optimal string alignment counts an adjacent swap as one
  edit, so a one-edit budget catches the most common typo.
- Typos of *incomplete* words remain a later extension (as the typo design
  already said); the trigram channel covers some of them.

**Parent context and ranking**
- Directories are interned once (`dirtree`) and scored lazily, at most once per
  query word. A word matches a parent through one directory name: a key prefix
  or a subsequence within the name, taking the nearest matching ancestor.
  Directories strictly above every root are not context; a root's own name is.
- Score tiers: basename prefix keys 4500–6000 > one-edit token typo 3500 >
  parent directory key prefix 2500 > basename subsequence 1500 + fuzzy >
  trigram 1000–2000 > parent subsequence (fuzzy). The parent prefix tier was
  raised from 1000 (and the basename subsequence bonus lowered from 2000)
  because a folder named exactly like a word is stronger evidence than letters
  scattered through a basename (`finance invoice` → `finance/invoice2024.pdf`,
  not `finalInvoice.js`). Basename *key* matches still outrank every parent
  match, as PLAN.md asks. Shorter basenames win ties by up to 63 points.

**Bounded, exact query work**
- Entries are stored as columns. Per-entry context and repeat masks reject
  impossible entries cheaply. Hit scores that beat `fuzzy_score_bound` skip
  fuzzy work.
- Single-word queries skip the scan when channel hits alone fill the results
  above every possible subsequence score. Multiword queries defer parent-only
  candidates of the first word and drop them when the heap already beats their
  maximum total. Both are proofs, not heuristics: the tests compare small and
  large capacities.
- One-symbol queries over `a`–`z`/`0`–`9` are precomputed when the engine is
  sealed (36 × up to 1000 results).
- Narrowing reuses the complete subsequence membership of a word that the new
  word extends, under the same `/` mode. Channel hits are always looked up,
  because they are not monotone.

**Configuration and crawling**
- A small `key = value` file (`root`, `allow`) under the XDG configuration
  directory; no new dependency. `~/` expands with HOME; there are no inline
  comments.
- Allowlisted directories are indexed; their hidden or ignored ancestors are
  traversed but not indexed, following only paths that lead to an allowlisted
  directory. The state directory exclusion always wins.
- `index` without roots syncs to the configuration in one transaction. Roots
  that are repeated or covered by another root are scanned once. Unavailable
  roots keep their entries. Unconfigured registered roots, including ad-hoc
  ones, are forgotten. The run fails unchanged when no root can be scanned.

**Evaluation**
- `make bench` uses a deterministic home-folder-like synthetic corpus and,
  optionally, a NUL-separated real path list (`scripts/make_corpus.sh`).
  Known-item queries of six kinds are generated with a tuning seed and a
  separate held-out seed. Latency is measured per keystroke while typing and
  per whole query.

## Alternatives considered

- Hashed byte trigrams with collision checks: more code for no benefit at this
  key size.
- Indexing whole basenames and stems in the typo dictionary: several times the
  memory on real corpora (mostly unique names), with trigrams covering
  multi-token typos anyway.
- Matching parents per finer token instead of per directory name: loses
  abbreviations of directory names (`prjnts` for `projectNotes/`).
- Keeping the 1000 parent tier: measured worse on parent queries (real held-out
  Recall@10 0.355 vs 0.475) and failed the `finance invoice` case.
- Character posting lists or approximate top-k: rejected by 0004/0005 for
  recall reasons; the bounds above keep results exact.
- Intra-query threads: outside M1's single-threaded CLI. A candidate for the M2
  daemon if p95 remains above target on unloaded hardware.

## Consequences

Memory at 500k drops by about two thirds, and most keystrokes take well under a
millisecond. Typo recall rises sharply. The remaining latency cost is
concentrated in the first keystroke of a word that cannot be narrowed or
skipped, especially second words whose first word matches a very large share of
the corpus through parent directories. The p95 gate is measured honestly in
[evaluation](../evaluation.md), including the reference machine's load. Ad-hoc
roots are not durable across configuration syncs; this is documented for users.
