# M3 Part 2 implementation and verification

Recorded 5 October 2026, Intel Core i7-8700K (12 logical CPUs), Linux 6.8.0,
C17/O3 release measurements. Design: [ADR 0020](adr/0020-m3-part2-search-quality.md).

M3 Part 2 is implemented: independent primary-name, generic-name, keyword and
folder evidence; complete-token scoring; indexed unfinished one-edit retrieval;
and optimal fuzzy alignment with a bounded greedy fallback. Desktop name bonuses
and exact-name/path priority remain. No dependencies, SQLite migrations or IPC
schema changes were needed. M4/M5 remain planned.

## Acceptance and correctness

`chrome` ranks Google Chrome ahead of 24 competing Chrome-prefixed files in the
mixed fixture. `proej` finds `projectNotes.md` in an isolated one-file unit
fixture. Desktop tests exercise unfinished name typos and words split between
generic-name and keyword fields. Query allocation interposition covers the new
prefix-edit and field paths as well as active scoring workers.

2048 small fuzzy cases agree with an independent exhaustive alignment oracle;
optimal scores never underperform greedy scores. Tests also cover later better
alignment, capped gaps and the >512-symbol greedy fallback. 729 prefix-edit
query variants agree with an independent full OSA distance matrix. Existing
exact-name/path, normalized Unicode/raw-byte, large-worker, warm/cold query,
cache-eviction and top-k consistency regressions continue to pass. The analytic
large-corpus score test now includes the intentional complete-token bonus.

`make test` passes ASan/UBSan units, allocation checks, CLI, desktop replacement
and daemon integration. `make lint` passes clang-tidy/cppcheck. AGENTS.md and
CLAUDE.md remain identical. `make bench` ran at 50k and 500k paths. The labeled
evaluator additionally checks fresh-workspace and result-limit equivalence for
every mixed query.

## Labeled relevance

[Fixture/evaluator](../tests/quality/README.md): 68 entries, 44 queries, 22 per
split. Each split includes one no-match query; those are excluded from relevance
averages and scored separately for empty-result correctness. Query variants stay
in the same target family/split. Ambiguous alternatives receive graded labels.
These are curated realistic launcher queries and controlled alignment examples,
not a personal usage log or a representative sample of all launcher searches.

Baseline: revision `7ace90f0a3d1474a0594b08ee8db9fde72e0f713`, already including
ADR 0018. The [JSON evidence](../tests/bench/results/2026-10-05-m3-part2-quality.json)
records every query and per-class metric for baseline, improved lexical and BM25.
The fixture fits within the 1000-result capacity, so candidate recall covers all
matching membership, without a ranked truncation proxy.

| Held-out measure | Previous engine | Improved lexical | Exact-token field BM25 |
|---|---:|---:|---:|
| Candidate recall | 0.952 | 1.000 | 0.683 |
| First-useful MRR | 0.952 | 1.000 | 0.714 |
| Top-ten success | 0.952 | 1.000 | 0.714 |
| Graded nDCG@10 | 0.939 | 1.000 | 0.657 |
| No-match correctness | 1.000 | 1.000 | 1.000 |

Unfinished-typo recall/top-ten success improves from 2/3 to 3/3 on held-out
queries. The held-out alignment example's nDCG rises from 0.710 to 1.000 while
retaining both reasonable results. Other labeled classes preserve their useful
results. First-useful rank is 1 on every improved held-out relevant query; a
missing useful result is represented by rank 1001/MRR 0 in the artifact.
BM25 has no prefix/edit/fuzzy expansion and therefore misses those query classes;
this does not establish that BM25 with equivalent expansion would be inferior.

## Latency, memory and larger-corpus relevance

The [previous-engine run](../tests/bench/results/2026-10-05-m3-part2-baseline.txt)
and [current run](../tests/bench/results/2026-10-05-m3-part2-lexical.txt) use the
same seeded synthetic corpora and 1200 held-out queries per size. All nine sanity
fixtures pass at both sizes. Results below are engine measurements, excluding
IPC/GUI launch and SQLite loading. The raw artifacts also include p50/p99/max,
per-class ranking and build time.

| Paths | Previous typing p95 | Current typing p95 | Previous whole-query p95 | Current whole-query p95 | Fresh-workspace p95 |
|---|---:|---:|---:|---:|---:|
| 50k | 0.671 ms | 1.162 ms | 0.831 ms | 1.472 ms | 0.919 ms |
| 500k | 8.035 ms | 9.226 ms | 7.356 ms | 10.275 ms | 14.194 ms |

