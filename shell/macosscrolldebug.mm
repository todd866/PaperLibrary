/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "macosscrolldebug.h"

#include <QtGlobal>

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <cstdio>

namespace
{
id s_scrollMonitor = nil;

NSString *phaseDescription(NSEventPhase phase)
{
    if (phase == NSEventPhaseNone) {
        return @"None";
    }

    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    if (phase & NSEventPhaseBegan) {
        [parts addObject:@"Began"];
    }
    if (phase & NSEventPhaseStationary) {
        [parts addObject:@"Stationary"];
    }
    if (phase & NSEventPhaseChanged) {
        [parts addObject:@"Changed"];
    }
    if (phase & NSEventPhaseEnded) {
        [parts addObject:@"Ended"];
    }
    if (phase & NSEventPhaseCancelled) {
        [parts addObject:@"Cancelled"];
    }
    if (phase & NSEventPhaseMayBegin) {
        [parts addObject:@"MayBegin"];
    }

    if (parts.count == 0) {
        return [NSString stringWithFormat:@"Unknown(%lu)", static_cast<unsigned long>(phase)];
    }
    return [parts componentsJoinedByString:@"|"];
}

NSView *hitTestViewForEvent(NSEvent *event)
{
    NSWindow *const window = event.window;
    if (!window) {
        return nil;
    }

    NSView *const contentView = window.contentView;
    if (contentView) {
        const NSPoint point = [contentView convertPoint:event.locationInWindow fromView:nil];
        if (NSView *const hitView = [contentView hitTest:point]) {
            return hitView;
        }
    }

    NSView *const frameView = contentView.superview;
    if (frameView) {
        const NSPoint point = [frameView convertPoint:event.locationInWindow fromView:nil];
        return [frameView hitTest:point];
    }

    return nil;
}
}

namespace MacScrollDebug
{
void installLocalMonitorIfEnabled()
{
    if (s_scrollMonitor || !qEnvironmentVariableIsSet("PAPERLIBRARY_SCROLL_DEBUG")) {
        return;
    }

    s_scrollMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                                           handler:^NSEvent *(NSEvent *event) {
                                                               NSString *const phase = phaseDescription(event.phase);
                                                               NSString *const momentumPhase = phaseDescription(event.momentumPhase);
                                                               NSView *const hitView = hitTestViewForEvent(event);
                                                               NSString *const hitClass = hitView ? NSStringFromClass(hitView.class) : @"(none)";

                                                               std::fprintf(stderr,
                                                                            "PAPERLIBRARY_SCROLL_DEBUG AppKit scroll phase=%s momentumPhase=%s precise=%s deltaX=%.3f deltaY=%.3f hitView=%s\n",
                                                                            phase.UTF8String,
                                                                            momentumPhase.UTF8String,
                                                                            event.hasPreciseScrollingDeltas ? "true" : "false",
                                                                            event.scrollingDeltaX,
                                                                            event.scrollingDeltaY,
                                                                            hitClass.UTF8String);
                                                               std::fflush(stderr);
                                                               return event;
                                                           }];

#if !__has_feature(objc_arc)
    [s_scrollMonitor retain];
#endif

    std::fprintf(stderr, "PAPERLIBRARY_SCROLL_DEBUG AppKit scroll monitor installed\n");
    std::fflush(stderr);
}
}
