# Mixed launcher relevance evaluation

`mixed.json` contains realistic application/settings names, noisy competing
files, incomplete edits, abbreviations, duplicate names, folder/name queries,
extensions, normalized Unicode, malformed bytes and no-match queries. Grades
are 3 (preferred) and 1 (reasonable alternative). Target families and their
variants stay together in the explicit tuning/held-out split. The two alignment
families are controlled comparisons rather than real application names.

The native evaluator merges separate desktop and file engines as the daemon does:
for a one-word query it keeps only word-start matches once any exists, then lists
exact paths, the applications the query names, exact names, the applications tagged
with its words and the rest by score (ADR 0058, ADR 0061). Libraries from before
`lexical_word_start` are merged as the daemon merged them then: exact paths, named
applications, then the rest by score, or by score alone before `desktop_named`. It verifies fresh-workspace and top-k equivalence. The
fixture has fewer than 1000 entries, so returned membership is complete, making
candidate recall exact here. Large synthetic benchmark recall@1000 is a ranked
proxy, not complete pre-ranking recall. Missing useful results have first rank
1001 and MRR 0; no-match queries have a separate empty-result accuracy measure
and do not enter relevance averages.

BM25 uses exact case-folded NFC word tokens, separate fields, k1=1.2, b=0.75,
and weights name/generic/keyword/parent = 3/1.5/1/0.5. It deliberately has no
prefix, typo or fuzzy expansion. It is a comparison, not a new runtime dependency.

Build the native library with `make build/quality_engine.so`, then run:

```sh
python3 tests/quality/evaluate.py \
  --baseline /tmp/torchlight-m3p2-baseline.so \
  --current build/quality_engine.so \
  --output /tmp/search-quality.json
```

The recorded baseline is revision `7ace90f0a3d1474a0594b08ee8db9fde72e0f713`.
To reproduce it, export that revision into a temporary directory using
`git archive`, copy the current Makefile (which adds the shared-library target)
there, and run `make build/quality_engine.so` in that directory. Supply the same
machine.mk or dependency overrides as the current build when needed. Save that
library as the baseline path above; no tracked source needs modification.

The evaluator fails unless held-out nDCG and candidate recall improve. Keep the
baseline frozen; do not move query variants between splits after measuring them.
Use a larger independently labeled corpus before drawing broad quality claims.

## M4 English semantic fixture

`semantic.json` contains 36 items and 52 queries with target-family splits.
`scripts/evaluate_semantic.py` compares local pinned Potion dimensions/fusion;
`check_potion_parity.py` checks C/reference inference and int8 neighbors;
`evaluate_semantic_daemon.py` evaluates the actual two-phase resident path.
See [model evaluation](../../docs/m4-model-evaluation.md). Normal tests do not
require model weights. Private Documents names are not committed.

## M8 place ranking on a real path list

`place_eval.py` indexes a NUL-separated absolute path list (the
`BENCH_PATHS` format; the common directory is the indexed root) with each
given engine library, as the daemon's file catalog does, runs 50 everyday
queries and counts the top-10 results inside third-party trees and inside
the person's own folders. Both are labeled by path substrings on the command
line, independent of the engine's own dependency markers:

```sh
python3 tests/quality/place_eval.py --paths home.paths \
  --library old/quality_engine.so --library build/quality_engine.so \
  --third-party /Android/Sdk/ --third-party /go/pkg/ --own /workspace/ \
  --show sc,scripts --output /tmp/place.json
```

A library without `lexical_set_place_ranking` (before M8) ranks without it.
Path lists of a real home folder are private; commit only aggregate counts.
