#!/bin/sh
# Runs the launcher comparison (docs/comparison.md) on Boat. This computer only
# issues boat commands: a base box builds, tests and lints Torchlight, builds one
# Docker image per launcher and smoke-tests them; it is then stopped and forked
# into one box per launcher, where that launcher and Torchlight take turns on the
# same hardware. Results land in build/compare/results/<launcher>/, the tables in
# build/compare/report.md, and every box is deleted at the end, also on failure.
# Usage, from the repository root: tests/bench/compare/boat_compare.sh
set -eu
launchers="gnome ulauncher rofi fzf plocate"
hours=3
work=build/compare
commit="$(git rev-parse --short HEAD)$(git diff --quiet HEAD || echo -dirty)"
boxes=""

cleanup() {
    for id in $boxes; do
        boat delete "$id" --yes >/dev/null 2>&1 || echo "could not delete box $id" >&2
    done
}
trap cleanup EXIT INT TERM

# Run a command on a box in the background and wait for it, printing its output
# and returning its exit code (boat exec alone stops waiting after 600 s).
run_on() {
    pid=$(boat exec "$1" --json --detach "$2" | jq -r 'select(.pid) | .pid')
    boat exec "$1" --attach "$pid"
}

mkdir -p "$work/logs"
base=$(boat new --json --name tl-compare-base | jq -r 'select(.event == "ready") | .id')
boxes=$base
boat extend "$base" --hours "$hours" >/dev/null
boat push "$base" . >/dev/null
run_on "$base" "sh torchlight/tests/bench/compare/box_build.sh" > "$work/logs/base-build.log"
run_on "$base" "sh torchlight/tests/bench/compare/box_images.sh $commit" > "$work/logs/base-images.log"
boat stop "$base" >/dev/null

for launcher in $launchers; do
    id=$(boat fork "$base" --wait --json | jq -r 'select(.event == "ready") | .id')
    boxes="$boxes $id"
    boat extend "$id" --hours "$hours" >/dev/null
    pid=$(boat exec "$id" --json --detach "sh torchlight/tests/bench/compare/box_run.sh $launcher" |
        jq -r 'select(.pid) | .pid')
    eval "box_$launcher=$id pid_$launcher=$pid"
done

for launcher in $launchers; do
    eval "id=\$box_$launcher pid=\$pid_$launcher"
    boat exec "$id" --attach "$pid" > "$work/logs/$launcher.log"
    rm -rf "$work/results/$launcher"
    mkdir -p "$work/results/$launcher"
    boat scp -r "$id:results/." "$work/results/$launcher/"
done

python3 tests/bench/compare/report.py "$work/results" "$work/report"
