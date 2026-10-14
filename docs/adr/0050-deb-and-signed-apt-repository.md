# 0050. Distribute as a .deb with a signed APT repository, and pick up updates

- **Status:** Accepted
- **Date:** 2026-10-14

## Context

Torchlight was installed by building it (`make install` into `~/.local`). The user asked
for a package that people can install, and for updates to reach them after new versions
are published. The target desktops are Linux Mint 22 and Ubuntu 24.04 or newer, whose GTK
(4.14) is the popup's minimum, so a native package needs no bundled libraries. The GitHub
repository is public.

Updating has two halves: getting new files onto the machine, and the running programs
switching to them. The daemon runs for the whole session and the popup stays resident
between shortcut presses, so both would keep running old code after an update.

## Decision

1. **A .deb for Mint 22 and Ubuntu 24.04+ (amd64).** `make release` builds optimized,
   hardened binaries (`-O2`, stack protector, fortify, full RELRO; no run path) and
   `make deb` (`packaging/deb/build_deb.sh`) packages them with the per-user daemon unit,
   the desktop entry, the popup's fonts and the licenses. `dpkg-shlibdeps` works out the
   library dependencies. The version lives in `include/torchlight/version.h` and the
   three programs print it with `--version`.
2. **A signed APT repository on GitHub Pages.** Pushing a `v*` tag runs
   `.github/workflows/release.yml`: it checks the tag against the version, runs `make
   test` and `make lint`, builds the .deb, creates a GitHub release with it, adds it to
   the repository on the `gh-pages` branch (`packaging/deb/publish_apt.sh`, served at
   `https://antash-mishra.github.io/torchlight/apt`), and finally installs the release on
   a clean Ubuntu 24.04 machine and checks that apt fetches the signed repository. The
   repository keeps earlier versions in its pool.
3. **Installing the package subscribes the machine.** The .deb installs
   `/etc/apt/sources.list.d/torchlight.sources` (a conffile, so deleting it opts out) and
   the public key in `/usr/share/keyrings`. From then on the update manager installs new
   versions like any other update; nothing in Torchlight touches the network.
4. **A dedicated release key.** An ed25519 OpenPGP key without expiry, made for this
   repository, signs the repository. Its private half is the `APT_SIGNING_KEY` Actions
   secret, with a backup outside the repository on the maintainer's machine; the public
   half is `packaging/deb/torchlight-archive-keyring.gpg`.
5. **Running programs pick up updates.** On upgrade the package's postinst restarts the
   daemon in every logged-in user's session (`systemctl --user --machine UID@ try-restart`);
   a first install enables it for all users and starts it for those logged in. The popup
   watches its own executable: when the file at its path is replaced (a different inode,
   as dpkg's rename or a rebuild leaves), it quits once hidden, so the next shortcut press
   starts the new version. Neither waits for the person to log out.

## Alternatives considered

- **An in-app updater** that downloads and verifies builds from GitHub Releases. It would
  work without root, but needs an HTTP client and signature checking (new dependencies),
  gives a privacy-minded app network access, and duplicates what the update manager does.
- **AppImage** with AppImageUpdate. Runs on more distributions, but bundles GTK and leaves
  the daemon service and shortcut to a first-run step.
- **Flatpak.** The sandbox conflicts with indexing the whole home folder, launching other
  applications and listing their windows.
- **A Launchpad PPA.** Native to Ubuntu, but builds from source packages on Launchpad and
  needs an account and key there; GitHub Pages keeps everything in this repository.

## Consequences

- Releasing is: bump the version and the changelog, tag, push the tag (docs/release.md).
- Losing the private key means existing installations stop receiving updates until they
  trust a new key; it must be backed up. A leaked key lets someone sign packages that
  installations would accept, so it should be revoked and replaced.
- Only amd64 and GTK 4.14+ systems are served. Other distributions build from source.
- The package installs system-wide; `make install` into `~/.local` remains for building
  from source, and a user's own `~/.config/systemd/user/torchlightd.service` overrides the
  packaged unit.
- The repository carries no license yet; the package's copyright file says so.
