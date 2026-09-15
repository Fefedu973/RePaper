#!/usr/bin/env bash
# Build ARM artifacts locally. This script never connects to or installs on a tablet.
set -euo pipefail

if [[ ${1-} == --help || ${1-} == -h ]]; then
    printf 'Usage: bash tools/build-arm.sh [build-directory]\n'
    printf 'Defaults: ~/repaper-arm; SDK /opt/repaper-sdk/5.8.203; 4 parallel jobs.\n'
    printf 'Overrides: REPAPER_ARM_BUILD_DIR, REPAPER_SDK_ROOT, REPAPER_BUILD_JOBS.\n'
    exit 0
fi
if (( $# > 1 )); then
    printf 'Expected at most one build directory.\n' >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
sdk_root=${REPAPER_SDK_ROOT:-/opt/repaper-sdk/5.8.203}
setup="$sdk_root/environment-setup-cortexa53-crypto-remarkable-linux"
build_dir=${1:-${REPAPER_ARM_BUILD_DIR:-$HOME/repaper-arm}}
jobs=${REPAPER_BUILD_JOBS:-4}

if [[ ! -r $setup ]]; then
    printf 'Missing SDK environment: %s\n' "$setup" >&2
    exit 2
fi
if [[ ! $jobs =~ ^[1-9][0-9]*$ ]]; then
    printf 'REPAPER_BUILD_JOBS must be a positive integer.\n' >&2
    exit 2
fi
mkdir -p -- "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
if [[ $build_dir == "$project_dir" ]]; then
    printf 'Use a separate ARM build directory, not the source root.\n' >&2
    exit 2
fi

# Yocto environment scripts intentionally refer to optional unset variables.
set +u
# shellcheck source=/dev/null
source "$setup"
set -u

# The SDK's native Qt tools use its bundled glibc/locale data. Passing the
# matching locale per command avoids changing the parent shell's locale.
sdk_run() {
    env LOCPATH="$OECORE_NATIVE_SYSROOT/usr/lib/locale" LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 "$@"
}

sdk_run cmake -S "$project_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
sdk_run cmake --build "$build_dir" --parallel "$jobs"

artifacts=(
    "$build_dir/bridge/paper-bridge"
    "$build_dir/apps/remoodle/remoodle"
    "$build_dir/apps/reagenda/reagenda"
    "$build_dir/apps/restencil/restencil"
    "$build_dir/apps/recalc/recalc"
    "$build_dir/apps/reink/reink"
)
for artifact in "${artifacts[@]}"; do
    if [[ ! -f $artifact ]] || ! LC_ALL=C readelf -h "$artifact" | grep -q 'Machine:.*AArch64'; then
        printf 'Missing or non-AArch64 artifact: %s\n' "$artifact" >&2
        exit 3
    fi
done

LC_ALL=C file "${artifacts[@]}" | tee "$build_dir/arm-artifacts.txt"
sha256sum "${artifacts[@]}" > "$build_dir/SHA256SUMS"
{
    printf 'SDK: %s\n' "$sdk_root"
    cat "$sdk_root/version-cortexa53-crypto-remarkable-linux"
    printf 'Qt: '
    sdk_run qtpaths --qt-version
    printf 'Build: Release; BUILD_TESTING=OFF; local compilation only\n'
} > "$build_dir/arm-build-info.txt"
printf 'Artifact formats and SHA-256 manifest saved in %s\n' "$build_dir"
