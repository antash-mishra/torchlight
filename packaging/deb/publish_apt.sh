#!/bin/sh
# Add packages to Torchlight's signed APT repository and re-sign it. SITE_DIR is a checkout
# of the gh-pages branch, which GitHub Pages serves at
# https://antash-mishra.github.io/torchlight/; the repository is its apt/ folder. Earlier
# versions stay in the pool, so an update can be rolled back. The release signing key must
# be in the GnuPG keyring (the release workflow imports it from the APT_SIGNING_KEY secret);
# set GPG_KEY to choose a key by fingerprint. Needs apt-utils and gnupg.
# usage: packaging/deb/publish_apt.sh SITE_DIR DEB...
set -eu

site=${1:?usage: publish_apt.sh SITE_DIR DEB...}
shift
[ "$#" -gt 0 ] || { echo "publish_apt.sh: no packages given" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
apt="$site/apt"
pool="$apt/pool/main/t/torchlight"
mkdir -p "$pool" "$apt/dists/stable/main/binary-amd64"
for deb in "$@"; do
    cp "$deb" "$pool/"
done

cd "$apt"
apt-ftparchive packages pool > dists/stable/main/binary-amd64/Packages
gzip -9n -k -f dists/stable/main/binary-amd64/Packages
# The signatures and the Release file itself must not be listed in the new Release.
rm -f dists/stable/Release dists/stable/InRelease dists/stable/Release.gpg
release=$(mktemp)
apt-ftparchive \
    -o APT::FTPArchive::Release::Origin=Torchlight \
    -o APT::FTPArchive::Release::Label=Torchlight \
    -o APT::FTPArchive::Release::Suite=stable \
    -o APT::FTPArchive::Release::Codename=stable \
    -o APT::FTPArchive::Release::Architectures=amd64 \
    -o APT::FTPArchive::Release::Components=main \
    -o "APT::FTPArchive::Release::Description=Torchlight launcher" \
    release dists/stable > "$release"
mv "$release" dists/stable/Release
chmod 644 dists/stable/Release
signer=${GPG_KEY:+--local-user $GPG_KEY}
# shellcheck disable=SC2086 # signer is empty or two words.
gpg --batch --yes $signer --clearsign -o dists/stable/InRelease dists/stable/Release
# shellcheck disable=SC2086
gpg --batch --yes $signer --armor --detach-sign -o dists/stable/Release.gpg dists/stable/Release

# The public key and source file, for people who add the repository by hand, and the
# site's front page.
cp "$here/torchlight-archive-keyring.gpg" torchlight-archive-keyring.gpg
cp "$here/torchlight.sources" torchlight.sources
cp "$here/site/index.html" "$site/index.html"
touch "$site/.nojekyll"
