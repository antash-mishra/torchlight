# Building Torchlight

Most people can install a release instead; see [Install](../README.md#install). This page
covers building from source, a headless build without the popup, and working on Torchlight.

## Build

Install the build dependencies (Debian, Ubuntu, Mint):

```sh
sudo apt install build-essential pkg-config libsqlite3-dev libutf8proc-dev \
    libgtk-4-dev libglib2.0-dev libx11-dev
```

You also need Rust 1.89 or newer (for example from [rustup](https://rustup.rs)) to
compile the vendored [Frizbee](../third_party/README.md) fuzzy matcher. GTK 4.14 or
newer is required.

```sh
make                             # debug build: build/torchlight, torchlightd, torchlight-gtk
make build/torchlightd-release   # optimized daemon, about half the tail latency
make deb                         # optimized .deb in build/deb (needs dpkg-dev)
make tarball                     # portable archive with install.sh in build/portable
```

Releases are published by tagging a version; see [Releasing](release.md).

The first `make` compiles Frizbee offline, which takes about four minutes; later
builds reuse it.

## Install a source build and set up the desktop launcher

```sh
make install                     # installs into ~/.local (set PREFIX to change it)
systemctl --user daemon-reload
systemctl --user import-environment DISPLAY XDG_CURRENT_DESKTOP DBUS_SESSION_BUS_ADDRESS
systemctl --user enable --now torchlightd.service
```

This installs the three programs, a desktop entry, the popup's fonts, the GNOME Shell and
Cinnamon extensions and a systemd user service for the daemon (in
`~/.local/share/systemd/user`). On Cinnamon, Xfce, MATE and LXQt the service does not start
at login by itself; [Desktop setup](desktop-setup.md) shows the autostart entry that starts
it. Make sure `~/.local/bin` is on your PATH, then bind
`~/.local/bin/torchlight-gtk --toggle` to a keyboard shortcut;
[Desktop setup](desktop-setup.md) says where on each desktop and covers staged installs
and service troubleshooting.

## Headless: daemon and command line only

On a server, in WSL, or anywhere without GTK 4.14, build and install just the
daemon and the command line. They need no GTK, X11 or display:

```sh
sudo apt install build-essential pkg-config libsqlite3-dev libutf8proc-dev libglib2.0-dev
make headless                    # build/torchlight and build/torchlightd
make install-headless            # into ~/.local, with a service that starts at login
systemctl --user daemon-reload
systemctl --user enable --now torchlightd.service
loginctl enable-linger "$USER"   # optional: keep it running while you are logged out
```

Rust is still needed for Frizbee. Search with `torchlight query`, or talk to the
daemon's socket from your own front end ([IPC protocol](modules/ipc/README.md)).
See [ADR 0037](adr/0037-headless-build.md).

## Try it without installing

Run the daemon in one terminal:

```sh
./build/torchlightd
```

Then, in another terminal, open the popup or search from the command line:

```sh
./build/torchlight-gtk --toggle
./build/torchlight query "prjnts"
```

[The command line](configuration.md#the-command-line) lists what else `torchlight` does.

## Development

```sh
make test               # unit, CLI and daemon tests under ASan, UBSan and leak checks
make lint               # clang-tidy and cppcheck; warnings fail the build
make lint-headless      # the same without the GTK popup, for machines without GTK
make format             # clang-format
make bench              # search latency and ranking quality at 50k and 500k paths
make bench-daemon       # daemon startup, IPC, indexing load, update lag and memory
make test-packaging     # the .deb's repository and scripts, and the portable archive's installer
make test-ui-isolated   # popup acceptance on a private X display (needs xvfb, xdotool, metacity)
make test-ui-wayland    # popup acceptance on a headless Wayland compositor (needs weston)
make bench-ui           # popup paint timing during a 500k rebuild
make test-ui            # popup keyboard acceptance on your own X11 session (needs xdotool)
```

Lint needs `clang-format`, `clang-tidy` and `cppcheck`. A local `machine.mk`
(ignored by git) can point the build at custom tool or dependency locations
(`DEPS_PREFIX`, `CLANG_TIDY`, `CPPCHECK`). To benchmark against your own files:

```sh
./scripts/make_corpus.sh /usr /tmp/usr.paths   # NUL-separated path list; keep it private
make bench BENCH_PATHS=/tmp/usr.paths
```

After rebuilding, restart the daemon (`systemctl --user restart torchlightd`). The
popup notices that its program file was replaced and quits the next time it is hidden,
so your next shortcut press starts the new one.

Read [CLAUDE.md](../CLAUDE.md) for the code rules and the [docs index](README.md) for
the architecture, evaluation and design decisions.
