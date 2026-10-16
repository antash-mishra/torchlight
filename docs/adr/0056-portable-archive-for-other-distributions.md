# 0056. Ship a portable archive for other distributions

- **Status:** Accepted
- **Date:** 2026-10-15

## Context

Releases shipped only a `.deb` ([ADR 0050](0050-deb-and-signed-apt-repository.md)),
which installs on Ubuntu-based systems. The user asked for a binary that people on other
distributions can install too.

The programs set the floor. The popup needs GTK 4.14, and binaries built on Ubuntu 24.04
need glibc 2.39 (`pidfd_spawnp` and `pidfd_getpid`, from the Rust standard library that
Frizbee links in). The distributions that have GTK 4.14 have glibc 2.39 as well: Ubuntu
24.04, Debian 13, Fedora 40, openSUSE Tumbleweed and Arch. Besides the programs, an
installation needs the daemon's systemd user unit, the launcher's desktop entry, the
popup's fonts and the GNOME Shell and Cinnamon extensions. utf8proc, which the programs
linked from the system, is rarely installed outside Debian's family.

## Decision

1. **A portable archive with each release.** `make tarball`
   (`packaging/portable/build_tarball.sh`) packs the release binaries, stripped, with the
   unit, desktop entry, fonts, extensions, licenses and changelog in a prefix layout, plus
   `install.sh` and a short README, into `torchlight-VERSION-linux-x86_64.tar.gz`. The
   archive is reproducible: sorted, owned by root, dated by the last commit.
2. **One build for both packages.** Release builds link SQLite and utf8proc in, so the
   `.deb` and the archive carry the same binaries. They take only the platform's libraries
   from the system: glibc, libgcc_s, GLib, GTK 4, Pango, Cairo, Graphene, X11 and the
   Wayland client. The archive's build refuses any other library, and a run path, so a new
   dependency is a decision rather than a surprise. The license file credits SQLite and
   utf8proc.
3. **A per-user installer, without root.** `install.sh` refuses to run as root, checks
   with `ldd -r` that every library and symbol is present (naming what is missing, with
   the package to install on common distributions), then installs into `~/.local/bin` and
   `~/.local/share`. The desktop entry names the popup by its full path, since a session's
   PATH may lack `~/.local/bin`. Programs are copied beside the old ones and renamed over
   them, so a running daemon keeps its file and the popup restarts onto the new one as it
   does after a `.deb` upgrade. The daemon runs from the systemd user unit when the
   session has a user manager, and otherwise from an XDG autostart entry, started at once;
   only one of the two is kept. Running a newer release's installer updates;
   `--uninstall` removes everything it installed and keeps the index, history and
   settings.
4. **Checked on other distributions before publishing.** `tests/test_portable.py` (in
   `make test-packaging`) builds archives from stand-in programs and runs the installer
   against stand-in `systemctl`, `pkill` and `pgrep`: first install, update with the old
   programs open, no systemd, removal, missing libraries, root and an awkward home path.
   The release workflow then installs the real archive as an ordinary user in Ubuntu
   24.04 (the GTK 4.14 floor), Debian 13, Fedora, Arch and openSUSE Tumbleweed containers
   (`tests/check_portable.sh`): the daemon must find a file by a misspelled name and the
   popup must open on a virtual display and take a search. Only then is the release
   published, with a `SHA256SUMS` file beside the `.deb` and the archive. Run by hand, the
   workflow builds and checks without publishing.

## Alternatives considered

- **AppImage.** One file, but it bundles GTK (tens of megabytes, with icon theme and GPU
  driver mismatches), needs FUSE, which many systems no longer install, and still needs a
  first-run step for the daemon's service.
- **Flatpak.** As in ADR 0050: the sandbox conflicts with indexing the home folder,
  launching other applications and listing their windows.
- **Native packages for each distribution** (RPM repositories, the AUR). Updates would
  come through each package manager, but each is another build and repository to keep.
  They can come later for the most asked-for distributions.
- **A separate, statically linked build for the archive only.** The `.deb` would keep the
  distribution's SQLite and utf8proc, but there would be two sets of binaries to test.
- **An older build machine for an older glibc.** GTK 4.14 is the floor either way.

## Consequences

- Fedora, Arch, openSUSE, Debian 13 and other recent distributions install Torchlight with
  two commands and no root. Updates are by hand: nothing tells archive users about a new
  version.
- Release builds no longer pick up the distribution's SQLite or utf8proc fixes; those
  arrive with Torchlight releases. The `.deb` no longer depends on `libsqlite3-0` and
  `libutf8proc3`, and grows by about 1.2 MB (5.7 to 6.9 MB), since each of the three
  programs carries both libraries. The archive is about 10 MB.
- The archive needs a 64-bit PC with glibc 2.39 and GTK 4.14; the installer says what is
  missing when they are not there.
- The release workflow runs five more jobs, in parallel, before publishing.
- `make install` puts the unit in `PREFIX/share/systemd/user`, where systemd looks; it had
  used `PREFIX/lib/systemd/user`, which systemd does not search under `~/.local`.
