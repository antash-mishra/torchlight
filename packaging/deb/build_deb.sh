#!/bin/sh
# Build the Torchlight .deb from the release binaries (make release) into OUT_DIR, as
# OUT_DIR/torchlight_VERSION_ARCH.deb. The version comes from include/torchlight/version.h.
# Library dependencies are worked out by dpkg-shlibdeps from the binaries; extra flags for
# it (for example -l DIR) can be passed in SHLIBDEPS_FLAGS. Needs dpkg-dev.
# usage: packaging/deb/build_deb.sh OUT_DIR
set -eu

out=${1:?usage: build_deb.sh OUT_DIR}
root=$(cd "$(dirname "$0")/../.." && pwd)
here="$root/packaging/deb"
version=$(sed -n 's/^#define TORCHLIGHT_VERSION "\(.*\)"$/\1/p' "$root/include/torchlight/version.h")
[ -n "$version" ] || { echo "build_deb.sh: no version in include/torchlight/version.h" >&2; exit 1; }
arch=$(dpkg --print-architecture)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
pkg="$stage/torchlight"

# Binaries, stripped. A run path would point every user's daemon at a directory on the
# build machine, so a binary that has one is refused.
for binary in torchlight torchlightd torchlight-gtk; do
    if readelf -d "$root/build/release/$binary" | grep -qE 'RPATH|RUNPATH'; then
        echo "build_deb.sh: build/release/$binary has a run path; build it without one" >&2
        exit 1
    fi
    install -D -m 755 "$root/build/release/$binary" "$pkg/usr/bin/$binary"
    strip --strip-unneeded "$pkg/usr/bin/$binary"
done

# The per-user daemon unit, the launcher's desktop entry and the popup's fonts.
install -d "$pkg/usr/lib/systemd/user"
sed 's|^ExecStart=.*|ExecStart=/usr/bin/torchlightd|' "$root/packaging/torchlightd.service" \
    > "$pkg/usr/lib/systemd/user/torchlightd.service"
chmod 644 "$pkg/usr/lib/systemd/user/torchlightd.service"
install -D -m 644 "$root/packaging/org.torchlight.Launcher.desktop" \
    "$pkg/usr/share/applications/org.torchlight.Launcher.desktop"
install -D -m 644 "$root/third_party/fonts/schibsted-grotesk/SchibstedGrotesk-VF.ttf" \
    "$pkg/usr/share/fonts/truetype/torchlight/SchibstedGrotesk-VF.ttf"
install -D -m 644 "$root/third_party/fonts/bedstead/bedstead.otf" \
    "$pkg/usr/share/fonts/truetype/torchlight/bedstead.otf"

# The repository that brings updates, and the key that signs it.
install -D -m 644 "$here/torchlight.sources" "$pkg/etc/apt/sources.list.d/torchlight.sources"
install -D -m 644 "$here/torchlight-archive-keyring.gpg" \
    "$pkg/usr/share/keyrings/torchlight-archive-keyring.gpg"

# Documentation: copyright and licenses, and the changelog.
install -D -m 644 "$here/copyright" "$pkg/usr/share/doc/torchlight/copyright"
install -m 644 "$root/third_party/fonts/schibsted-grotesk/OFL.txt" "$pkg/usr/share/doc/torchlight/OFL.txt"
gzip -9n -c "$root/CHANGELOG.md" > "$pkg/usr/share/doc/torchlight/changelog.gz"
chmod 644 "$pkg/usr/share/doc/torchlight/changelog.gz"

# Dependencies from the binaries' shared libraries, with the minimum versions their symbols need.
mkdir -p "$stage/debian"
printf 'Source: torchlight\n\nPackage: torchlight\nArchitecture: any\n' > "$stage/debian/control"
# shellcheck disable=SC2086 # SHLIBDEPS_FLAGS is a list of flags.
depends=$(cd "$stage" && dpkg-shlibdeps -O ${SHLIBDEPS_FLAGS:-} \
          -e"$pkg/usr/bin/torchlight" -e"$pkg/usr/bin/torchlightd" -e"$pkg/usr/bin/torchlight-gtk" |
          sed -n 's/^shlibs:Depends=//p')
[ -n "$depends" ] || { echo "build_deb.sh: dpkg-shlibdeps found no dependencies" >&2; exit 1; }
depends="$depends, systemd"

mkdir -p "$pkg/DEBIAN"
size=$(du -sk "$pkg" | cut -f1)
sed -e "s|@VERSION@|$version|" -e "s|@ARCH@|$arch|" -e "s|@SIZE@|$size|" -e "s|@DEPENDS@|$depends|" \
    "$here/control.in" > "$pkg/DEBIAN/control"
install -m 755 "$here/postinst" "$here/prerm" "$here/postrm" "$pkg/DEBIAN/"
echo /etc/apt/sources.list.d/torchlight.sources > "$pkg/DEBIAN/conffiles"
(cd "$pkg" && find usr -type f -exec md5sum {} + | sort -k2 > DEBIAN/md5sums)

mkdir -p "$out"
deb="$out/torchlight_${version}_${arch}.deb"
dpkg-deb --root-owner-group -Zxz --build "$pkg" "$deb" >/dev/null
echo "$deb"
