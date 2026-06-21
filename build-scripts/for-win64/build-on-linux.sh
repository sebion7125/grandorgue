#!/bin/bash

# $1 - Version
# $2 - Build version
# $3 - Go source Dir. If not set then relative to the script dir
# $4 - release flag (ON/OFF, default: OFF)

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

source $(dirname $0)/../set-ver-prms.sh "$1" "$2" "$4"

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

# Set GO_LOG_RELEASE_ALIGN=1 before calling this script to enable release-align logging.
# Note: keep debug info enabled here (no -g0/-s) — cv2pdb needs CodeView/DWARF
# entries in the linked exe to split out the .pdb (see BuildExecutable.cmake).
LOG_RELEASE_ALIGN_FLAG="${GO_LOG_RELEASE_ALIGN:+-DGO_LOG_RELEASE_ALIGN}"
export CXXFLAGS="-O3 -DNDEBUG $LOG_RELEASE_ALIGN_FLAG"
export CFLAGS="-O3 -DNDEBUG"

cmake "$SRC_DIR" \
  $CMAKE_MINGW_PRMS \
  $CMAKE_APP_PRMS \
  -DASIO_SDK_DIR=/usr/local/asio-sdk \
  -DCV2PDB_EXE=/usr/local/share/wine/cv2pdb/cv2pdb.exe \
  -DIMPORT_EXECUTABLES=../build-tools/ImportExecutables.cmake \
  -DINSTALL_DEPEND=ON \
  -DMSYS=1 -DSTATIC=0 \
  -DRTAUDIO_USE_ASIO=ON \
  -DVC_PATH=/usr/local/share/wine/msvc/VC/Tools/MSVC/14.29.30133/bin/Hostx86/x86

CMAKE_APP_PRMS="-DGO_USE_JACK=ON $CMAKE_VERSION_PRMS $CMAKE_RELEASE_FLAG_PRM"

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
