#!/bin/sh
# Install Torchlight from this folder for the person running it, without root (ADR 0056).
# The programs go into ~/.local/bin; the search daemon's systemd user unit, the launcher's
# desktop entry, the popup's fonts and the GNOME Shell and Cinnamon extensions that list
# open windows go into ~/.local/share (or XDG_DATA_HOME). The daemon starts now and at each
# login: through systemd when the session has a user manager, otherwise through an
# autostart entry. Running it from a newer release updates Torchlight in place;
# --uninstall removes it and keeps the index, history and settings.
# usage: ./install.sh [--uninstall]
set -eu

here=$(cd "$(dirname "$0")" && pwd)
bin_dir=$HOME/.local/bin
data_dir=${XDG_DATA_HOME:-$HOME/.local/share}
config_dir=${XDG_CONFIG_HOME:-$HOME/.config}
programs="torchlight torchlightd torchlight-gtk"
extension=torchlight@antash-mishra.github.io
unit=$data_dir/systemd/user/torchlightd.service
own_unit=$config_dir/systemd/user/torchlightd.service
entry=$data_dir/applications/org.torchlight.Launcher.desktop
autostart=$config_dir/autostart/torchlightd.desktop
fonts=$data_dir/fonts/torchlight
setup=https://github.com/antash-mishra/torchlight/blob/master/docs/desktop-setup.md
# Seconds to wait for a stopped daemon to let go of its socket.
STOP_WAIT=15

fail() {
    printf 'install.sh: %s\n' "$*" >&2
    exit 1
}

# Whether this session has a systemd user manager to run the daemon.
has_systemd() {
    command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1
}

# Stops this person's running copies of a program and waits for them to exit.
stop_own() {
    command -v pkill >/dev/null 2>&1 || return 0
    pkill -u "$(id -u)" -x "$1" 2>/dev/null || return 0
    waited=0
    while pgrep -u "$(id -u)" -x "$1" >/dev/null 2>&1 && [ "$waited" -lt "$STOP_WAIT" ]; do
        sleep 1
        waited=$((waited + 1))
    done
}

# Every library and symbol the programs need must come from this system; names what is
# missing and how to get it.
check_libraries() {
    command -v ldd >/dev/null 2>&1 || return 0
    missing=$(for program in $programs; do LC_ALL=C ldd -r "$here/bin/$program" 2>&1; done |
              grep -E 'not found|undefined symbol' | sort -u || true)
    [ -z "$missing" ] && return 0
    printf '%s\n' "Torchlight cannot run here yet. Missing:" "$missing" "" >&2
    case $missing in
    *GLIBC_*) fail "this system's C library is older than glibc 2.39, which Torchlight needs" ;;
    esac
    printf '%s\n' "Install GTK 4 (4.14 or newer) with your package manager, for example:" \
        "    sudo dnf install gtk4            (Fedora)" \
        "    sudo pacman -S gtk4              (Arch)" \
        "    sudo zypper install libgtk-4-1   (openSUSE)" \
        "    sudo apt install libgtk-4-1      (Debian, Ubuntu)" >&2
    exit 1
}

check_programs() {
    check_libraries
    "$here/bin/torchlight" --version >/dev/null 2>&1 ||
        fail "$here/bin/torchlight does not run here; this build is for 64-bit PCs (x86_64)"
}

# Each program is copied beside the old one and renamed over it, so a running daemon or
# popup keeps its own file; the popup restarts onto the new one the next time it hides.
install_programs() {
    mkdir -p "$bin_dir"
    for program in $programs; do
        cp "$here/bin/$program" "$bin_dir/.$program.new"
        chmod 755 "$bin_dir/.$program.new"
        mv -f "$bin_dir/.$program.new" "$bin_dir/$program"
    done
}

