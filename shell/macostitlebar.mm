/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "macostitlebar.h"

#include <QWindow>

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <cmath>

namespace
{
// Padding past the rightmost traffic light before the first tab may begin.
constexpr int kInsetPadding = 12;

/** The NSWindow backing a Qt top-level, or nil if it is not up yet. */
NSWindow *nativeWindow(QWindow *window)
{
    if (!window) {
        return nil;
    }
    // winId() is the backing NSView* on macOS. It will create the platform
    // window if absent, so every caller here first gates on the non-forcing
    // windowHandle() being up; the zero check below is just belt-and-braces.
    const WId wid = window->winId();
    if (!wid) {
        return nil;
    }
    NSView *const view = reinterpret_cast<NSView *>(wid);
    if (![view isKindOfClass:[NSView class]]) {
        return nil;
    }
    return [view window];
}

bool styleTitlebarWindow(QWindow *window, bool removeToolbar)
{
    NSWindow *const nsWindow = nativeWindow(window);
    if (!nsWindow) {
        return false;
    }

    // Full-size content view + transparent, title-less titlebar: the Qt
    // content view now fills the whole window and draws up under the
    // titlebar, so the tab strip sits beside the (still native) traffic
    // lights.
    const NSWindowStyleMask mask = nsWindow.styleMask;
    const NSWindowStyleMask adoptedMask = mask | NSWindowStyleMaskFullSizeContentView;
    if (mask != adoptedMask) {
        nsWindow.styleMask = adoptedMask;
    }
    if (!nsWindow.titlebarAppearsTransparent) {
        nsWindow.titlebarAppearsTransparent = YES;
    }
    if (nsWindow.titleVisibility != NSWindowTitleHidden) {
        nsWindow.titleVisibility = NSWindowTitleHidden;
    }

    // The empty strip must not itself drag the window (the tab strip decides
    // where a press drags), and macOS's own window tabbing would stack a
    // second, native tab bar above ours — refuse it.
    if (nsWindow.movableByWindowBackground) {
        nsWindow.movableByWindowBackground = NO;
    }
    if ([nsWindow respondsToSelector:@selector(setTabbingMode:)] && nsWindow.tabbingMode != NSWindowTabbingModeDisallowed) {
        nsWindow.tabbingMode = NSWindowTabbingModeDisallowed;
    }

    // No NSToolbar. An empty unified toolbar grows a real ~40 pt titlebar
    // band, and Qt then lays its content view out *below* that band (it
    // honors the shrunken contentLayoutRect) — so the tab strip fell under
    // the titlebar instead of into it. With no toolbar the full-size content
    // view fills the window edge to edge and the strip draws from y = 0, up
    // under the transparent titlebar and beside the native traffic lights;
    // the strip's leading inset keeps the tabs clear of them. The lights
    // keep their standard vertical position, resting near the strip's top.
    // Clear any toolbar an earlier adopt (or app version) may have left, so a
    // re-adopt always converges on the no-toolbar state.
    if (removeToolbar && nsWindow.toolbar) {
        nsWindow.toolbar = nil;
    }

    return true;
}
}

