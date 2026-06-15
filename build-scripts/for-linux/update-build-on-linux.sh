#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(readlink -f "$SCRIPT_DIR/../..")"
BUILD_DIR="$SRC_DIR/build/linux"

# ---- CLI-Optionen ----------------------------------------------------------
DO_CLEAN=false
DO_RECONF=false
LOG_RELEASE_ALIGN=false
LOG_RELEASE_ALIGN_VERBOSE=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean)                    DO_CLEAN=true;                  shift ;;
    --reconfigure)              DO_RECONF=true;                 shift ;;
    --log-release-align)        LOG_RELEASE_ALIGN=true;         shift ;;
    --log-release-align-verbose) LOG_RELEASE_ALIGN=true; LOG_RELEASE_ALIGN_VERBOSE=true; shift ;;
    *) break ;;
  esac
done

# ---- Release-Align-Logging -------------------------------------------------
LOG_HEADER="$SRC_DIR/src/grandorgue/sound/playing/GOLogReleaseAlignEnable.h"
LOG_VERBOSE_HEADER="$SRC_DIR/src/grandorgue/sound/playing/GOLogReleaseAlignVerbose.h"
STREAM_SRC="$SRC_DIR/src/grandorgue/sound/playing/GOSoundStream.cpp"
ALIGN_SRC="$SRC_DIR/src/grandorgue/sound/playing/GOSoundReleaseAlignTable.cpp"
PROVIDER_SRC="$SRC_DIR/src/grandorgue/sound/providers/GOSoundProvider.cpp"
AUDIO_SECTION_SRC="$SRC_DIR/src/grandorgue/sound/playing/GOSoundAudioSection.cpp"

if $LOG_RELEASE_ALIGN; then
  echo "// generated — delete to disable release-align logging" > "$LOG_HEADER"
  echo "Release-Align-Logging aktiviert."
  touch "$STREAM_SRC" "$ALIGN_SRC" "$PROVIDER_SRC" "$AUDIO_SECTION_SRC"
else
  if [[ -f "$LOG_HEADER" ]]; then
    rm -v "$LOG_HEADER"
    touch "$STREAM_SRC" "$ALIGN_SRC" "$PROVIDER_SRC" "$AUDIO_SECTION_SRC"
  fi
fi

if $LOG_RELEASE_ALIGN_VERBOSE; then
  echo "// generated — delete to disable verbose sample logging" > "$LOG_VERBOSE_HEADER"
  echo "Release-Align-Verbose-Logging aktiviert."
  touch "$STREAM_SRC"
else
  if [[ -f "$LOG_VERBOSE_HEADER" ]]; then
    rm -v "$LOG_VERBOSE_HEADER"
    touch "$STREAM_SRC"
  fi
fi

# ---- Build-Verzeichnis vorbereiten -----------------------------------------
mkdir -p "$BUILD_DIR"
pushd "$BUILD_DIR"

if $DO_CLEAN; then
  echo "Lösche Build-Verzeichnis …"
  rm -rf ./*
fi

export LANG=C

# ---- CMake konfigurieren (nur wenn nötig) ----------------------------------
if [[ ! -f CMakeCache.txt || $DO_RECONF || $DO_CLEAN ]]; then
  echo "Führe CMake-Konfiguration aus …"
  cmake -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "$SRC_DIR"
fi

# ---- Bauen -----------------------------------------------------------------
make -j$(nproc) VERBOSE=1 grandorgue

popd
echo "✅ Build abgeschlossen: $BUILD_DIR/bin/grandorgue"
