#!/bin/bash
set -e


# $1..: Optionen/Versionen
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "📄 Kopiere GO_Attack_Parameters.[cpp/h] ins src-Verzeichnis (falls vorhanden) ..."
# Ziel relativ zum Skriptverzeichnis bestimmen (script-local), nicht relativ zum Aufruf-cwd
TARGET_SOUND_DIR="$(readlink -f "$SCRIPT_DIR/../../src/grandorgue/sound")"
mkdir -p "$TARGET_SOUND_DIR"
if [ -e /media/sf_Code_Exchange/GO_Attack_Parameters.cpp ]; then
  cp -v /media/sf_Code_Exchange/GO_Attack_Parameters.cpp "$TARGET_SOUND_DIR/"
else
  echo "Warnung: /media/sf_Code_Exchange/GO_Attack_Parameters.cpp nicht gefunden, überspringe."
fi
if [ -e /media/sf_Code_Exchange/GO_Attack_Parameters.h ]; then
  cp -v /media/sf_Code_Exchange/GO_Attack_Parameters.h "$TARGET_SOUND_DIR/"
else
  echo "Warnung: /media/sf_Code_Exchange/GO_Attack_Parameters.h nicht gefunden, überspringe."
fi

BUILD_DIR="$SCRIPT_DIR/build/win64"     # <<< früh setzen!

# ---- CLI-Optionen ----------------------------------------------------------
EXTRA_LABEL=""   # wird zu GO_VERSION_EXTRA
DO_CLEAN=false
DO_RECONF=false
LOG_RELEASE_ALIGN=false
LOG_RELEASE_ALIGN_VERBOSE=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean) DO_CLEAN=true; shift ;;
    --reconfigure) DO_RECONF=true; shift ;;
    --extra) EXTRA_LABEL="$2"; shift 2 ;;
    --log-release-align) LOG_RELEASE_ALIGN=true; shift ;;
    --log-release-align-verbose) LOG_RELEASE_ALIGN=true; LOG_RELEASE_ALIGN_VERBOSE=true; shift ;;
    *) break ;;  # Rest bleibt für set-ver-prms.sh (z.B. numerische Version)
  esac
done

