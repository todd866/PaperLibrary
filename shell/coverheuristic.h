/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_COVERHEURISTIC_H
#define PAPERLIBRARY_COVERHEURISTIC_H

class QImage;

/**
 * Decides whether a document's rendered first page is worth using as its
 * library cover. Designed book covers and image-heavy pages read well at
 * tile size; a mostly-white page of paper text reads as a smudge, and those
 * documents deserve a generated typographic cover instead.
 */
namespace CoverHeuristic
{
enum Verdict {
    KeepRender,          /**< the page render is visually interesting — use it */
    GenerateTypographic, /**< a mostly-white text page — generate a cover */
};

/** The measured evidence behind a verdict. */
struct Score {
    double colorfulness = 0.0; /**< mean HSV saturation over opaque pixels, 0..1 */
    double inkCoverage = 0.0;  /**< fraction of pixels that are not near-white paper, 0..1 */
    double variance = 0.0;     /**< luminance variance, 0 (flat) .. 0.25 (bimodal) */
};

/**
 * Judge @p pageOne (a small render of the document's first page). A null
 * image yields GenerateTypographic. @p scoreOut, when given, receives the
 * measurements the verdict was based on.
 */
Verdict analyze(const QImage &pageOne, Score *scoreOut = nullptr);
}

#endif
