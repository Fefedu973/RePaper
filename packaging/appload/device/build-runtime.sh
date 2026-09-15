#!/usr/bin/env bash
# Local ARM build only. Never contacts a tablet or modifies the reviewed AppLoad checkout.
set -euo pipefail
if (( $# < 1 || $# > 2 )); then
    printf 'Usage: bash build-runtime.sh APPLOAD_SOURCE [BUILD_DIRECTORY]\n' >&2
    exit 2
fi
runtime_source=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
appload_source=$(cd -- "$1" && pwd)
build_root=${2:-/root/repaper-appload-device-runtime}
sdk_root=${REPAPER_SDK_ROOT:-/opt/repaper-sdk/5.8.203}
mkdir -p -- "$build_root"
build_root=$(cd -- "$build_root" && pwd)
if [[ $build_root == "$runtime_source" || $build_root == "$appload_source" ]]; then
    printf 'Build directory must be separate from source directories.\n' >&2
    exit 2
fi
# No recursive delete: build the shipping shim from a fresh, private export.
shipping_source="$build_root/shim-source"
if [[ -e $shipping_source ]]; then
    printf 'Shipping shim source already exists; choose a new build directory: %s\n' "$shipping_source" >&2
    exit 2
fi
mkdir -- "$shipping_source"
git -C "$appload_source" archive HEAD | tar -x -C "$shipping_source"
git -C "$shipping_source" apply --ignore-space-change --include='shim/src/input-shim.cpp' \
    "$runtime_source/stylus-input.patch"
git -C "$shipping_source" apply --ignore-space-change "$runtime_source/shim-private-input.patch"

set +u
# shellcheck source=/dev/null
source "$sdk_root/environment-setup-cortexa53-crypto-remarkable-linux"
set -u
sdk_run() {
    env LOCPATH="$OECORE_NATIVE_SYSROOT/usr/lib/locale" LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 "$@"
}
sdk_run cmake -S "$runtime_source" -B "$build_root/arm" \
    -DAPPLOAD_SOURCE:PATH="$appload_source" -DCMAKE_BUILD_TYPE=Release
sdk_run cmake --build "$build_root/arm" --parallel "${REPAPER_BUILD_JOBS:-2}"
sdk_run cmake -S "$shipping_source/shim" -B "$build_root/shim-arm" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17
sdk_run cmake --build "$build_root/shim-arm" --target qtfb-shim --parallel "${REPAPER_BUILD_JOBS:-2}"
mkdir -p -- "$build_root/runtime"
cp -- "$build_root/arm/repaper-appload-launch" "$build_root/arm/qt-linuxfb-refresh" \
    "$build_root/shim-arm/qtfb-shim.so" "$build_root/runtime/"
sha256sum "$build_root/runtime/repaper-appload-launch" \
    "$build_root/runtime/qt-linuxfb-refresh" "$build_root/runtime/qtfb-shim.so" > "$build_root/SHA256SUMS"
printf 'ARM supervisor, refresh helper and patched shim: %s/runtime\n' "$build_root"
