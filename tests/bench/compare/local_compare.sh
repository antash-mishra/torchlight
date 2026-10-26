#!/bin/sh
# Runs the launcher comparison on this computer instead of on Boat, for timings on
# real hardware. Docker needs root here, so every launcher runs on the host with the
# same inputs as the images, inside an unprivileged user and mount namespace whose
# private /home holds the synthetic home at /home/user, as in the containers; the
# real home is hidden from it and never touched. Each launcher and Torchlight take
# turns over three rounds, who goes first alternating, pinned to the same two CPUs.
# Downloads are checked against downloads.sha256 or by apt. Nothing is installed.
# Usage: local_compare.sh WORK TORCHLIGHTD BENCH_FIXTURE
#   WORK must be outside /home (the namespace hides /home); results land in
#   WORK/results/<launcher>/ for report.py. The two binaries are the release
#   daemon and the fixture exporter (build/torchlightd-release, build/bench_fixture).
set -eu
work=$1
daemon=$2
fixture=$3
here=$(cd "$(dirname "$0")" && pwd)
launchers=${LAUNCHERS:-gnome ulauncher rofi fzf plocate}  # LAUNCHERS=rofi reruns one
rounds=3
cpus=2,3
utf8proc=${UTF8PROC_DIR:-$HOME/.local/lib/torchlight}

case $work in /home/*) echo "WORK must be outside /home" >&2; exit 2 ;; esac
context=$work/context
tools=$work/tools
rm -rf "$context"
for launcher in $launchers; do rm -rf "$work/results/$launcher"; done
mkdir -p "$context/lib" "$tools/debs" "$tools/root" "$tools/bin" "$work/results"

# The images' inputs (box_images.sh), plus the daemon's libutf8proc.
LD_LIBRARY_PATH="$utf8proc" "$fixture" 50000 "$context/paths" "$context/queries.jsonl"
cp -r "$here/apps" "$here/everyday_queries.tsv" "$here/run_compare.py" "$context/"
cp "$here/../bench_daemon.py" "$context/"
cp "$daemon" "$context/torchlightd"
cp -P "$utf8proc"/libutf8proc.so* "$context/lib/"
git -C "$here" rev-parse --short HEAD > "$context/commit"

# The launchers that are not installed: fzf and Ulauncher from their releases,
# rofi and Ulauncher's Python libraries from Ubuntu, unpacked rather than installed.
curl -fsSLo "$tools/debs/ulauncher_5.16.2_all.deb" \
    https://github.com/Ulauncher/Ulauncher/releases/download/5.16.2/ulauncher_5.16.2_all.deb
curl -fsSLo "$tools/debs/fzf-0.74.5-linux_amd64.tar.gz" \
    https://github.com/junegunn/fzf/releases/download/v0.74.5/fzf-0.74.5-linux_amd64.tar.gz
(cd "$tools/debs" && sha256sum -c "$here/downloads.sha256")
(cd "$tools/debs" && apt-get download rofi libxcb-ewmh2 python3-levenshtein python3-rapidfuzz >/dev/null)
for deb in "$tools"/debs/*.deb; do
    dpkg -x "$deb" "$tools/root"
done
tar -xzf "$tools/debs/fzf-0.74.5-linux_amd64.tar.gz" -C "$tools/bin" fzf

# Inside the namespace: a private /home with the synthetic home, plocate's index of
# it, then the rounds. The socket directory stays short (sun_path limit).
state=${XDG_RUNTIME_DIR:-/tmp}/torchlight-compare-local
unshare --user --map-root-user --mount env \
    WORK="$work" CONTEXT="$context" TOOLS="$tools" STATE="$state" LAUNCHERS="$launchers" \
    ROUNDS="$rounds" CPUS="$cpus" sh -eu -c '
    mount -t tmpfs compare-home /home
    mkdir -p /home/user "$STATE"
    export HOME=/home/user LANG=C.UTF-8 XDG_CURRENT_DESKTOP=GNOME
    export XDG_DATA_DIRS="$CONTEXT/apps" XDG_DATA_HOME=/nonexistent-data-home
    export PATH="$TOOLS/bin:$TOOLS/root/usr/bin:/usr/local/bin:/usr/bin:/bin"
    export PYTHONPATH="$TOOLS/root/usr/lib/python3/dist-packages"
    export LD_LIBRARY_PATH="$CONTEXT/lib:$TOOLS/root/usr/lib/x86_64-linux-gnu"
    export COMPARE_DATA="$CONTEXT" COMPARE_STATE="$STATE/torchlight"
    export COMPARE_TORCHLIGHTD="$CONTEXT/torchlightd" COMPARE_PLOCATE_DB="$STATE/plocate.db"
    python3 "$CONTEXT/run_compare.py" materialize
    updatedb --database-root /home/user --output "$COMPARE_PLOCATE_DB" --require-visibility no
    run() {
        mkdir -p "$WORK/results/$3"
        taskset -c "$CPUS" python3 "$CONTEXT/run_compare.py" run "$1" --round "$2" \
            --out "$WORK/results/$3/$1-round$2.json"
    }
    for launcher in $LAUNCHERS; do
        mkdir -p "$WORK/results/$launcher"
        { lscpu; echo "Load average before: $(cat /proc/loadavg)"; } > "$WORK/results/$launcher/lscpu.txt"
        round=1
        while [ "$round" -le "$ROUNDS" ]; do
            if [ $((round % 2)) -eq 1 ]; then
                run "$launcher" "$round" "$launcher"; run torchlight "$round" "$launcher"
            else
                run torchlight "$round" "$launcher"; run "$launcher" "$round" "$launcher"
            fi
            round=$((round + 1))
        done
    done
'
rm -rf "$state"
echo "LOCAL DONE"
