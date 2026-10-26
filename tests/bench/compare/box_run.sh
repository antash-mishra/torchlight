#!/bin/sh
# Runs on one forked box of the comparison: LAUNCHER and Torchlight take turns over
# three rounds (who goes first alternates), on the same two CPUs and memory limit,
# with no network. Results and the box's CPU description go to /home/user/results.
# Usage: box_run.sh LAUNCHER
set -eu
launcher=$1
rounds=3
out=/home/user/results
mkdir -p "$out"
until docker info >/dev/null 2>&1; do sleep 1; done
lscpu > "$out/lscpu.txt"

run() {
    docker run --rm --network=none --cpuset-cpus=0,1 --memory=4g -v "$out:/out" \
        "compare-$1" python3 /opt/compare/run_compare.py run "$1" --round "$2" \
        --out "/out/$1-round$2.json"
}

round=1
while [ "$round" -le "$rounds" ]; do
    if [ $((round % 2)) -eq 1 ]; then
        run "$launcher" "$round"
        run torchlight "$round"
    else
        run torchlight "$round"
        run "$launcher" "$round"
    fi
    round=$((round + 1))
done
echo "RUN DONE"
