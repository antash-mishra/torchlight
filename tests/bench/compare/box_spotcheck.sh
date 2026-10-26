#!/bin/sh
# Runs on a Boat box: opens the real rofi (drun) and Ulauncher windows on the
# comparison's app fixture under Xvfb and screenshots five queries in each, to
# check by eye that run_compare.py's adapters list what the windows show.
# Screenshots go to /home/user/spotcheck.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
queries="term chrome firefox settings calc"
shots=/home/user/spotcheck
mkdir -p "$shots"

sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends rofi \
    imagemagick xdotool xvfb dbus-x11 python3-gi gir1.2-glib-2.0 shared-mime-info \
    librsvg2-common >/dev/null
curl -fsSLo /tmp/ulauncher_5.16.2_all.deb \
    https://github.com/Ulauncher/Ulauncher/releases/download/5.16.2/ulauncher_5.16.2_all.deb
(cd /tmp && grep ulauncher "$here/downloads.sha256" | sha256sum -c)
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq /tmp/ulauncher_5.16.2_all.deb >/dev/null

# The same entries and program stubs as the images (Dockerfile.base).
sudo mkdir -p /opt/compare
sudo cp -r "$here/apps" /opt/compare/
sudo python3 -c "import sys; sys.path.insert(0, '$here'); import run_compare; run_compare.stub_programs()"
# GTK finds its MIME and icon data through XDG_DATA_DIRS too; link them in rather
# than add /usr/share, whose applications folder would add the box's own apps.
sudo ln -sfn /usr/share/mime /opt/compare/apps/mime
sudo ln -sfn /usr/share/icons /opt/compare/apps/icons
export XDG_DATA_DIRS=/opt/compare/apps XDG_DATA_HOME=/tmp/empty-data XDG_CURRENT_DESKTOP=GNOME
export DISPLAY=:99
Xvfb :99 -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
sleep 2

for query in $queries; do
    rofi -show drun -filter "$query" >/dev/null 2>&1 &
    sleep 2
    import -window root "$shots/rofi-$query.png"
    kill $! 2>/dev/null || true
    sleep 0.5
done

dbus-run-session -- sh -c "
    ulauncher --hide-window >/tmp/ulauncher.log 2>&1 &
    sleep 8
    for query in $queries; do
        ulauncher-toggle
        sleep 1.5
        xdotool type --delay 60 \"\$query\"
        sleep 2
        import -window root \"$shots/ulauncher-\$query.png\"
        xdotool key Escape
        sleep 1
    done
"
ls "$shots"
