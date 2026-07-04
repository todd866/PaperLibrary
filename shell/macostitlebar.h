/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_MACOSTITLEBAR_H
#define PAPERLIBRARY_MACOSTITLEBAR_H

#include <QString>

class QWindow;

/**
 * A narrow C++ front for the AppKit calls that turn a Qt top-level window
 * into a Chrome-style "tabs in the titlebar" window on macOS. The whole
 * body lives in macostitlebar.mm (Objective-C++); this header carries no
 * Cocoa types so the rest of the shell stays plain C++.
 *
 * The mechanism: give the NSWindow a full-size content view and a
 * transparent, title-less titlebar, and attach no toolbar. The Qt content
 * view then fills the window edge to edge and draws from y=0 straight up
 * under the titlebar, so the hand-painted tab strip sits beside the native
 * traffic lights (an empty NSToolbar was tried first, but it grew a real
 * titlebar band that Qt laid the strip out *below* — the bug this shim
 * fixes). The traffic lights keep their standard position near the strip's
 * top; the strip's leading inset holds the tabs clear of them.
 *
 * Every call is guarded and idempotent: if the native window is not up yet
 * (or anything is missing), it degrades to a clean no-op. adoptWindow()
 * must therefore be re-called after show and on window-state / screen
 * changes, since Qt can restyle the surface underneath.
 */
namespace MacTitlebar
{
/** Titlebar geometry read back from the live window, in logical points. */
struct Metrics {
    int titlebarHeight = 0; ///< native titlebar height (0 if unmeasurable)
    int leadingInset = 0;   ///< clearance the tab strip needs to miss the traffic lights
};

/**
 * Adopt @p window into titlebar-tabs mode. Idempotent and safe to call
 * repeatedly. Returns true when the AppKit window was found and styled,
 * false when the native window is not available yet (caller should retry
 * after the next show / state change).
 */
bool adoptWindow(QWindow *window);

/**
 * Re-assert the cheap, already-adopted titlebar properties after Qt relayouts
 * the content widgets. It also clears any native toolbar Qt/KXmlGui may have
 * reattached while switching document modes; otherwise that toolbar band can
 * cover the hand-painted tab strip.
 */
bool repinWindow(QWindow *window);

/**
 * Measure the live titlebar height and the leading inset the strip needs
 * to clear the traffic lights, from the actual standard-window-button
 * frames. Returns zeroed Metrics if the window is unavailable or the
 * buttons are hidden (e.g. in fullscreen) — callers keep their last good
 * windowed measurement in that case.
 */
Metrics measure(QWindow *window);

/**
 * Perform the user's configured empty-titlebar double-click action
 * (AppleActionOnDoubleClick: Maximize/zoom, Minimize, or None). Defaults
 * to zoom when the preference is unset. A safe no-op if unavailable.
 */
void performDoubleClickAction(QWindow *window);

/**
 * A human-readable dump of the adopted window's relevant NSWindow
 * properties (style-mask bits, titlebar transparency, title visibility,
 * toolbar + style, measured heights). For verify-by-launch only.
 */
QString describe(QWindow *window);
}

#endif
