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

mkdir -p build/build-tools
pushd build/build-tools
rm -rf *
cmake $SRC_DIR/src/build
make
popd

mkdir -p build/win64
pushd build/win64

rm -rf *
export LANG=C

WX_CONFIG=$MINGW_DIR/bin/wx-config; export WX_CONFIG

source "$SCRIPT_DIR/set-mingw-vars.sh"

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

# === Ergebnis in gemeinsamen Zielordner kopieren ===

BUILD_BIN_DIR="/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/bin"
EXPORT_DIR="/media/sf_Code_Exchange/GrandOrgue Dev/bin"

# Sicherstellen, dass Zielverzeichnis existiert
mkdir -p "$EXPORT_DIR"

echo "Kopiere gesamten Inhalt von \"$BUILD_BIN_DIR\" nach \"$EXPORT_DIR\" …"

cp -v "$BUILD_BIN_DIR/"* "$EXPORT_DIR/" || {
    echo "FEHLER: Kopieren nach \"$EXPORT_DIR\" fehlgeschlagen."
    exit 1
}

echo "Kopie abgeschlossen."
