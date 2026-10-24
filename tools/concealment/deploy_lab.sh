#!/bin/bash
# Build libxorcist for the lab and put it where the lab looks for it.
#
# Why this exists: the .so used to be built and copied by hand.  On
# 2026-08-06 the deployed copy turned out to be a plain -O3 build while
# the measurements quoted in chat came from a -march=native -ffast-math
# build - the same source giving results 7-10 dB apart at order 16/20,
# and a lab that ran without AVX2 while everyone assumed it did not.
# Neither build matched the engine, which is the one thing the ctypes
# binding exists to guarantee.
#
# The flags below are GrandOrgue's own, read from
# build/linux/compile_commands.json:
#     -O3 -g -fomit-frame-pointer -funroll-loops -ffast-math -std=gnu++17
# Deliberately NO -march=native: GO ships generic x86-64 binaries, so a
# lab built for the local CPU would not be the same code the product
# runs.  The one intentional difference is XO_AR_ORDER_MAX, which the
# lab raises to 20 to let the dropdown reach orders the engine caps.
#
# Usage:  tools/concealment/deploy_lab.sh [target-dir]

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$REPO/src/xorcist/xorcist.cpp"
DEST="${1:-/media/sf_sebion/projekte/pwdsp/experiments}"
ORDER_MAX="${XO_AR_ORDER_MAX:-20}"

FLAGS=(-std=gnu++17 -O3 -fomit-frame-pointer -funroll-loops -ffast-math
       -DNDEBUG -fPIC -shared "-DXO_AR_ORDER_MAX=$ORDER_MAX")

[ -d "$DEST" ] || { echo "deploy_lab: target $DEST not mounted" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
g++ "${FLAGS[@]}" "$SRC" -lfftw3f -ldl -o "$tmp/libxorcist.so"

# The stamp answers "which build am I hearing?" without anyone having to
# ask.  gap_lab shows it in the window title; see _build_stamp there.
sha="$(sha256sum "$SRC" | cut -c1-12)"
commit="$(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null || echo '?')"
dirty=""
(cd "$REPO" && git diff --quiet -- src/xorcist) || dirty=" +lokale-Aenderungen"
stamp="$commit$dirty  src:$sha  ordnung<=$ORDER_MAX  $(date '+%Y-%m-%d %H:%M')"

# Rename, do NOT copy over it.  A running lab has the .so mmapped, and
# overwriting the file in place invalidates its pages - the process dies
# with SIGBUS mid-measurement (seen 2026-08-06, an order-16 run lost
# after 40 minutes).  A rename leaves the old inode alive for whoever
# still holds it and gives new starts the new file.
cp "$tmp/libxorcist.so" "$DEST/.libxorcist.so.new"
mv -f "$DEST/.libxorcist.so.new" "$DEST/libxorcist.so"
printf '%s\n' "$stamp" >"$DEST/libxorcist.stamp"
printf '%s\n' "${FLAGS[*]}" >>"$DEST/libxorcist.stamp"

echo "deploy_lab -> $DEST/libxorcist.so"
echo "            $stamp"
echo "            $(stat -c%s "$DEST/libxorcist.so") B"

# The lab itself travels with the library, so a commit pins WHICH
# measuring stand belongs to WHICH algorithm.  Keeping them in separate
# repositories is what let a stale .so and a fresh gap_lab disagree by
# 12 dB without anyone being able to see it.
#
# The host runs these files, so it needs its own copy - which makes the
# copy a deploy artefact, not a place to edit.  Editing it anyway is an
# easy mistake, and losing that edit to the next deploy would be a
# nasty one, so refuse rather than overwrite: if the destination differs
# from the repository AND is the newer of the two, somebody worked on
# the wrong copy.  DEPLOY_LAB_FORCE=1 overrides once the change is
# either saved or known to be worthless.
# Die Kernelquelle reist mit: die Bibliothek laedt sie zur Laufzeit
# (Suchreihenfolge XO_KERNEL_SRC > neben der .so > Repo-Pfad).
cp "$REPO/src/xorcist/xorcist_kernels.cl" "$DEST/xorcist_kernels.cl"
echo "            xorcist_kernels.cl  ($(stat -c%s "$DEST/xorcist_kernels.cl") B)"

for f in gap_lab.py xorcist.py; do
  src="$REPO/tools/concealment/$f"
  dst="$DEST/$f"
  if [ -e "$dst" ] && ! cmp -s "$src" "$dst"; then
    if [ "$dst" -nt "$src" ] && [ "${DEPLOY_LAB_FORCE:-0}" != 1 ]; then
      echo "deploy_lab: ABBRUCH - $dst ist NEUER als die Repo-Fassung" >&2
      echo "  und unterscheidet sich davon.  Da hat jemand die Kopie" >&2
      echo "  bearbeitet statt das Original.  Erst sichern:" >&2
      echo "      cp '$dst' '$src'   # Aenderung ins Repo holen" >&2
      echo "  oder verwerfen:  DEPLOY_LAB_FORCE=1 $0" >&2
      exit 1
    fi
  fi
  cp "$src" "$dst"
  echo "            $f  ($(stat -c%s "$dst") B)"
done
