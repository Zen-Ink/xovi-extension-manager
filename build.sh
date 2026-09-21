#!/usr/bin/env bash
set -eo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
export XOVI_REPO=${XOVI_REPO:-$project/xovi}
toolchains=${TOOLCHAIN_ROOT:-/run/media/lurenmax/Data/work/4_workspace/remarkable/toolchian}
case "${1:-all}" in
    all) targets=(aarch64 arm32);;
    aarch64|arm32) targets=("$1");;
    *) echo "Usage: $0 [all|aarch64|arm32]" >&2; exit 2;;
esac
for target in "${targets[@]}"; do (
    if [ "$target" = aarch64 ]; then
        source "$toolchains/rmpp/environment-setup-cortexa53-crypto-remarkable-linux"
    else
        source "$toolchains/rm2/environment-setup-cortexa7hf-neon-remarkable-linux-gnueabi"
    fi
    make -C "$project" clean
    make -C "$project" -j"${JOBS:-4}"
    mkdir -p "$project/build/$target"
    cp "$project/xovi-extension-manager.so" "$project/manifest.json" "$project/build/$target/"
); done
