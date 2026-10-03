# 0016: Empty popup without default recommendations

**Status:** Accepted, implemented

## Context

The initial popup sent an empty query and displayed indexed roots. Home appeared
as a default result, which looked like a recommendation or indexing progress.
The user requested removing that default result.

## Decision

Opening or clearing the GTK popup shows `Type to search` with no result rows or
actionable selection. Whitespace-only input behaves the same. Cancel pending
query work and clear the model's request id, selection and metadata immediately;
late responses cannot restore results. Retry/recovery never sends an empty query.

The lexical engine and explicit CLI/IPC empty-query contract continue returning
indexed roots. This presentation choice belongs to the popup, which does not
send empty-query requests or record them in search history.

## Consequences

Enter and Ctrl+Enter do nothing until current results exist. Model regressions
cover clearing and late-response rejection. The desktop test covers initial
show, clearing populated/pending searches, whitespace-only input, inert Enter
and absence of blank search-history entries.