# Optional: set-ver-prms.sh laden (z.B. numerische Versionen), wenn noch Args da sind
if [[ $# -gt 0 ]]; then
  source "$SCRIPT_DIR/../set-ver-prms.sh" "$@"
fi

# ---- Cache-Schutz (jetzt mit korrekt gesetztem BUILD_DIR) ------------------
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  if grep -q 'VERSION="\|VERSION=--' "$BUILD_DIR/CMakeCache.txt"; then
    echo "⚠️  Entferne ungültigen VERSION-Eintrag aus CMakeCache.txt"
    rm -v "$BUILD_DIR/CMakeCache.txt"
  fi
fi

# ---- Quellverzeichnis bestimmen -------------------------------------------
if [[ -n "$3" ]]; then
  SRC_DIR="$3"
else
  SRC_DIR=$(readlink -f "$SCRIPT_DIR/../..")
fi

# ---- Parallelisierung ------------------------------------------------------
PARALLEL_PRMS="-j$(nproc)"

# === Build-Tools erzeugen ===
if [[ ! -f build/build-tools/ImportExecutables.cmake ]]; then
  echo "Baue Build-Tools (ImportExecutables.cmake) …"
  mkdir -p build/build-tools
  pushd build/build-tools
  cmake "$SRC_DIR/src/build"
  make
  popd
else
  echo "Build-Tools bereits vorhanden – überspringe."
fi

# === Haupt-Buildverzeichnis vorbereiten ===
mkdir -p "$BUILD_DIR"
pushd "$BUILD_DIR"

# Optional: Build-Verzeichnis leeren
if $DO_CLEAN; then
  echo "Lösche Inhalte von $BUILD_DIR …"
  rm -rf ./*
fi

export LANG=C
# This script performs a win64 cross-build — always load mingw vars so
# cmake is configured to find the MinGW toolchain and headers.
source "$SCRIPT_DIR/set-mingw-vars.sh"
# set WX_CONFIG if mingw wx-config exists
if [[ -x "${MINGW_DIR:-}/bin/wx-config" ]]; then
  export WX_CONFIG="$MINGW_DIR/bin/wx-config"
fi

# ⚙️ Compiler-Flags
export CXXFLAGS="-O3 -DNDEBUG -g0"
export CFLAGS="-O3 -DNDEBUG -g0"

# ---- Release-Align-Logging (kein cmake-Eingriff nötig) ---------------------
# Schreiben/Löschen eines Header-Files triggert automatisch nur GOSoundStream.cpp neu.
LOG_HEADER="$SRC_DIR/src/grandorgue/sound/playing/GOLogReleaseAlignEnable.h"
LOG_VERBOSE_HEADER="$SRC_DIR/src/grandorgue/sound/playing/GOLogReleaseAlignVerbose.h"
STREAM_SRC="$SRC_DIR/src/grandorgue/sound/playing/GOSoundStream.cpp"
ALIGN_SRC="$SRC_DIR/src/grandorgue/sound/playing/GOSoundReleaseAlignTable.cpp"
if $LOG_RELEASE_ALIGN; then
  echo "// generated — delete to disable release-align logging" > "$LOG_HEADER"
  echo "Release-Align-Logging aktiviert ($LOG_HEADER)"
  # __has_include is not tracked by cmake deps — force recompile of affected files
  touch "$STREAM_SRC" "$ALIGN_SRC"
else
  if [[ -f "$LOG_HEADER" ]]; then
    rm -v "$LOG_HEADER"
    touch "$STREAM_SRC" "$ALIGN_SRC"
  fi
fi
if $LOG_RELEASE_ALIGN_VERBOSE; then
  echo "// generated — delete to disable verbose sample logging" > "$LOG_VERBOSE_HEADER"
  echo "Release-Align-Verbose-Logging aktiviert ($LOG_VERBOSE_HEADER)"
  touch "$STREAM_SRC"
else
  if [[ -f "$LOG_VERBOSE_HEADER" ]]; then
    rm -v "$LOG_VERBOSE_HEADER"
    touch "$STREAM_SRC"
  fi
fi

# ---- Version/Extra immer setzen -------------------------------------------
# 1) Wenn --extra nicht gesetzt wurde, Standard setzen:
if [[ -z "$EXTRA_LABEL" ]]; then
  EXTRA_LABEL="XFadeDemo"
fi
VERSION_PRMS="-DGO_VERSION_EXTRA=${EXTRA_LABEL}"

# Falls set-ver-prms.sh vorher CMAKE_VERSION_PRMS gesetzt hat: anhängen
if [[ -n "$CMAKE_VERSION_PRMS" ]]; then
  VERSION_PRMS="$VERSION_PRMS $CMAKE_VERSION_PRMS"
fi

# CMake nur bei Bedarf ausführen
if [[ ! -f CMakeCache.txt || $DO_RECONF || $DO_CLEAN ]]; then
  echo "Führe CMake-Konfiguration aus …"
  CMAKE_APP_PRMS="-DGO_USE_JACK=ON $VERSION_PRMS"

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
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS_USE_ARGUMENTS=ON \
    "-DCMAKE_C_FLAGS_RELEASE:STRING=-O3 -DNDEBUG -g0" \
    "-DCMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG -g0" \
    "-DCMAKE_EXE_LINKER_FLAGS_RELEASE:STRING=-s" \
    -DVC_PATH=/usr/local/share/wine/msvc/VC/Tools/MSVC/14.29.30133/bin/Hostx86/x86
else
  echo "CMake-Konfiguration bereits vorhanden – überspringe Konfiguration."
fi

# 🔨 Bauen
make $PARALLEL_PRMS VERBOSE=1 GrandOrgue
popd

# === Ergebnis kopieren ===
BUILD_BIN_DIR="$BUILD_DIR/bin"
EXPORT_DIR="/media/sf_Code_Exchange/GrandOrgue Dev/bin"
mkdir -p "$EXPORT_DIR"
echo "Kopiere Build-Ergebnis von \"$BUILD_BIN_DIR\" nach \"$EXPORT_DIR\" …"
cp -v "$BUILD_BIN_DIR/"* "$EXPORT_DIR/" || {
  echo "FEHLER: Kopieren nach \"$EXPORT_DIR\" fehlgeschlagen."
  exit 1
}
echo "✅ Kopie abgeschlossen."
