# Releasing Torchlight

Torchlight ships as a `.deb` for Linux Mint 22 and Ubuntu 24.04 or newer (amd64), from a
signed APT repository on GitHub Pages. Installed copies update through the system's update
manager. See [ADR 0050](adr/0050-deb-and-signed-apt-repository.md) for the design.

## Publish a new version

1. Set the version in `include/torchlight/version.h` (for example `0.1.1`) and add a
   `## 0.1.1 (YYYY-MM-DD)` section at the top of `CHANGELOG.md`. The release notes are
   that section.
2. Run `make test`, `make lint`, `make test-packaging` and `make test-ui-isolated`. The
   release workflow runs the first three again, but not the UI checks, which need a
   display.
3. Commit, then tag and push:

   ```sh
   git tag v0.1.1
   git push origin master v0.1.1
   ```

4. Watch the **Release** workflow under the repository's Actions tab (`gh run watch`). It
   checks that the tag matches the version, tests, lints and builds the package, creates
   the GitHub release, adds the package to the APT repository and finally installs it on
   a clean Ubuntu 24.04 machine and checks that the repository serves it.

Installed copies see the new version at their next update check. When it is installed,
each logged-in user's daemon restarts at once, and the popup restarts the next time it
is hidden.

A failed run can be re-run from the Actions tab (**Re-run failed jobs**); a GitHub release
made by an earlier attempt is reused. A fix to the workflow or the publishing scripts needs
a new commit: delete the release and the tag (`gh release delete v0.1.1 --cleanup-tag`),
tag the fixed commit and push the tag again.

## What lives where

| What | Where |
|---|---|
| Version | `include/torchlight/version.h` |
| Package layout and scripts | `packaging/deb/` (`build_deb.sh`, `postinst`, `prerm`, `postrm`, `control.in`, `copyright`) |
| Repository publishing | `packaging/deb/publish_apt.sh`, run by `.github/workflows/release.yml` |
| GNOME Shell extension | `ui/gnome-shell/torchlight@antash-mishra.github.io/`; add each new GNOME release to `shell-version` in its `metadata.json` once `make test-ui-gnome` passes on it |
| Published repository | the `gh-pages` branch, served at `https://antash-mishra.github.io/torchlight/apt`, which redirects over HTTPS to the account's custom domain (`https://antash.dev/torchlight/apt`); installations use the github.io address so that a domain change does not break them |
| Public key | `packaging/deb/torchlight-archive-keyring.gpg`, also published with the repository |
| Private key | the `APT_SIGNING_KEY` Actions secret, backed up in `~/.local/share/torchlight-release/` on the maintainer's machine |

The release key is an ed25519 OpenPGP key without expiry, fingerprint
`6A36 424D 0E32 058F 1024  F8FB BC78 7234 43BA 2945`. Keep a copy of
`~/.local/share/torchlight-release/` somewhere safe and offline: without the private key,
installed copies cannot receive updates. The GnuPG home there also holds a revocation
certificate (`gnupg/openpgp-revocs.d/`).

## Build and check a package locally

```sh
make deb                      # build/deb/torchlight_VERSION_amd64.deb
dpkg-deb --info build/deb/torchlight_*_amd64.deb
dpkg-deb --contents build/deb/torchlight_*_amd64.deb
```

`build_deb.sh` refuses binaries with a run path. On a machine that builds against
libraries outside the system (as `machine.mk` does), override the linker flags, for
example `make deb LDFLAGS=-L/path/to/libs`, and give dpkg-shlibdeps the same folder with
`SHLIBDEPS_FLAGS="-l/path/to/libs --ignore-missing-info"`. Published packages are built by
the workflow, where every library comes from the distribution.

To try the repository before publishing, publish into a scratch folder with the release key
and point apt at it as an ordinary user:

```sh
GNUPGHOME=~/.local/share/torchlight-release/gnupg packaging/deb/publish_apt.sh /tmp/site build/deb/*.deb
# then a sources file with URIs: file:///tmp/site/apt and Signed-By: packaging/deb/torchlight-archive-keyring.gpg,
# and apt-get -o Dir::Etc=... -o Dir::State=... -o APT::Sandbox::User=$USER update --error-on=any
```

## If the key is lost or leaks

Generate a new key, replace `packaging/deb/torchlight-archive-keyring.gpg` and the
`APT_SIGNING_KEY` secret, and publish a release signed with the new key. Installed copies
cannot verify it until they trust the new key, so tell users to reinstall from the latest
`.deb` (which carries the new key) or to fetch the key as the
[site's instructions](https://antash-mishra.github.io/torchlight/) show. If the old key
leaked, also publish its revocation certificate.
