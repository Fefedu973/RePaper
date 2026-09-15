#!/usr/bin/env bash
# Build the reviewed AppLoad source locally. No device connection or installation.
set -eo pipefail
if [[ $# -ne 3 ]]; then
    echo 'Usage: build-appload-module.sh APPLOAD_SOURCE NEW_BUILD_DIR XOVI_SOURCE' >&2
    exit 2
fi
task_source=$(cd "$1" && pwd)
task_build="$2"
task_xovi=$(cd "$3" && pwd)
task_repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
task_sdk=${REPAPER_SDK_DIR:-/opt/repaper-sdk/5.8.203}
[[ "$task_build" = /* && ! -e "$task_build/source" ]] || {
    echo 'Use a new absolute build directory.' >&2
    exit 2
}
[[ "$(sha256sum "$task_source/xovi/template/appload.qmd" | cut -d' ' -f1)" = 7283b8ec5bd2621e63c40b4ea79cada634f83c71e45a305af5234197854315db ]] || {
    echo 'The source does not contain the reviewed PR 59 QMD.' >&2
    exit 2
}
source "$task_sdk/environment-setup-cortexa53-crypto-remarkable-linux"
export LOCPATH="$OECORE_NATIVE_SYSROOT/usr/lib/locale"
export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
mkdir -p "$task_build/source" "$task_build/module"
cp -a "$task_source/src" "$task_build/source/"
cp -a "$task_source/shim" "$task_build/source/"
cp "$task_source/appload.pro" "$task_build/source/"
mkdir -p "$task_build/source/xovi"
cp -a "$task_source/xovi/template" "$task_build/source/xovi/"
cp -a "$task_source/resources" "$task_build/source/"
cp -a "$task_repo/packaging/appload/device/native-keyboard-host" "$task_build/source/src/"
cp -a "$task_repo/packaging/appload/device/native-viewport-host" "$task_build/source/src/"
cp -a "$task_repo/packaging/appload/device/native-documents-host" "$task_build/source/src/"
mkdir -p "$task_build/source/src/native-lines/scene"
for task_file in NativeObjectAccess.cpp NativeObjectAccess.h NativeObjectAccessMemory_p.h \
    NativeObjectSnapshot.h NativeHistorySnapshot.h NativeHistoryGuard.h NativePropertyHistory_p.h \
    NativeSelectionColor.cpp NativeSelectionColor.h NativeStrokeColor.h \
    NativeLineFactory.cpp NativeLineFactory.h TargetProfile.cpp TargetProfile.h \
    NativeStrokeSampling.cpp NativeStrokeSampling.h; do
    cp "$task_repo/extensions/editor-common/$task_file" "$task_build/source/src/native-lines/"
done
cp "$task_repo/apps/restencil/src/Geometry.h" "$task_build/source/src/native-lines/"
for task_file in rm_Line.cpp rm_Line.hpp rm_SceneItem.hpp LICENSE.txt; do
    cp "$task_repo/.tools/xovi-scene-assistant/$task_file" "$task_build/source/src/native-lines/scene/"
done
# Normalize only the shipping copies: Windows checkouts may use CRLF, while
# the maintained patches use LF. The upstream checkout remains unchanged.
python3 - "$task_build/source" <<'PY'
from pathlib import Path
import sys
for path in Path(sys.argv[1]).rglob('*'):
    if path.is_file() and path.suffix in ('.cpp', '.h', '.hpp', '.qml', '.pro', '.qrc', '.xovi'):
        path.write_bytes(path.read_bytes().replace(b'\r\n', b'\n'))
PY
cp "$task_repo/packaging/appload/device/native-documents-host/NativeDocumentsAdapter.qml" \
   "$task_build/source/resources/qml/RePaperNativeDocuments.qml"
cp "$task_repo/packaging/appload/device/native-documents-host/NativeImportsAdapter.qml" \
   "$task_build/source/resources/qml/RePaperNativeImports.qml"
cp "$task_repo/packaging/appload/device/calculator-toolbar/calculator.svg" "$task_build/source/resources/icons/calculator.svg"
cp "$task_repo/packaging/appload/device/calculator-toolbar/pdf-reader.svg" "$task_build/source/resources/icons/pdf-reader.svg"
for task_patch in windowed-input window-rendering stylus-input native-stylus-mouse native-keyboard-host native-documents-host calculator-window calculator-toolbar pdf-toolbar pdf-responsive-window window-chrome suite-responsive; do
    patch --batch --forward --fuzz=0 -d "$task_build/source" -p1 \
      < "$task_repo/packaging/appload/device/$task_patch.patch"
done
for task_project in "$task_build/source/appload.pro" "$task_build/source/xovi/template/appload.pro"; do
    cat >> "$task_project" <<'PRO'
CONFIG -= c++17
CONFIG += c++20
INCLUDEPATH += src/native-lines src/native-lines/scene
DEFINES += REPAPER_WITH_NATIVE_ABI
SOURCES += src/native-lines/NativeObjectAccess.cpp src/native-lines/NativeSelectionColor.cpp src/native-lines/NativeLineFactory.cpp src/native-lines/TargetProfile.cpp src/native-lines/NativeStrokeSampling.cpp src/native-lines/scene/rm_Line.cpp
HEADERS += src/native-lines/NativeObjectAccess.h src/native-lines/NativeSelectionColor.h
# These QObject classes must own their MOC definitions inside AppLoad. Reject
# unresolved symbols at link time instead of depending on another XOVI module.
QMAKE_LFLAGS += -Wl,--no-undefined
PRO
done
python3 - "$task_build/source/xovi/template/appload.qmd" \
  "$task_repo/packaging/appload/device/native-documents.qmd" \
  "$task_repo/packaging/appload/device/calculator-toolbar.qmd" \
  "$task_repo/packaging/appload/device/pdf-toolbar.qmd" <<'PY'
from pathlib import Path
import sys
target, *addons = map(Path, sys.argv[1:])
target.write_bytes(b'\n\n'.join(path.read_bytes().strip() for path in [target, *addons]) + b'\n')
PY
# Apply all source/resource patches before overlaying XOVI's module entry point.
cp -a "$task_build/source/xovi/template/." "$task_build/source/"
# Consume the complete RCC output before trimming its registration wrapper.
# A streaming sed that quits at #ifdef can close its pipe early for larger QML
# resources, making the otherwise successful generator fail under pipefail.
"$OECORE_NATIVE_SYSROOT/usr/libexec/rcc" --no-compress -g cpp "$task_build/source/resources/resources.qrc" \
  -o "$task_build/source/resources.full.cpp"
sed '/#ifdef _MSC_VER/,/#endif/d' "$task_build/source/resources.full.cpp" > "$task_build/source/resources.trimmed.cpp"
sed -n '/#ifdef/q;p' "$task_build/source/resources.trimmed.cpp" > "$task_build/source/resources.cpp"
cd "$task_build/source"
python3 "$task_xovi/util/xovigen.py" -o xovi.cpp -H xovi.h appload.xovi
cd "$task_build/module"
qmake6 "$task_build/source/appload.pro"
make -j"${REPAPER_BUILD_JOBS:-4}"
"$READELF" -h appload.so
