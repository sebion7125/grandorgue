#!/bin/bash

# $1 - Version
# $2 - Build version
# $3 - Go source Dir. If not set then relative to the script dir
# $4 - pkg_suffix
# $5 - release flag (ON/OFF, default: OFF)

set -e

DIR=$(readlink -f $(dirname $0))
source $DIR/../set-ver-prms.sh "$1" "$2" "$5"

if [[ -n "$3" ]]; then
	SRC_DIR=$(readlink -f $3)
else
	SRC_DIR=$(readlink -f $DIR/../..)
fi

PARALLEL_PRMS="-j$(nproc)"

mkdir -p build/appimage-x86_64
pushd build/appimage-x86_64

rm -rf *
export LANG=C

GO_PRMS="-DCMAKE_INSTALL_PREFIX=/usr \
  -DRTAUDIO_USE_JACK=OFF \
  -DRTMIDI_USE_JACK=OFF \
  -DGO_USE_JACK=OFF \
  $CMAKE_RELEASE_FLAG_PRM \
  $CMAKE_VERSION_PRMS"
echo "cmake -G \"Unix Makefiles\" $GO_PRMS . $SRC_DIR"
cmake -G "Unix Makefiles" $GO_PRMS . $SRC_DIR
make -k $PARALLEL_PRMS
make install DESTDIR=AppDir

# initialize AppDir and build AppImage
export DEPLOY_GTK_VERSION=3 APPIMAGE_EXTRACT_AND_RUN=1
linuxdeploy-x86_64.AppImage --appdir AppDir --plugin gtk

# The gtk plugin unconditionally forces GDK_BACKEND=x11 in its AppRun hook, so
# on a Wayland session GrandOrgue is pushed through XWayland: blurred scaling on
# HiDPI displays and no native Wayland window. Without that assignment GDK picks
# the backend itself - Wayland when WAYLAND_DISPLAY is present, X11 otherwise -
# so make the assignment conditional instead of dropping it: an X11 session then
# behaves exactly as before.
GTK_HOOK=AppDir/apprun-hooks/linuxdeploy-plugin-gtk.sh
if grep -q '^export GDK_BACKEND=x11' "$GTK_HOOK"; then
  # "," as the separator, not "|": the replacement itself contains "||"
  sed -i 's,^export GDK_BACKEND=x11,[ -n "$WAYLAND_DISPLAY" ] || export GDK_BACKEND=x11,' "$GTK_HOOK"
  echo "patched $GTK_HOOK: GDK_BACKEND=x11 only without a Wayland session"
elif grep -q 'WAYLAND_DISPLAY.*GDK_BACKEND=x11' "$GTK_HOOK"; then
  echo "$GTK_HOOK: already conditional, nothing to patch"
else
  # Fail loudly rather than silently shipping XWayland-only again: the plugin
  # has changed and the line above needs to be revisited.
  echo "ERROR: no known GDK_BACKEND line in $GTK_HOOK - has linuxdeploy-plugin-gtk changed?" >&2
  exit 1
fi

appimagetool-x86_64.AppImage --no-appstream AppDir grandorgue-$1-$2.x86_64.AppImage

popd
