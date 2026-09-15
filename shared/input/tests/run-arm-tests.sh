#!/usr/bin/env bash
set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
sdk_dir=${REPAPER_SDK_DIR:-/opt/repaper-sdk/5.8.203}
build_dir=${1:-/root/repaper-tablet-mouse-tests-arm}
stage_dir=${2:-/root/repaper-appload-328-stage-v1}
if [[ "$build_dir" != /* || "$stage_dir" != /* ]]; then
    echo "Build and stage directories must be absolute." >&2
    exit 2
fi

set +u
source "$sdk_dir/environment-setup-cortexa53-crypto-remarkable-linux"
set -u
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$OECORE_NATIVE_SYSROOT/usr/share/cmake/Qt6Toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --parallel 4
mkdir -p "$build_dir/runtime"
chmod 700 "$build_dir/runtime"
sha256sum "$source_dir/../TabletMouseAdapter.h" > "$build_dir/source.sha256"

export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QT_QUICK_CONTROLS_STYLE=Basic
export QT_PLUGIN_PATH="$SDKTARGETSYSROOT/usr/lib/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$SDKTARGETSYSROOT/usr/lib/plugins/platforms"
export QML_IMPORT_PATH="$stage_dir/runtime/qml" QML2_IMPORT_PATH="$stage_dir/runtime/qml"
export QT_QPA_FONTDIR="$stage_dir/runtime/fonts"
export XDG_RUNTIME_DIR="$build_dir/runtime" REPAPER_APPLOAD_TABLET_INPUT=0
"$OECORE_NATIVE_SYSROOT/usr/bin/qemu-aarch64" -L "$SDKTARGETSYSROOT" \
    -E "LD_LIBRARY_PATH=$SDKTARGETSYSROOT/usr/lib:$SDKTARGETSYSROOT/lib" \
    "$build_dir/tablet-mouse-adapter-tests" -o "$build_dir/test-results.txt",txt \
    -o "$build_dir/test-results.xml",junitxml
cat "$build_dir/test-results.txt"
