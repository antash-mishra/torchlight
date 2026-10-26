#!/bin/sh
# Runs on the comparison's base Boat box after box_build.sh. Gathers the inputs every
# image shares (corpus, queries, app fixture, runner, the release daemon and the
# checked downloads), builds one image per launcher, keeps each image with a stopped
# container so forks of this box still have it, and smoke-tests every launcher.
# Usage: box_images.sh COMMIT
set -eu
commit=$1
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
context=/home/user/compare-context
launchers="torchlight gnome ulauncher rofi fzf plocate"

rm -rf "$context"
mkdir -p "$context"
"$repo/build/bench_fixture" 50000 "$context/paths" "$context/queries.jsonl"
cp -r "$here/apps" "$here/everyday_queries.tsv" "$here/run_compare.py" "$context/"
cp "$repo/tests/bench/bench_daemon.py" "$context/"
cp "$repo/build/torchlightd-release" "$context/torchlightd"
echo "$commit" > "$context/commit"
curl -fsSLo "$context/ulauncher_5.16.2_all.deb" \
    https://github.com/Ulauncher/Ulauncher/releases/download/5.16.2/ulauncher_5.16.2_all.deb
curl -fsSLo "$context/fzf-0.74.5-linux_amd64.tar.gz" \
    https://github.com/junegunn/fzf/releases/download/v0.74.5/fzf-0.74.5-linux_amd64.tar.gz
(cd "$context" && sha256sum -c "$here/downloads.sha256")

until docker info >/dev/null 2>&1; do sleep 1; done
docker build -q -t compare-base -f "$here/Dockerfile.base" "$context"
for launcher in $launchers; do
    docker build -q -t "compare-$launcher" -f "$here/Dockerfile.$launcher" "$context"
    docker rm -f "keep-$launcher" >/dev/null 2>&1 || true
    docker create --name "keep-$launcher" "compare-$launcher" true >/dev/null
done
echo "IMAGES DONE"

mkdir -p /home/user/smoke
for launcher in $launchers; do
    docker run --rm --network=none -v /home/user/smoke:/out "compare-$launcher" \
        python3 /opt/compare/run_compare.py run "$launcher" --round 0 --smoke \
        --out "/out/$launcher.json"
done
echo "SMOKE DONE"