# The desktop entry names the popup by its full path, since the desktop session's PATH may
# not include ~/.local/bin. Fonts and the shell extensions are copied as they are.
install_files() {
    mkdir -p "$(dirname "$entry")" "$fonts"
    sed "s|^Exec=.*|Exec=\"$bin_dir/torchlight-gtk\" --toggle|" \
        "$here/share/applications/org.torchlight.Launcher.desktop" > "$entry"
    cp "$here/share/fonts/torchlight/"* "$fonts/"
    for shell in gnome-shell cinnamon; do
        mkdir -p "$data_dir/$shell/extensions/$extension"
        cp "$here/share/$shell/extensions/$extension/"* "$data_dir/$shell/extensions/$extension/"
    done
    if command -v fc-cache >/dev/null 2>&1; then
        fc-cache "$fonts" >/dev/null 2>&1 || true
    fi
}

# The daemon (re)starts now, so an update takes effect at once, and at each login. Only one
# way of starting it is kept, so two daemons never compete for the same index.
start_daemon() {
    if has_systemd; then
        if [ -e "$autostart" ]; then
            rm -f "$autostart"
            stop_own torchlightd
        fi
        mkdir -p "$(dirname "$unit")"
        cp "$here/share/systemd/user/torchlightd.service" "$unit"
        systemctl --user daemon-reload
        systemctl --user --quiet enable torchlightd.service
        systemctl --user restart torchlightd.service ||
            printf '%s\n' "The search daemon did not start; see systemctl --user status torchlightd" >&2
        return 0
    fi
    rm -f "$unit"
    mkdir -p "$(dirname "$autostart")"
    printf '%s\n' "[Desktop Entry]" "Type=Application" "Name=Torchlight search daemon" \
        "Exec=\"$bin_dir/torchlightd\"" "NoDisplay=true" > "$autostart"
    stop_own torchlightd
    nohup "$bin_dir/torchlightd" </dev/null >/dev/null 2>&1 &
}

next_steps() {
    printf '%s\n' "Installed $("$bin_dir/torchlight" --version) in $bin_dir." "" \
        "Now bind a keyboard shortcut, such as Super+Space, to this command:" \
        "    \"$bin_dir/torchlight-gtk\" --toggle" \
        "Your desktop's keyboard settings have a place for custom shortcuts; this page says" \
        "where on each desktop: $setup"
    case ":$PATH:" in
    *":$bin_dir:"*) ;;
    *) printf '\n%s\n' "To use the torchlight command in a terminal, add $bin_dir to your PATH." ;;
    esac
    case "${XDG_CURRENT_DESKTOP:-}:${XDG_SESSION_TYPE:-}" in
    *GNOME*:wayland)
        printf '\n%s\n' "Log out and back in once, so GNOME finds the extension that lists open windows." ;;
    esac
    if [ -x /usr/bin/torchlightd ]; then
        printf '\n%s\n' "Torchlight is also installed for the whole system; this copy takes its place for you."
    fi
    if [ -e "$own_unit" ]; then
        printf '\n%s\n' "Your own $own_unit comes before the installed unit; systemd runs the daemon it names."
    fi
}

uninstall() {
    if has_systemd && [ -e "$unit" ]; then
        systemctl --user --quiet disable --now torchlightd.service || true
    fi
    stop_own torchlightd
    stop_own torchlight-gtk
    rm -f "$unit" "$autostart" "$entry"
    for program in $programs; do
        rm -f "$bin_dir/$program"
    done
    rm -rf "$fonts" "$data_dir/gnome-shell/extensions/$extension" \
        "$data_dir/cinnamon/extensions/$extension"
    if has_systemd; then
        systemctl --user daemon-reload
    fi
    printf '%s\n' "Torchlight is removed. Your index and history are still in $data_dir/torchlight" \
        "and your settings in $config_dir/torchlight; delete those folders if you want them gone."
}

[ "$(id -u)" -ne 0 ] || fail "run install.sh as yourself, without sudo: it installs Torchlight just for you"
# The path goes into a desktop entry and a sed replacement.
case $HOME in
*[\"\`\$\\%\|\&]*) fail "your home folder's path ($HOME) has a character this script cannot handle" ;;
esac
case ${1:-} in
"")
    check_programs
    install_programs
    install_files
    start_daemon
    next_steps
    ;;
--uninstall) uninstall ;;
-h | --help) printf '%s\n' "usage: ./install.sh [--uninstall]" ;;
*) fail "unknown option $1; usage: ./install.sh [--uninstall]" ;;
esac
