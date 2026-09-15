#!/usr/bin/env bash
# Build the Linux core locally, using native Go or the installed Windows Go in WSL.
set -euo pipefail

if [[ ${1-} == --help || ${1-} == -h ]]; then
    printf 'Usage: bash tools/build-rmchat-core.sh /absolute/output/rmchat-core [amd64|arm64]\n'
    printf 'Default target: linux/amd64. GO_BIN selects a native Linux Go executable.\n'
    printf 'Without native Go, WSL uses the installed Windows Go through a fixed PowerShell helper.\n'
    printf 'The Go 1.25+ toolchain and module dependencies must already be installed/cached.\n'
    exit 0
fi
if (( $# < 1 || $# > 2 )) || [[ $1 != /* || $1 == */ ]]; then
    printf 'Supply an absolute Linux output file and optional amd64 or arm64 target.\n' >&2
    exit 2
fi
target_arch=${2:-amd64}
if [[ $target_arch != amd64 && $target_arch != arm64 ]]; then
    printf 'Target architecture must be amd64 or arm64.\n' >&2
    exit 2
fi
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
source_dir="$project_dir/apps/rmchat/core"
native_go=$(command -v -- "${GO_BIN:-go}" || true)
if [[ -n ${GO_BIN:-} && ( -z $native_go || $native_go == *.exe ) ]]; then
    printf 'GO_BIN must identify an installed native Linux Go executable.\n' >&2
    exit 2
fi
if [[ $native_go == *.exe ]]; then
    native_go=''
fi
if [[ -z $native_go ]]; then
    if ! command -v wslpath >/dev/null || ! command -v powershell.exe >/dev/null; then
        printf 'No native Go found. Install Go 1.25+ or run in WSL with Windows Go available.\n' >&2
        exit 2
    fi
fi
command -v python3 >/dev/null
output_parent=$(dirname -- "$1")
mkdir -p -- "$output_parent"
output_file="$(cd -- "$output_parent" && pwd)/$(basename -- "$1")"
if [[ -d $output_file ]]; then
    printf 'The output must be a file, not a directory.\n' >&2
    exit 2
fi
temporary=$(mktemp "${output_file}.tmp.XXXXXXXX")
trap 'rm -f -- "$temporary"' EXIT

if [[ -n $native_go ]]; then
    env GOOS=linux GOARCH="$target_arch" CGO_ENABLED=0 GOTOOLCHAIN=local \
        GOPROXY=off GOSUMDB=off GONOPROXY=none GOVCS='*:off' GOWORK=off GOENV=off GOFLAGS= \
        "$native_go" -C "$source_dir" build -mod=readonly -trimpath -buildvcs=false \
        -o "$temporary" ./cmd/rmchat-core
else
    windows_script=$(wslpath -w "$script_dir/build-rmchat-core.ps1")
    windows_source=$(wslpath -w "$source_dir")
    windows_output=$(wslpath -w "$temporary")
    powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass \
        -File "$windows_script" -SourceDirectory "$windows_source" \
        -OutputFile "$windows_output" -TargetArchitecture "$target_arch"
fi

# Reject a wrong target before replacing an existing application core.
python3 - "$temporary" "$target_arch" <<'PY'
import struct
import sys
with open(sys.argv[1], 'rb') as binary:
    header = binary.read(20)
machine = {'amd64': 62, 'arm64': 183}[sys.argv[2]]
if (len(header) != 20 or header[:6] != b'\x7fELF\x02\x01'
        or struct.unpack_from('<H', header, 18)[0] != machine):
    raise SystemExit('Go did not produce the requested Linux ELF64 executable.')
PY
chmod 755 -- "$temporary"
mv -f -- "$temporary" "$output_file"
printf 'RMChat core built for linux/%s: %s\n' "$target_arch" "$output_file"
