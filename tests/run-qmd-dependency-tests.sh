#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${TMPDIR:-/tmp}/xovi-extension-manager-qmd-tests

mkdir -p "$build_dir"

${CXX:-c++} \
    -std=c++17 \
    -D_GNU_SOURCE \
    -I"$project_dir/tests/stubs" \
    -I"$project_dir/src" \
    "$project_dir/tests/qmd_dependency_tests.cpp" \
    "$project_dir/src/inventory.cpp" \
    "$project_dir/src/jsonutil.cpp" \
    -o "$build_dir/qmd_dependency_tests"

"$build_dir/qmd_dependency_tests"
