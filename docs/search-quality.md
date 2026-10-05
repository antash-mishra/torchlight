# Search quality review

Reviewed 2026-10-05. The application-name weighting and tie-order fixes are
implemented in [ADR 0018](adr/0018-application-name-ranking.md). The wider work
is now planned as M3 Part 2 search quality, M4 hybrid semantic search and M5
personal recommendations in [the plan](../PLAN.md) and
[ADR 0019](adr/0019-search-quality-before-semantic-personalization.md). M3 Part 2 is now implemented in
[ADR 0020](adr/0020-m3-part2-search-quality.md);
[its verification](m3-part2-completion.md) records held-out relevance and latency
tradeoffs. M4's [initial C foundation](m4-implementation.md) is implemented;
trained-model choice, production retrieval and M5 still require evaluation.

For Torchlight, the recommended direction is hybrid retrieval: indexed name
search with better autocomplete and ranking, plus optional local semantic
search. No published benchmark establishes a universally best method for our
filenames, installed applications and hardware.

## What the current failures tell us

On the running catalog, `chrome` returned Google Chrome at position 25; `google
chrome` put it first. Discovery worked. Mixed ranking hid the application behind
filename prefixes. The new name weighting addresses that class of failure.

In a separate temporary catalog containing `projectNotes.md`, `project` and
`proejct` found the file, while the unfinished typo `proej` returned nothing.
Whole-token typo correction leaves a real autocomplete gap. This is a retrieval
failure: a later ranking stage cannot promote a missing candidate.

At review time the fuzzy scorer chose the earliest matching characters greedily. Its
score can miss a better alignment later in the name. Existing synthetic
known-item benchmarks are useful for regressions, but ambiguous prefixes and
random target selection do not establish realistic launcher relevance. See
[evaluation](evaluation.md) for their conditions and limitations.

## Approaches to compare

| Approach | What it helps with | Role in Torchlight |
|---|---|---|
| Field-aware token search with prefix edit matching | Names, partial words, spelling mistakes, folder context | Main immediate retrieval path |
| Optimal fuzzy subsequence scoring | Abbreviations and letters typed in order | Complementary matching/scoring channel |
| BM25 over separate name/context fields | Word-based retrieval with corpus statistics | Comparison baseline; still needs autocomplete and typo handling |
| Local embedding similarity | Related phrases that use different words | Optional M4 retrieval path |
| Hybrid fusion with exact-match priority | Combines the useful results from both paths | Recommended target, evaluated against each individual path |

[Meilisearch's typo tolerance](https://www.meilisearch.com/docs/capabilities/full_text_search/relevancy/typo_tolerance_settings)
uses prefix edit matching. Its [ranking rules](https://www.meilisearch.com/docs/capabilities/full_text_search/relevancy/ranking_rules)
separate word coverage, typo count, proximity, field importance and exactness.
[Typesense](https://typesense.org/docs/29.0/api/search.html) supports weighted
fields, autocomplete prefixes and exact-field priority. These are useful design
references; adopting their full server runtimes is a separate dependency and
performance decision.

[fzf's V2 matcher](https://github.com/junegunn/fzf/blob/master/src/algo/algo.go)
finds the best character alignment under its scoring rules. Matching has an
O(name length × query length) cost and a greedy fallback for large inputs.
It requires every pattern character, so it does not replace edit-based typo
retrieval. An indexed candidate filter and benchmarked scoring are still needed
for a large filename catalog.

## Implementation sequence

Steps 1–4 are implemented for M3 Part 2, using explicit fields, sorted-range
prefix editing and bounded optimal alignment. The following list records the
original scope. Step 5 and fusion comparison belong to M4;
personalization belongs to M5.

1. Expand relevance tests before choosing a replacement. Include application
   name fragments competing with files, misspelled unfinished words, duplicate
   names, folder-plus-filename queries, extensions, abbreviations, Unicode and
   meaningful no-match queries. Label all reasonable results for ambiguous
   queries. Keep variants of the same target together when splitting tuning
   and held-out data.
2. Keep names, application keywords/generic names and parent context as explicit
   fields. Rank evidence using exactness, complete token versus partial token,
   typo count, word coverage, proximity and field importance. The current
   synthetic-parent desktop representation cannot express all those distinctions.
   Test the balance between application-name relevance and an explicit file query.
3. Add indexed prefix typo retrieval, using a token trie/FST with bounded edit
   traversal or an equivalent measured index. Start with one edit on sufficiently
   long prefixes and preserve exact numeric identifiers. Include adjacent swaps
   such as `proej`/`proje`. Bound candidate work with measured recall; truncating
   candidates arbitrarily recreates missed-result problems.
4. Compare an optimal fuzzy scorer against the current greedy scorer. Check
   character alignment against an independent reference, update score bounds
   used for pruning, and measure allocation-free warm typing at 50k/500k paths.
5. Complete M4's local semantic path on names, extensions, relevant folders and
   application metadata. Preserve immediate name results, cancellation, shared
   snapshots and lexical fallback. Reading file contents remains outside M4.
6. Compare fusion strategies and then add M5's bounded usage signals. Keep
   explicit exact-name/path priority and verify that popularity does not bury
   a clearly named item.

[Elastic's hybrid documentation](https://www.elastic.co/docs/solutions/search/hybrid-search)
uses reciprocal rank fusion to combine lexical and vector rankings. RRF is a
reasonable first baseline already in `PLAN.md`, not a guaranteed optimum. A
[fusion study](https://arxiv.org/abs/2210.11934) found a tuned score combination
outperformed RRF on its datasets. Compare both on our held-out queries before
selecting a fusion rule.

## Updated semantic shortlist for evaluation

| Candidate | Why compare it | Remaining questions |
|---|---|---|
| [potion-retrieval-32M](https://huggingface.co/minishlab/potion-retrieval-32M) / [Model2Vec](https://github.com/MinishLab/model2vec) | Small static retrieval embeddings; simple token lookup/pooling is attractive for C | Short-name relevance, abbreviation handling, C parity and total memory |
| [Granite small English R2](https://github.com/ibm-granite/granite-embedding-models) | IBM's newer 47M/384-dimension English model replaces the older 30M model in the plan | CPU query latency, runtime/tokenizer parity and improvement over static embeddings |
| [EmbeddingGemma](https://ai.google.dev/gemma/docs/embeddinggemma) | Local multilingual model with adjustable 128–768 dimensions | CPU latency, quantized quality, total RSS and integration cost |
| [BGE-small-en-v1.5](https://huggingface.co/BAAI/bge-small-en-v1.5) | Existing plan's small transformer comparison baseline | Whether a newer candidate improves actual launcher queries within the budget |

Google's published EdgeTPU timing is not a measurement of this machine's CPU.
Model-card retrieval results concern different datasets; none proves filename
quality here. No model or inference dependency was installed for this review.

## How to choose

Compare current, improved lexical, semantic-only and hybrid search on the same
held-out labeled queries. Report first-useful-result rank, success in the popup's
top ten, candidate recall, and graded nDCG@10 where several results are relevant.
Report per query class so strong exact matching cannot hide weak typo results.
Also measure warm typing, final response latency, cancellation during fast edits,
startup, indexing/update cost and total steady/peak RSS at 50k and 500k entries.

Prefer the least costly design that materially improves those outcomes. Start
vector correctness with the float reference in the plan; add quantization or ANN
only when latency, memory and recall measurements justify it. A heavyweight
neural reranker is a later experiment if cheaper ranking still fails and its
measured latency fits the launcher.