namespace MacTitlebar
{
bool adoptWindow(QWindow *window)
{
    return styleTitlebarWindow(window, true);
}

bool repinWindow(QWindow *window)
{
    return styleTitlebarWindow(window, true);
}

Metrics measure(QWindow *window)
{
    Metrics metrics;
    NSWindow *const nsWindow = nativeWindow(window);
    if (!nsWindow) {
        return metrics;
    }

    const CGFloat frameHeight = NSHeight(nsWindow.frame);
    const CGFloat contentHeight = NSHeight(nsWindow.contentLayoutRect);
    const CGFloat titlebarHeight = frameHeight - contentHeight;
    if (titlebarHeight > 0.0) {
        metrics.titlebarHeight = static_cast<int>(std::lround(titlebarHeight));
    }

    // The zoom button is the rightmost traffic light; its right edge plus a
    // little padding is where the first tab may begin. Buttons are hidden in
    // fullscreen, which leaves the inset at 0 — exactly the collapse we want.
    NSButton *const zoomButton = [nsWindow standardWindowButton:NSWindowZoomButton];
    if (zoomButton && !zoomButton.isHidden) {
        const CGFloat rightEdge = NSMaxX(zoomButton.frame);
        if (rightEdge > 0.0) {
            metrics.leadingInset = static_cast<int>(std::ceil(rightEdge)) + kInsetPadding;
        }
    }

    return metrics;
}

void performDoubleClickAction(QWindow *window)
{
    NSWindow *const nsWindow = nativeWindow(window);
    if (!nsWindow) {
        return;
    }

    // Honor the user's system preference; an unset value means the modern
    // macOS default, which is zoom.
    NSString *action = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
    if (action.length == 0) {
        action = @"Maximize";
    }

    if ([action isEqualToString:@"Minimize"]) {
        if (nsWindow.miniaturizable) {
            [nsWindow performMiniaturize:nil];
        }
    } else if ([action isEqualToString:@"Maximize"]) {
        if (nsWindow.zoomable) {
            [nsWindow performZoom:nil];
        }
    }
    // "None" (or anything unrecognized): do nothing.
}

QString describe(QWindow *window)
{
    NSWindow *const nsWindow = nativeWindow(window);
    if (!nsWindow) {
        return QStringLiteral("MacTitlebar: no native window");
    }

    const NSWindowStyleMask mask = nsWindow.styleMask;
    const Metrics m = measure(window);

    NSString *toolbarStyle = @"n/a";
    if (@available(macOS 11.0, *)) {
        switch (nsWindow.toolbarStyle) {
        case NSWindowToolbarStyleAutomatic:
            toolbarStyle = @"Automatic";
            break;
        case NSWindowToolbarStyleExpanded:
            toolbarStyle = @"Expanded";
            break;
        case NSWindowToolbarStylePreference:
            toolbarStyle = @"Preference";
            break;
        case NSWindowToolbarStyleUnified:
            toolbarStyle = @"Unified";
            break;
        case NSWindowToolbarStyleUnifiedCompact:
            toolbarStyle = @"UnifiedCompact";
            break;
        }
    }

    NSString *titleVisibility = (nsWindow.titleVisibility == NSWindowTitleHidden) ? @"Hidden" : @"Visible";

    // The content view (Qt's QNSView) should span the full frame while the
    // contentLayoutRect stays shorter by the titlebar band — the pair that
    // shows the full-size content view is in effect and how tall the titlebar
    // Qt reports as safe area is.
    const NSRect frame = nsWindow.frame;
    const NSRect contentLayout = nsWindow.contentLayoutRect;
    NSView *const contentView = nsWindow.contentView;
    const NSRect cvFrame = contentView ? contentView.frame : NSZeroRect;

    NSString *dump = [NSString stringWithFormat:@"MacTitlebar window dump:\n"
                                                @"  styleMask.FullSizeContentView = %@\n"
                                                @"  styleMask.Titled             = %@\n"
                                                @"  titlebarAppearsTransparent   = %@\n"
                                                @"  titleVisibility              = %@\n"
                                                @"  toolbar attached             = %@\n"
                                                @"  toolbarStyle                 = %@\n"
                                                @"  tabbingMode disallowed       = %@\n"
                                                @"  window.frame                 = %.0f x %.0f\n"
                                                @"  contentLayoutRect            = %.0f x %.0f (titlebar band %.0f)\n"
                                                @"  contentView.frame            = origin(%.1f,%.1f) %.0f x %.0f\n"
                                                @"  measured titlebarHeight      = %d pt\n"
                                                @"  measured leadingInset        = %d pt",
                                                (mask & NSWindowStyleMaskFullSizeContentView) ? @"YES" : @"NO",
                                                (mask & NSWindowStyleMaskTitled) ? @"YES" : @"NO",
                                                nsWindow.titlebarAppearsTransparent ? @"YES" : @"NO",
                                                titleVisibility,
                                                nsWindow.toolbar ? @"YES" : @"NO",
                                                toolbarStyle,
                                                (nsWindow.tabbingMode == NSWindowTabbingModeDisallowed) ? @"YES" : @"NO",
                                                frame.size.width, frame.size.height,
                                                contentLayout.size.width, contentLayout.size.height, frame.size.height - contentLayout.size.height,
                                                cvFrame.origin.x, cvFrame.origin.y, cvFrame.size.width, cvFrame.size.height,
                                                m.titlebarHeight,
                                                m.leadingInset];

    return QString::fromNSString(dump);
}
}
