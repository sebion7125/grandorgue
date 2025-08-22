#!/bin/bash

# $1 - Version
# $2 - Build version
# $3 - Go source Dir. If not set then relative to the script dir

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

source $(dirname $0)/../set-ver-prms.sh "$1" "$2"

if [[ -n "$3" ]]; then
	SRC_DIR=$3
else
	SRC_DIR=$(readlink -f $(dirname $0)/../..)
fi

PARALLEL_PRMS="-j$(nproc)"

BUILD_ROOT="$SCRIPT_DIR/build"
mkdir -p "$BUILD_ROOT/build-tools"
pushd "$BUILD_ROOT/build-tools"
rm -rf *
cmake $SRC_DIR/src/build
make
popd

mkdir -p "$BUILD_ROOT/win64"
pushd "$BUILD_ROOT/win64"

rm -rf *
export LANG=C

source "$SCRIPT_DIR/set-mingw-vars.sh"

# Ensure MinGW bin is first in PATH so the MinGW wx-config (/mingw64/bin/wx-config)
# is found by CMake and tools before the system wx-config.
export PATH="$MINGW_DIR/bin:$PATH"

# Export WX_CONFIG (now that MINGW_DIR is defined)
WX_CONFIG="$MINGW_DIR/bin/wx-config"; export WX_CONFIG

# ⚠️ Verhindert unnötige Debug-Infos
export CXXFLAGS="-O3 -DNDEBUG -g0"
export CFLAGS="-O3 -DNDEBUG -g0"

cmake "$SRC_DIR" \
  $CMAKE_MINGW_PRMS \
  $CMAKE_APP_PRMS \
  -DASIO_SDK_DIR=/usr/local/asio-sdk \
  -DCV2PDB_EXE=/usr/local/share/wine/cv2pdb/cv2pdb.exe \
  -DIMPORT_EXECUTABLES=../build-tools/ImportExecutables.cmake \
  -DINSTALL_DEPEND=ON \
  -DMSYS=1 -DSTATIC=0 \
  -DRTAUDIO_USE_ASIO=ON \
  -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_C_FLAGS_RELEASE:STRING=-O3 -DNDEBUG -g0" \
  "-DCMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG -g0" \
  "-DCMAKE_EXE_LINKER_FLAGS_RELEASE:STRING=-s" \
  -DVC_PATH=/usr/local/share/wine/msvc/VC/Tools/MSVC/14.29.30133/bin/Hostx86/x86

CMAKE_APP_PRMS="-DGO_USE_JACK=ON $CMAKE_VERSION_PRMS"

cmake $CMAKE_MINGW_PRMS $CMAKE_WIN_PRMS $CMAKE_APP_PRMS . $SRC_DIR
make $PARALLEL_PRMS VERBOSE=1 GrandOrgue

popd

# Versuch: Erzeuge Installer mit CPack (falls von CMake konfiguriert).
# Wechsel ins win64-Buildverzeichnis und versuche das package-Target / CPack.
pushd "$BUILD_ROOT/win64"

echo "Erzeuge Installer (CPack / package target)..."

# Versuche zuerst das CMake package target (plattformunabhängig).
if cmake --build . --target package; then
    echo "Erfolg: cmake --build --target package"
else
    echo "Hinweis: package-target nicht verfügbar oder fehlgeschlagen, versuche cpack direkt..."
    if /usr/bin/cpack --config ./CPackConfig.cmake; then
        echo "Erfolg: cpack hat Pakete erzeugt."
    else
        echo "Warnung: cpack/package erzeugte keine Installer. Fortfahren und ggf. Binärdateien kopieren."
    fi
fi

popd

# === Ergebnis in gemeinsamen Zielordner kopieren ===

BUILD_BIN_DIR="$BUILD_ROOT/win64/bin"
EXPORT_DIR="/media/sf_Code_Exchange/GrandOrgue Dev/bin"

# Sicherstellen, dass Zielverzeichnis existiert
mkdir -p "$EXPORT_DIR"

echo "Kopiere gesamten Inhalt von \"$BUILD_BIN_DIR\" nach \"$EXPORT_DIR\" …"

cp -v "$BUILD_BIN_DIR/"* "$EXPORT_DIR/" || {
    echo "FEHLER: Kopieren nach \"$EXPORT_DIR\" fehlgeschlagen."
    exit 1
}

echo "Kopie abgeschlossen."
