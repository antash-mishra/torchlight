# dirtree

> **Status:** Implemented (M1)
> **Source:** `src/index/dirtree.c` · **Header:** `include/torchlight/dirtree.h`
> **Tests:** `tests/unit/test_dirtree.c`

## Purpose
Stores every distinct directory of the indexed paths once, with a link to its
parent and its normalized name. Many files share each directory, so `lexical`
matches parent context once per directory instead of once per file, and stores
only basenames per entry.

## Responsibilities
- Interns directory paths into nodes and owns their normalized names.
- Provides parent links, names, path masks and path lengths.
- Does **not** match or score anything, and does not know about roots.

## Public API
| Function | Description |
|---|---|
| `dirtree_create()` / `dirtree_destroy()` | Owned tree containing only `DIRTREE_ROOT` (`/`). |
| `dirtree_intern(tree, bytes, length, &node)` | Intern an absolute directory path and all its ancestors. |
| `dirtree_finish(tree)` | Seal: storage stops moving, name views stay valid. |
| `dirtree_count`, `dirtree_parent`, `dirtree_name` | Node count, parent link, normalized name view. |
| `dirtree_path_mask`, `dirtree_path_length`, `dirtree_max_path_length` | Whole-path metadata. |

## Design
- Nodes are keyed by `(parent node, raw name bytes)` in a `tl_hashmap`, so a
  directory is found without storing its full path. Repeated slashes are ignored.
- Names are normalized with `tokenize_into` into shared symbol/boundary arenas.
  Each node records its name mask, the mask of its whole path including `/`, and
  the normalized length of its path.
- The last interned path is remembered: crawls emit siblings consecutively, so
  most interns skip the per-component walk entirely.
- A parent is always interned before its children, so node numbers are a
  topological order.

## Data flow
`lexical_add` interns each path's parent directory and stores only the node
number; `lexical_finish` interns each root's own directory, then seals the tree.

## Invariants
- `dirtree_parent(node) < node` for every node except the root.
- Name views stay valid from `dirtree_finish` until destruction.

## Performance
Memory is about 48 bytes per directory plus its name. Interning a path costs one
hash probe per new component and none when it repeats the previous path.

## Testing
`make test` runs `tests/unit/test_dirtree.c`: interning and reuse, repeated
slashes, topological order, relative and NUL-containing paths, sealing, names,
masks and lengths.

## Gotchas
Relative paths are rejected; the engine only indexes absolute paths.

## Related
- [lexical](../lexical/README.md), [tokenize](../tokenize/README.md), [core](../core/README.md)
- ADR: [0008](../../adr/0008-m1-completion-channels-directories-config.md)

## Raw ASCII names (M6)

Each node records whether its raw name is all ASCII; `dirtree_ascii_name`
borrows those bytes (exactly `dirtree_name(node).length` of them, case kept) so
the Frizbee matcher scores directory names without re-encoding symbols. Other
names return NULL and are encoded from symbols.
