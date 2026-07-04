/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_COVERGENERATOR_H
#define PAPERLIBRARY_COVERGENERATOR_H

#include <QImage>
#include <QPalette>
#include <QString>

/**
 * Paints Penguin-paperback-style typographic covers for documents whose
 * page-one render is a mostly-white text page (per CoverHeuristic) or
 * could not be rendered at all: a flat card in a deterministic muted
 * accent — same tag, same hue, so shelves cluster visually by genre —
 * with the title set large as the artwork.
 */
namespace CoverGenerator
{
/** What the card says. Empty fields are skipped cleanly. */
struct CoverSpec {
    QString title;
    QString authors;     /**< one line, e.g. "A. Author, B. Author" */
    QString yearJournal; /**< one line for the foot, e.g. "2026 · Nature" */
    QString tag;         /**< first genre tag: drives the accent hue and the chip */
};

/**
 * The card's accent color for @p seed (a tag, or the title as fallback):
 * a stable FNV-1a hash into a curated 10-hue muted palette, so the same
 * seed yields the same hue in every session. @p darkMode deepens the
 * color (never inverts it) to sit in a dark shelf.
 */
QColor accentColor(const QString &seed, bool darkMode);

/**
 * Paint the card for @p spec at @p px device pixels (callers pass 2x the
 * logical cover size for retina crispness). @p palette only decides
 * light versus dark treatment. Same spec, size and palette produce a
 * bitwise-identical image.
 */
QImage generate(const CoverSpec &spec, const QSize &px, const QPalette &palette);

/**
 * The exact lines generate() will paint for @p spec's title at @p px,
 * after shrink-to-fit: wrapped at word boundaries only, never inside a
 * word. Only when a single word cannot fit the content width even at
 * the minimum size is that word cut, and then always under an ellipsis;
 * a title with more words than the card can hold likewise ends in an
 * ellipsis. Exposed so tests can assert the wrap policy directly.
 */
QStringList titleLines(const CoverSpec &spec, const QSize &px);
}

#endif
