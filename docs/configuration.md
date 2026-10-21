# Configuration

## Folders

Torchlight indexes your home folder by default. To choose folders, create
`~/.config/torchlight/config` (or `$XDG_CONFIG_HOME/torchlight/config`):

```text
# Folders to index. "~/" expands to your home folder.
root = ~/Documents
root = ~/projects
# Hidden or normally skipped folders to index anyway.
allow = ~/.config/nvim
# Folders never to index, even inside a root or an allowed folder.
exclude = ~/Downloads/archive
exclude = ~/VirtualBox VMs
```

Hidden folders and folders that tools manage are skipped unless allowed: those
named `node_modules`, `bower_components`, `site-packages`, `dist-packages`,
`target`, `build`, `CMakeFiles` or `__pycache__`, Python virtual environments,
Conda environments, Android SDKs, Go's module cache and caches marked with a
`CACHEDIR.TAG` ([ADR 0062](adr/0062-folders-tools-manage.md)), wherever they are.
Hidden files are still searchable. Symlinked folders are not followed. An excluded folder's saved entries are removed by the next full
scan; the daemon reads the configuration when it starts, so restart it after
editing (`systemctl --user restart torchlightd`). The catalog and history live
in `~/.local/share/torchlight/`.

Files are ranked by how well their names match, then by where they live:
among similar matches, files closer to the indexed folder come first, and
files inside installed or vendored software (`vendor`, `third_party`, SDKs and
versioned install folders) come after your own
([ADR 0047](adr/0047-place-ranking-and-excluded-folders.md)). Typing one word lists
what matches it by a word start (a name, a word of a name, initials, or an
application's tags such as "wifi" for Network); near matches (typos, shared
letters) appear only when nothing does
([ADR 0061](adr/0061-word-matches-first.md)).

## The daemon

| Option | Meaning |
|---|---|
| `--no-history` | Don't record what you open |
| `--history-days N` | Keep history for N days (default 30) |
| `--rescan-ms N` | Milliseconds between rescans of folders the watcher cannot cover, such as unwatchable, unreadable or network folders (default 30000) |
| `--repair-ms N` | Milliseconds between safety-net full rescans (default 3600000, one hour; `0` turns them off) |
| `--socket PATH` | Use another socket (also accepted by `torchlight`) |
| `--db PATH`, `--config PATH` | Use another catalog or configuration file |
| `--watch-capacity N`, `--max-entries N`, `--max-path-bytes N` | Resource limits |
| `--model PATH.tlm` | Enable optional semantic search (see below) |

To change the options of the installed service, run
`systemctl --user edit torchlightd`, set a new `ExecStart=`, then restart it. Where the
portable archive's installer found no systemd user manager, the daemon starts from
`~/.config/autostart/torchlightd.desktop`; edit its `Exec=` line instead.

### Optional semantic search

Torchlight can also match by meaning using a local
[Potion](m4-model-evaluation.md) model passed with `--model`. It works and is
tested, but it is parked: it is slower on large catalogs and is not enabled by
default ([ADR 0032](adr/0032-park-semantic-search.md)).

## The command line

`torchlight` searches through the running daemon, and manages it:

```sh
torchlight query "prjnts"             # abbreviation
torchlight query --limit 20 "work notes"
torchlight query --json "report"      # ids, exact paths and status
torchlight query --null "report"      # exact paths, NUL-separated, for scripts
torchlight status
torchlight reconcile                  # rescan in the background
torchlight history-clear              # forget search and open history
```

Every word of a query must match the file name or one of its folders; a word
containing `/` can match across the whole path. Put `--` before a query that
starts with a dash.

With the daemon stopped, you can index and search directly:

```sh
torchlight index                      # sync the configured roots
torchlight index "$HOME/Documents"    # or refresh specific folders
torchlight query --db ~/.local/share/torchlight/catalog.db "prjnts"
```
