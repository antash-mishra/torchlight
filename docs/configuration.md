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

## Web search and available apps

Type an app's name, such as `vlc`, to open an installed app or find an available
Flatpak app to install. No `install` prefix is needed. Installed apps show
**Open**; available apps show **Install**. A named installed app suppresses
install suggestions. An exact available-app name selects Install ahead of loose
file matches; exact local filenames and paths keep priority. Partial app matches
follow local results, with at most three suggestions. If nothing local matches,
the first available app is selected. `install QUERY` optionally browses all
matching sources, and `local QUERY` searches only local items.

The popup also accepts `web linux tips` for a browser search and
`local install notes` for a literal local search.
Ordinary searches keep ten local slots and show a fixed Web action below the
query, outside the scrolling results. Website addresses such as `google.com`
offer **Open google.com** first and navigate directly. Ordinary text keeps the
best local match selected unless the exact-app rule above applies; no local or
available-app matches selects Web. **Alt+Enter** searches
the web immediately, including when Enter would visit a website. A bare `web`
or `install` remains a local query. Browser search sends nothing to the web
until you activate its row.

Web searches and website addresses always open in the system's default browser
for every user. Change it in your desktop's default-applications settings;
Torchlight follows the current HTTP(S) handler when you activate a Web action,
without needing a restart. Legacy `browser=` entries in `web.ini` are ignored.

The popup has no settings button. It reads the saved search engine from
`~/.config/torchlight/web.ini` (under `XDG_CONFIG_HOME` when set).
To change the engine, edit this file and restart the popup:

```ini
[Web]
engine=google
```

This is a popup preference. Without a saved engine override the popup uses the
daemon's `web_engine` below. If the system cannot open a Web action, the popup
keeps the query visible and reports the error.
CLI `web` requests continue using the daemon's configuration.

Automatic addresses use common domain suffixes and validate HTTP(S) URLs;
`report.pdf`, `main.rs`, paths and unsupported schemes remain searches.
Use an explicit `https://` URL for less common domain suffixes, or `local` to
search a filename that resembles a website.

Optional daemon settings belong in `~/.config/torchlight/config`, separately
from the popup's `web.ini`:

```ini
web_search = true
web_engine = duckduckgo
software_search = auto
```

`web_search` accepts `true` or `false`; `web_engine` accepts `duckduckgo` or
`google`; `software_search` accepts `auto` or `false`. These are the defaults.
Restart torchlightd after changing configuration. Feature settings may occur
only once; unknown keys and invalid values fail validation.

Software discovery uses the optional Flatpak executable and your already
configured, enabled remotes. It does not set up repositories. Enter opens a
supported manager's app details, labeled **Install in Software**, where
you choose the source and confirm installation. GNOME Software and KDE Discover
need their Flatpak support installed. Mint Software Manager's import route is
not enabled because it may create a system remote; Cinnamon can use GNOME
Software or Discover. Without a supported manager, Torchlight keeps the selection
visible and explains the missing support. In explicit software browsing, installed
variants show **Manage**; local application results retain their verified Open action.

Installed-app preference uses the desktop launcher catalog. For example, a native
VLC launcher is enough to offer Open instead of a Flatpak installation in ordinary
search. Torchlight does not yet map every native, Snap or AppImage package to a
Flatpak identity. Apps without a visible launcher may remain undetected, and
explicit software browsing can still offer another package format.

On Debian/Ubuntu/Mint, GNOME Software's plugin is packaged separately as
`gnome-software-plugin-flatpak`; Discover's is `plasma-discover-backend-flatpak`.
Installing the manager executable alone may not enable Flatpak app details.

Discovery runs in the background and retains cached results when a refresh
fails. Retry or `torchlight software-refresh` requests a refresh. Local search
continues when Flatpak, its metadata or the network is unavailable.

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

Storage usage can be inspected while the daemon runs:

```sh
torchlight storage                 # database bytes, free pages and semantic payload/counts
```

To reclaim database space, stop the daemon and close other SQLite readers first:

```sh
systemctl --user stop torchlightd
torchlight maintenance --purge-semantic
systemctl --user start torchlightd
```

This removes cached vectors/model descriptors and compacts the database while
preserving file IDs, indexed roots and history. Omit `--purge-semantic` to compact
while keeping semantics. Both commands accept `--db PATH` for another existing
catalog. Maintenance never runs automatically; allow temporary free space of up
to twice the database size. A failed compaction may follow a successful purge;
close readers/free space and retry. SQLite journals the operation for recovery.
External `.tlm` model files are separate: remove only a model you no longer use,
and remove its `--model` service argument before restarting. Otherwise semantic
search can recreate its cache. See [ADR 0072](adr/0072-smaller-builds-and-storage-maintenance.md).

With the daemon stopped, you can index and search directly:

```sh
torchlight index                      # sync the configured roots
torchlight index "$HOME/Documents"    # or refresh specific folders
torchlight query --db ~/.local/share/torchlight/catalog.db "prjnts"
```

The explicit lookup commands below print results without opening a browser or
installing anything. They require an updated daemon and reject `--null`:

```sh
torchlight web 'café & recipes'        # encoded browser URL
torchlight software vlc               # app ID, name, installation/source, arch/branch
torchlight software --json vlc        # full typed metadata and catalog state
torchlight software-refresh           # enqueue refresh; status reports readiness
```

`torchlight query 'web notes'`, `query --null`, and offline `query --db` keep
their original literal, path-only behavior. New GTK probes daemon capabilities;
with an old daemon it retains local searches and explains that explicit web or
software modes need an update.
