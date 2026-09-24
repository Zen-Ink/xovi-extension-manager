#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python3 "$root/tests/check-broker-exports.py"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT

${CXX:-c++} -std=c++17 -fPIC -pthread \
    $(pkg-config --cflags Qt6Core) \
    "$root/tests/notifications_tests.cpp" \
    "$root/src/notifications.cpp" \
    $(pkg-config --libs Qt6Core) \
    -o "$build/notifications-tests"
"$build/notifications-tests"
