#!/bin/bash
# Linux-Pendant zu deploy_go_exe_when_free.sh: wartet, bis das GrandOrgue-
# Binary im entpackten AppImage nicht mehr laeuft ("Text file busy"), und
# deployt dann den frischen for-linux-Build.
#
# Zwei Eigenheiten dieses Wegs, die man leicht vergisst:
#  - Der for-linux-Build linkt einen ABSOLUTEN RPATH auf das Build-
#    Verzeichnis in der VM. Auf dem Host findet er die GO-Libs damit nicht
#    ("cannot open libGrandOrgueImages.so"), deshalb der patchelf-Schritt.
#  - Die .so-Dateien werden nur mitkopiert, wenn sie neuer sind; bei
#    laufendem GO waeren auch sie gesperrt.
SRC="/home/vboxuser/grandorgue/build/linux/bin/GrandOrgue"
LIBSRC="/home/vboxuser/grandorgue/build/linux/lib"
DSTDIR="/media/sf_sebion/squashfs-root/usr"
DST="$DSTDIR/bin/GrandOrgue"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

command -v patchelf >/dev/null || export PATH="$PATH:$HOME/.local/bin"
if ! command -v patchelf >/dev/null; then
  echo "FEHLER: patchelf fehlt (pip3 install --user --break-system-packages patchelf)"
  exit 1
fi
if [ ! -f "$SRC" ]; then
  echo "FEHLER: kein Build unter $SRC"
  exit 1
fi
if [ ! -d "$DSTDIR/bin" ]; then
  echo "FEHLER: $DSTDIR/bin nicht erreichbar (sf_sebion nicht gemountet?)"
  exit 1
fi

cp "$SRC" "$TMP/GrandOrgue"
patchelf --set-rpath '$ORIGIN/../lib' "$TMP/GrandOrgue"

for i in $(seq 1 240); do   # 240 x 30s = max 2 Stunden
  waited=$(( (i - 1) * 30 ))
  if cmp -s "$TMP/GrandOrgue" "$DST"; then
    echo "OK: GrandOrgue war bereits aktuell (nach ${waited}s)"
    exit 0
  fi
  if [ -f "$DST" ]; then
    cp "$DST" "$TMP/GrandOrgue.prev" 2>/dev/null
  fi
  if cp "$TMP/GrandOrgue" "$DST" 2>/dev/null; then
    for so in "$LIBSRC"/libGrandOrgue*.so.*; do
      [ -f "$so" ] && cp "$so" "$DSTDIR/lib/" 2>/dev/null
    done
    echo "OK: deployt nach ${waited}s  ($(md5sum "$DST" | cut -c1-8))"
    exit 0
  fi
  sleep 30
done
echo "ABBRUCH: GrandOrgue lief 2 Stunden durchgehend - nicht deployt"
exit 1
