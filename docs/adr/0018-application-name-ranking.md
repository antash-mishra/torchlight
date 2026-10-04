# 0018. Application names in mixed launcher results

- **Status:** Accepted, implemented
- **Date:** 2026-10-05

## Context

The running launcher indexed Google Chrome correctly, but `chrome` ranked it
25th. The popup showed only ten results. An application name token scored 5,000
while unrelated filenames starting with the query scored 6,000. Existing M3
tests checked that application names appeared somewhere, without enough file
competition to expose this failure. Equal-score desktop results were also
inserted in reverse order during merging, making the first result depend on
the requested limit.

## Decision

Add a bounded, domain-independent `lexical_set_prefix_bonus` builder setting.
It defaults to zero and accepts at most 4,096. The desktop engine uses 2,000
for name/token/initials prefixes. Apply it while collecting lexical evidence,
before top-k selection and one-symbol caching. Multiword upper bounds include
the setting, and a compile-time bound preserves exact-name/path priority.
No new query allocations, I/O or dependencies are introduced.

The daemon retains the desktop engine's order at equal scores, while still
placing application/settings results ahead of equally scored files.

## Alternatives considered

- Putting every application before files would promote weak metadata matches
  and could hide an exact file query.
- Adding a bonus during merging could miss an application already discarded
  by desktop top-k selection and would couple daemon ranking to score constants.
- A Chrome-specific alias would leave the same problem for other multiword
  application names.
- Semantic retrieval does not repair this identified name-ranking failure;
  M4 remains a separate measured addition.

## Consequences

The regression fixture contains Google Chrome, 24 competing filename prefixes,
an unrelated keyword-only application, and twelve tied desktop entries. It
checks typing prefixes, case, multiword names, limits of 1/10/1,000, metadata-only
ranking, exact file/path priority, and consistent tied application ordering.
Unit tests cover bonus limits, sealed builders, exact priorities and query
capacity consistency. These establish the new rule, not broad search relevance.
The remaining M3 Part 2, M4 and M5 work is tracked in
[the search quality review](../search-quality.md).
