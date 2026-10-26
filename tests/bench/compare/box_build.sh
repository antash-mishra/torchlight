#!/bin/sh
# Runs on the comparison's base Boat box: installs the same build dependencies as
# the release workflow, builds the release daemon and the benchmark fixture, and
# runs the unit tests and lint. Output goes to the log given by the caller.
set -eu
cd "$(dirname "$0")/../../.."

sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential pkg-config libsqlite3-dev libutf8proc-dev libgtk-4-dev \
    libglib2.0-dev libx11-dev dpkg-dev clang-tidy cppcheck dbus-daemon \
    dbus-session-bus-common
# The box's preinstalled Rust (1.97) meets the 1.89+ requirement.
cargo --version

make -j"$(nproc)" build/torchlightd-release build/bench_fixture
echo "BUILD DONE"

# AddressSanitizer needs the address-space randomization of older kernels.
sudo sysctl -w vm.mmap_rnd_bits=28
# Boat's user holds an ambient cap_dac_read_search, which lets it read the
# folders the desktop tests make unreadable; run the tests without it.
setpriv --inh-caps=-all --ambient-caps=-all make test
echo "TEST DONE"
make lint
echo "LINT DONE"
