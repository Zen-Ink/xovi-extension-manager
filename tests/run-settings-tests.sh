#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/xem-settings-tests.XXXXXX")
trap 'rm -rf "$build"' EXIT
(cd "$root" && python3 "$root/xovi/util/xovigen.py" -o xovi.cpp -H xovi.h xovi-extension-manager.xovi)
# Qt Core is only needed for settings/protocol tests; inventory tests remain standalone.
${CXX:-c++} -std=c++17 -fPIC -I"$root" $(pkg-config --cflags Qt6Core) \
    "$root/tests/settings_tests.cpp" \
    "$root/src/settings.cpp" "$root/src/inventory.cpp" \
    "$root/src/jsonutil.cpp" $(pkg-config --libs Qt6Core) -o "$build/settings-tests"
"$build/settings-tests"
