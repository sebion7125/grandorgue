/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOWindowPlacement.h"

#if defined(__WXGTK__)
#include <cstdlib>
#include <cstring>
#endif

bool GOCanPlaceWindows() {
#if defined(__WXGTK__)
  /* Which backend GTK ended up on is decided once per process, so the answer
   * is cached. It is derived from the environment rather than asked of GDK,
   * which would pull gtk/gdk headers into a library that otherwise only needs
   * wxWidgets. GDK walks its backend list in order and takes the first one
   * whose display server answers, so we walk the same list: GDK_BACKEND when
   * set, otherwise GTK's built-in order, in which Wayland precedes X11.
   *
   * Order matters and a plain substring test would get it wrong in the
   * damaging direction: GDK_BACKEND="wayland,x11" contains "x11" but lands on
   * Wayland whenever a session offers one.
   *
   * If a listed backend has its socket but refuses the connection, GDK does
   * not silently fall through to the next one - it fails to start at all
   * (measured: WAYLAND_DISPLAY pointing at a dead socket aborts with "Unable
   * to initialize GTK+"). So a backend we consider available is the one that
   * is in use.
   */
  static const bool canPlace = []() {
    const char *const backend = std::getenv("GDK_BACKEND");
    const char *p = backend ? backend : "wayland,x11";

    while (*p) {
      const char *const sep = std::strchr(p, ',');
      const size_t len = sep ? (size_t)(sep - p) : std::strlen(p);

      if (len == 7 && !std::strncmp(p, "wayland", 7)
          && std::getenv("WAYLAND_DISPLAY"))
        return false;
      if (len == 3 && !std::strncmp(p, "x11", 3) && std::getenv("DISPLAY"))
        return true;
      p += sep ? len + 1 : len;
    }
    // Neither backend recognised (broadway, or an empty GDK_BACKEND): assume
    // positions work, which is how GO behaved before this guard existed.
    return true;
  }();

  return canPlace;
#else
  return true;
#endif
}
