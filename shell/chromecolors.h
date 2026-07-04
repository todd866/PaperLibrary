/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_CHROMECOLORS_H
#define PAPERLIBRARY_CHROMECOLORS_H

#include <QColor>
#include <QPalette>
#include <QtGlobal>

namespace ChromeColors
{

inline bool darkMode(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightnessF() < 0.5;
}

inline QColor blend(const QColor &from, const QColor &to, qreal amount)
{
    const qreal t = qBound<qreal>(0.0, amount, 1.0);
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * t,
                            from.greenF() + (to.greenF() - from.greenF()) * t,
                            from.blueF() + (to.blueF() - from.blueF()) * t,
                            from.alphaF() + (to.alphaF() - from.alphaF()) * t);
}

inline QColor frameActive(const QPalette &palette)
{
    // md3.info recessed tab-strip/frame surface: darker than the app body.
    return darkMode(palette) ? QColor(0x0D, 0x0D, 0x0D) : QColor(0xD0, 0xD9, 0xDD);
}

inline QColor toolbar(const QPalette &palette)
{
    // md3.info --md-surface: app body, active tab, and toolbar surface.
    return darkMode(palette) ? QColor(0x12, 0x16, 0x1A) : QColor(0xF8, 0xFA, 0xFB);
}

inline QColor toolbarText(const QPalette &palette)
{
    // md3.info --md-on-surface: primary text.
    return darkMode(palette) ? QColor(0xE3, 0xE3, 0xE3) : QColor(0x17, 0x1D, 0x22);
}

inline QColor inactiveTabText(const QPalette &palette)
{
    // md3.info --md-on-surface-variant: muted tab text.
    return darkMode(palette) ? QColor(0xC3, 0xC6, 0xCF) : QColor(0x4D, 0x58, 0x60);
}

inline QColor inactiveTabHover(const QPalette &palette)
{
    // md3.info hover: blend --md-surface-container-high toward --md-surface.
    return blend(frameActive(palette), toolbar(palette), 0.40);
}

inline QColor tabSeparator(const QPalette &palette)
{
    // md3.info --md-outline-variant: hairline between idle tabs.
    return darkMode(palette) ? QColor(0x43, 0x47, 0x4E) : QColor(0xC3, 0xC6, 0xCF);
}

inline QColor accent(const QPalette &palette)
{
    // md3.info --md-primary (Material Design 3 brand): #1c5888 light, #6e9fcc dark.
    return darkMode(palette) ? QColor(0x6E, 0x9F, 0xCC) : QColor(0x1C, 0x58, 0x88);
}

} // namespace ChromeColors

#endif
