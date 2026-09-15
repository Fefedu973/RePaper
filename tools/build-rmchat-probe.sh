#!/usr/bin/env bash
set -euo pipefail

# Native Linux PC build. Set GO_BIN/CMAKE_BIN when tools are outside PATH.
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${repo_root}/.local/build/rmchat-probe}"
go_bin="${GO_BIN:-go}"
cmake_bin="${CMAKE_BIN:-cmake}"
command -v "${go_bin}" >/dev/null
command -v "${cmake_bin}" >/dev/null
mkdir -p -- "${build_dir}"
build_dir="$(cd -- "${build_dir}" && pwd)"
(
    cd -- "${repo_root}/apps/rmchat/core"
    "${go_bin}" test ./...
    CGO_ENABLED=0 "${go_bin}" build -trimpath -o "${build_dir}/rmchat-core" ./cmd/rmchat-core
)
"${cmake_bin}" -S "${repo_root}/apps/rmchat" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
"${cmake_bin}" --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --output-on-failure
"${build_dir}/rmchat-probe" --vault-dir "${build_dir}/smoke-vault" status
