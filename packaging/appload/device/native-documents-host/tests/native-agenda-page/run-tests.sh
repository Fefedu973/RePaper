#!/usr/bin/env bash
# Local host or QEMU tests only; FONT_FILE is a private native-resource cache.
set -eo pipefail
[[ $# -eq 4 ]] || { echo 'Usage: run-tests.sh host|arm NEW_BUILD_DIR FONT_FILE PREVIEW_PNG' >&2; exit 2; }
task_arch=$1
task_build=$2
task_font=$3
task_preview=$4
task_source=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[[ "$task_build" = /* && -f "$task_font" ]] || exit 2
mkdir -p "$task_build"
if [[ "$task_arch" == arm ]]; then
    source "${REPAPER_SDK_DIR:-/opt/repaper-sdk/5.8.203}/environment-setup-cortexa53-crypto-remarkable-linux"
    export LOCPATH="$OECORE_NATIVE_SYSROOT/usr/lib/locale"
    export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
elif [[ "$task_arch" != host ]]; then
    exit 2
fi
cmake -S "$task_source" -B "$task_build" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$task_build/configure.log" 2>&1
cmake --build "$task_build" --parallel "${REPAPER_BUILD_JOBS:-3}" > "$task_build/build.log" 2>&1
if [[ "$task_arch" == arm ]]; then
    "$OECORE_NATIVE_SYSROOT/usr/bin/qemu-aarch64" -L "$OECORE_TARGET_SYSROOT" \
        -E "LD_LIBRARY_PATH=$OECORE_TARGET_SYSROOT/usr/lib" \
        -E "QT_PLUGIN_PATH=$OECORE_TARGET_SYSROOT/usr/lib/plugins" \
        -E QT_QPA_PLATFORM=offscreen -E "REPAPER_AGENDA_TEST_FONT=$task_font" \
        -E "REPAPER_AGENDA_TEST_PREVIEW=$task_preview" \
        "$task_build/native-agenda-page-tests" -o "$task_build/test-results.xml",xml \
        -o "$task_build/test-results.txt",txt -o -,txt
else
    env QT_QPA_PLATFORM=offscreen REPAPER_AGENDA_TEST_FONT="$task_font" \
        REPAPER_AGENDA_TEST_PREVIEW="$task_preview" \
        "$task_build/native-agenda-page-tests" -o "$task_build/test-results.xml",xml \
        -o "$task_build/test-results.txt",txt -o -,txt
fi
