# <module name>

> **Status:** Planned | In progress | Implemented
> **Source:** `src/<area>/<module>.c` · **Header:** `include/torchlight/<module>.h`
> **Tests:** `tests/unit/test_<module>.c`

## Purpose
One paragraph: what this module does and why it exists.

## Responsibilities
- What it owns.
- What it explicitly does **not** do (and which module does).

## Public API
| Function | Description |
|---|---|
| `<module>_create()` | ... |
| `<module>_destroy()` | ... |

Ownership rules for pointers passed in and returned.

## Design
How it works internally: data structures, algorithms, key constants and why
they have those values. Include a small diagram if it helps.

## Data flow
Who calls this module, what it calls, and what data goes in and out.

## Invariants
Things that must always be true (e.g. "posting lists are sorted by id").

## Performance
Complexity, memory footprint, benchmark numbers (`make bench`) with the date
measured.

## Testing
What the unit tests cover and how to run only this module's tests.

## Gotchas
Non-obvious behavior, edge cases, known limitations.

## Related
- Modules: [[other module docs]]
- ADRs: `docs/adr/NNNN-*.md`
