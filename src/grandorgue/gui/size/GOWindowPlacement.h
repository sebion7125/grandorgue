/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOWINDOWPLACEMENT_H
#define GOWINDOWPLACEMENT_H

/**
 * Whether window positions mean anything on the display server we are running
 * on.
 *
 * On X11 and Windows an application may place its windows and ask where they
 * are. Wayland allows neither: the xdg-shell protocol has no message that
 * carries a window position, so wxTopLevelWindow::Move() never reaches the
 * compositor, and GetPosition() answers with the position the client last
 * asked for - or, before that, with the shadow margin of the client side
 * decoration. Both are fiction.
 *
 * Callers use this to keep stored window positions untouched instead of
 * overwriting them with those fictional values. Window *sizes* stay valid
 * everywhere - a client does determine its own size.
 */
bool GOCanPlaceWindows();

#endif /* GOWINDOWPLACEMENT_H */
