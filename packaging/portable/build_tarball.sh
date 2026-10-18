#!/bin/sh
# Build the portable Torchlight archive from the release binaries (make release) into
# OUT_DIR, as OUT_DIR/torchlight-VERSION-linux-ARCH.tar.gz: the three programs and the
# files they need, in a prefix layout, with install.sh, which installs them for one person
# on any distribution with GTK 4.14 (ADR 0056). The version comes from
# include/torchlight/version.h. BINARY_DIR replaces build/release, for tests.
# usage: packaging/portable/build_tarball.sh OUT_DIR [BINARY_DIR]
set -eu

out=${1:?usage: build_tarball.sh OUT_DIR [BINARY_DIR]}
root=$(cd "$(dirname "$0")/../.." && pwd)
binaries=${2:-$root/build/release}
here="$root/packaging/portable"
version=$(sed -n 's/^#define TORCHLIGHT_VERSION "\(.*\)"$/\1/p' "$root/include/torchlight/version.h")
[ -n "$version" ] || { echo "build_tarball.sh: no version in include/torchlight/version.h" >&2; exit 1; }
name="torchlight-$version-linux-$(uname -m)"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
dir="$stage/$name"

# The libraries the programs may take from the system: the C library's and those every
# desktop with GTK 4 has. Anything else must be linked in (as SQLite and utf8proc are), or
# added here on purpose once every supported distribution is known to install it.
platform=" libc.so.6 libm.so.6 ld-linux-x86-64.so.2 libgcc_s.so.1 libglib-2.0.so.0 \
libgobject-2.0.so.0 libgio-2.0.so.0 libgtk-4.so.1 libpango-1.0.so.0 libcairo.so.2 \
libgraphene-1.0.so.0 libX11.so.6 libwayland-client.so.0 "

# Binaries, stripped. A run path would point at a directory on the build machine.
for binary in torchlight torchlightd torchlight-gtk; do
    file="$binaries/$binary"
    if readelf -d "$file" | grep -qE 'RPATH|RUNPATH'; then
        echo "build_tarball.sh: $file has a run path; build it without one" >&2
        exit 1
    fi
    for library in $(readelf -d "$file" | sed -n 's/.*Shared library: \[\(.*\)\]$/\1/p'); do
        case "$platform" in
        *" $library "*) ;;
        *) echo "build_tarball.sh: $file needs $library, which not every system has" >&2
           exit 1 ;;
        esac
    done
    install -D -m 755 "$file" "$dir/bin/$binary"
    strip --strip-unneeded "$dir/bin/$binary"
done

# The daemon's user unit (its ExecStart is ~/.local/bin/torchlightd, where install.sh puts
# it), the launcher's desktop entry, the popup's fonts and the GNOME Shell and Cinnamon
# extensions that list open windows on those desktops' Wayland sessions.
install -D -m 644 "$root/packaging/torchlightd.service" "$dir/share/systemd/user/torchlightd.service"
# Where the session never starts graphical-session.target (Cinnamon, Xfce, MATE, LXQt), this
# autostart entry starts the unit at login; install.sh copies it to ~/.config/autostart.
install -D -m 644 "$root/packaging/torchlightd-session.desktop" \
    "$dir/etc/xdg/autostart/torchlightd.desktop"
install -D -m 644 "$root/packaging/org.torchlight.Launcher.desktop" \
    "$dir/share/applications/org.torchlight.Launcher.desktop"
install -D -m 644 "$root/third_party/fonts/schibsted-grotesk/SchibstedGrotesk-VF.ttf" \
    "$dir/share/fonts/torchlight/SchibstedGrotesk-VF.ttf"
install -D -m 644 "$root/third_party/fonts/bedstead/bedstead.otf" "$dir/share/fonts/torchlight/bedstead.otf"
extension=torchlight@antash-mishra.github.io
for file in metadata.json extension.js; do
    install -D -m 644 "$root/ui/gnome-shell/$extension/$file" \
        "$dir/share/gnome-shell/extensions/$extension/$file"
    install -D -m 644 "$root/ui/cinnamon/$extension/$file" "$dir/share/cinnamon/extensions/$extension/$file"
done

# The installer, a note on using it, the licenses of everything included and the changelog.
install -m 755 "$here/install.sh" "$dir/install.sh"
sed "s|@VERSION@|$version|" "$here/README" > "$dir/README"
install -D -m 644 "$root/LICENSE" "$dir/share/doc/torchlight/LICENSE"
install -m 644 "$root/packaging/deb/copyright" "$dir/share/doc/torchlight/copyright"
install -m 644 "$root/third_party/fonts/schibsted-grotesk/OFL.txt" "$dir/share/doc/torchlight/OFL.txt"
install -m 644 "$root/CHANGELOG.md" "$dir/share/doc/torchlight/CHANGELOG.md"
chmod 644 "$dir/README"

# The same sources give the same archive: sorted, owned by root, dated by the last commit.
when=$(git -C "$root" log -1 --format=%ct 2>/dev/null || date +%s)
mkdir -p "$out"
tarball="$out/$name.tar.gz"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$when" -C "$stage" -cf - "$name" |
    gzip -9n > "$tarball"
echo "$tarball"
