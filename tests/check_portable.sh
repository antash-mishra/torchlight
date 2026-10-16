#!/bin/sh
# Smoke test of a portable archive on the distribution it runs on, as a person who just
# downloaded it (ADR 0056): install it into a fresh home, check that the daemon indexes the
# home folder and answers a search, that the popup loads its libraries and, on a private X
# display, opens and takes a search, then remove it. Run it as an ordinary user without a
# systemd user manager (as in a container), so install.sh takes its autostart path. The
# popup's display step needs Xvfb, xdotool and dbus-run-session; without them it is skipped,
# or fails with TORCHLIGHT_CHECK_POPUP=required (as the release workflow sets).
# usage: tests/check_portable.sh ARCHIVE
set -eu

archive=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export HOME="$work/home" XDG_RUNTIME_DIR="$work/run" GTK_A11Y=none
mkdir -p "$HOME/Projects/site" "$HOME/Documents/Work/notes"
mkdir -m 700 "$XDG_RUNTIME_DIR"
echo site > "$HOME/Projects/site/README.md"
echo notes > "$HOME/Documents/Work/notes/meeting-notes.md"
bin="$HOME/.local/bin"

fail() {
    echo "check_portable.sh: $*" >&2
    exit 1
}

# The query is retried while the daemon's first scan runs.
search() {
    for _ in $(seq 1 100); do
        if "$bin/torchlight" query "$1" 2>/dev/null | grep -q "$2"; then
            return 0
        fi
        sleep 0.2
    done
    fail "searching \"$1\" did not find $2"
}

# The popup opens on a private display and bus, takes a search and stays up. The bus has
# no services, so no desktop portal is started for GTK to wait on.
popup() {
    for tool in Xvfb xdotool dbus-run-session; do
        command -v "$tool" >/dev/null 2>&1 && continue
        [ "${TORCHLIGHT_CHECK_POPUP:-}" != required ] || fail "no $tool for the popup's display check"
        echo "Popup display check skipped: no $tool"
        return 0
    done
    Xvfb -displayfd 1 -screen 0 1280x800x24 -nolisten tcp > "$work/display" 2>/dev/null &
    server=$!
    for _ in $(seq 1 50); do
        [ -s "$work/display" ] && break
        sleep 0.1
    done
    printf '%s\n' "<busconfig><type>session</type><listen>unix:tmpdir=$XDG_RUNTIME_DIR</listen>" \
        "<auth>EXTERNAL</auth><policy context=\"default\"><allow send_destination=\"*\"/>" \
        "<allow receive_sender=\"*\"/><allow own=\"*\"/></policy></busconfig>" > "$work/bus.conf"
    DISPLAY=":$(cat "$work/display")" GDK_BACKEND=x11 GSK_RENDERER=cairo \
        dbus-run-session --config-file="$work/bus.conf" -- sh -c '
        "$1/torchlight-gtk" --toggle &
        popup=$!
        for _ in $(seq 1 75); do
            xdotool search --onlyvisible --name "^Torchlight\$" >/dev/null 2>&1 && break
            sleep 0.2
        done
        xdotool search --onlyvisible --name "^Torchlight\$" >/dev/null 2>&1 || exit 1
        xdotool type --delay 50 raedme
        sleep 1
        kill -0 "$popup"' sh "$bin" || { kill "$server"; fail "the popup did not open, or quit"; }
    kill "$server"
    echo "The popup opened and took a search."
}

tar -xzf "$archive" -C "$work"
top=$(echo "$work"/torchlight-*)
"$top/install.sh"
"$bin/torchlight" --version
search raedme Projects/site/README.md
search "work notes" Documents/Work/notes
"$bin/torchlight-gtk" --help >/dev/null || fail "the popup does not start"
popup
"$top/install.sh" --uninstall
[ ! -e "$bin/torchlightd" ] || fail "uninstall left the daemon"
echo "Portable archive works on $(. /etc/os-release && echo "$PRETTY_NAME")."
