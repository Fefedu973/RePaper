#!/usr/bin/env bash
set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
sdk_dir=${REPAPER_SDK_DIR:-/opt/repaper-sdk/5.8.203}
build_dir=${1:-/root/repaper-appload-qtplugins-arm-v2}

if [[ -z "$build_dir" || "$build_dir" != /* ]]; then
    echo "Specify an absolute build directory." >&2
    exit 2
fi

set +u
source "$sdk_dir/environment-setup-cortexa53-crypto-remarkable-linux"
set -u

(cd "$source_dir" && sha256sum --check upstream.sha256)

cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$sdk_dir/sysroots/x86_64-codexsdk-linux/usr/share/cmake/Qt6Toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --parallel 4
{
    for plugin in platforms/libqlinuxfb.so sqldrivers/libqsqlite.so generic/libqevdevtablet.so; do
        "$READELF" -h "$build_dir/plugins/$plugin"
        "$READELF" -d "$build_dir/plugins/$plugin"
    done
} | tee "$build_dir/elf-report.txt"
sha256sum "$build_dir/plugins/platforms/libqlinuxfb.so" \
    "$build_dir/plugins/sqldrivers/libqsqlite.so" \
    "$build_dir/plugins/generic/libqevdevtablet.so" | tee "$build_dir/plugins.sha256"

# This instantiates the plugin object only: it never creates a platform
# integration, touches a display, or opens any input device.
"$OECORE_NATIVE_SYSROOT/usr/bin/qemu-aarch64" -L "$SDKTARGETSYSROOT" \
    -E LD_LIBRARY_PATH="$SDKTARGETSYSROOT/usr/lib:$SDKTARGETSYSROOT/lib" \
    "$build_dir/validate-plugins" "$build_dir/plugins" 2>&1 | tee "$build_dir/smoke.log"
