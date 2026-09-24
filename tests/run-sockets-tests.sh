#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
# The production service intentionally only registers in the xochitl process.
${CXX:-c++} -std=c++17 -fPIC -pthread $(pkg-config --cflags Qt6Core Qt6Network) \
    "$root/tests/sockets_tests.cpp" "$root/src/sockets.cpp" \
    $(pkg-config --libs Qt6Core Qt6Network) -o "$build/xochitl"
"$build/xochitl"
cp "$build/xochitl" "$build/xochitl_pdf_renderer"
"$build/xochitl_pdf_renderer"
