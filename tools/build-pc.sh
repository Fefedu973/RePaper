#!/usr/bin/env bash
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="${REPAPER_BUILD_DIR:-$HOME/repaper-build}"
rmchat="${REPAPER_BUILD_RMCHAT:-OFF}"
if [[ "$rmchat" != OFF && "$rmchat" != ON ]]; then
    printf 'REPAPER_BUILD_RMCHAT must be OFF (default) or ON (archived research build).\n' >&2
    exit 2
fi

if [[ "${1:-}" == "--install-deps" ]]; then
    # Ubuntu 22.04/24.04; call explicitly when preparing a new WSL distribution.
    apt-get update
    apt-get install -y build-essential cmake ninja-build git python3 pkg-config \
        qt6-base-dev qt6-declarative-dev qt6-tools-dev-tools \
        qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
        qml6-module-qtquick-templates qml6-module-qtquick-window \
        qml6-module-qtqml-workerscript libqt6sql6-sqlite libqt6svg6 libssl-dev zlib1g-dev libboost-dev
fi

git -C "$repo" submodule update --init external/pdfio
mkdir -p -- "$build"
build="$(cd -- "$build" && pwd)"
# Pass OFF explicitly so an older CMake cache cannot restore the archived app.
rmchat_options=(-DREPAPER_BUILD_RMCHAT="$rmchat" -DREPAPER_BUILD_RMCHAT_PROBE=OFF -DRMCHAT_CORE_BINARY=)
if [[ "$rmchat" == ON ]]; then
    printf 'Building archived RMChat by explicit opt-in; see apps/rmchat/ARCHIVED.md.\n'
    bash "$repo/tools/build-rmchat-core.sh" "$build/rmchat-core" amd64
    rmchat_options+=(-DRMCHAT_CORE_BINARY="$build/rmchat-core")
fi
cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
    "${rmchat_options[@]}"
cmake --build "$build" --parallel "${REPAPER_JOBS:-4}"
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
    ctest --test-dir "$build" --output-on-failure
printf 'Applications disponibles dans %s/apps\n' "$build"