Fresh-workspace measurements sample every twentieth held-out query (60 per
size), exclude workspace allocation/worker startup, and reset membership/evidence
caches. They are not cold disk/OS-cache timings and differ in sampling from the
1200 warm whole-query measurements. No benchmark competes with the sanitizer or
lint runs; timing noise still applies to these single runs.

| Paths | Engine/workspace RSS increase, previous/current | Current total steady RSS | Process peak RSS, previous/current |
|---|---:|---:|---:|
| 50k | 30168 / 30280 KiB | 40080 KiB | 46440 / 46520 KiB |
| 500k | 255852 / 255904 KiB | 334696 KiB | 398944 / 399040 KiB |

The engine/workspace RSS increase subtracts resident memory before building.
Total steady process RSS is also reported in the current artifact; it includes
the benchmark's retained input corpus and query fixtures. Peak includes private
construction buffers and workspace/catalog transitions. Reusing sorted terms
avoids a large new persistent trie or prefix-deletion table.

Synthetic held-out top-ten success rises 0.615→0.621 at 50k and 0.531→0.539 at
500k; ranked candidate recall@1000 rises 0.920→0.978 and 0.825→0.853. For unfinished
typos specifically, recall@1000 rises 0.570→0.960 and 0.190→0.335. These are
known-item ranking proxies: random targets among many similar synthetic names
still produce poor top-ten typo quality. They are not complete pre-ranking
candidate recall and do not replace the smaller graded mixed fixture.

## Limits and next work

Optimal alignment and complete prefix-edit expansion increase query work.
The 500k whole-query tail regresses relative to the frozen baseline, despite
improved relevance and almost unchanged memory. This milestone does not claim
that the original 5 ms target is met; latency and full-engine rebuild optimization
remain separate tracked work. Broader independent labeling and more ambiguous
application/file catalogs are needed before making general relevance claims.

Prefix edits apply to nonnumeric 5–32-symbol queries over 3–32-symbol tokens;
complete-token typos retain the three-symbol minimum. Longer tokens and directory
name typos use other channels. Optimal fuzzy scoring applies up to 512 text
symbols; larger inputs keep greedy scoring. Auxiliary fuzzy evidence scans the
bounded desktop fields and should be measured before using this API for a large
metadata-heavy catalog. M4 can now compare semantic retrieval against this
improved lexical baseline; M5 will evaluate optional personalization afterward.

## First-token completeness review fix

[ADR 0021](adr/0021-first-token-completeness.md) fixes completion evidence being
overridden by a whole-basename prefix. `project` now ranks `project notes.txt`
(6175) ahead of `projectile.txt` (6050). The first token keeps basename strength;
only its complete match gains 128, with no additional index keys or query buffers.
Generic-name/keyword completion retains its existing field weights.

The new prefix regression fails on the original Part 2 implementation and passes
with the fix. Ranking regressions cover spaces, underscores, camelCase, acronym
boundaries, case folding and the one-symbol cache, including single-result and
larger capacities and exact-name priority. Existing worker/evidence tests now
include the completed first token in their independent expected scores.

`make test` passes sanitizer units, allocation checks and CLI/desktop/daemon
integration; `make lint` passes. AGENTS.md and CLAUDE.md remain identical. The
mixed evaluator retains all 44 recorded query rankings and metrics, including
held-out recall and nDCG@10 of 1.000. Its small-fixture limits still apply.

`make bench` passes all nine fixtures at both sizes. The
[review-fix run](../tests/bench/results/2026-10-05-m3-part2-token-completeness.txt)
records the same seeded synthetic corpora without overlapping test/lint runs:

| Paths | Typing p95 | Whole-query p95 | Total steady RSS | Process peak RSS |
|---|---:|---:|---:|---:|
| 50k | 1.185 ms | 1.568 ms | 40016 KiB | 46520 KiB |
| 500k | 8.393 ms | 9.447 ms | 334760 KiB | 399204 KiB |

The completion preference changes ambiguous synthetic rankings: at 50k, held-out
parent-query top-ten success changes from 0.645 to 0.560 and overall success from
0.621 to 0.607; prefix Recall@1 rises from 0.240 to 0.245. At 500k, overall top-ten
success remains 0.539. These known-item proxies label a random target among many
similar names; the fix deliberately favors complete first tokens. The graded
mixed-catalog results remain unchanged. Timing differences between single runs
are noisy, and the original 5 ms target remains unmet.
