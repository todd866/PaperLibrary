/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_READINGPROGRESS_H
#define PAPERLIBRARY_READINGPROGRESS_H

#include <QHash>
#include <QString>
#include <QUrl>

/**
 * A tiny store of how far the reader is through each document, so the library tiles can show a
 * percent-read the way Apple Books does.
 *
 * The saved reader POSITION (page, spine index, scroll) is not enough on its own -- a percentage
 * needs the total, which only the open reader knows. So the readers, which have current-and-total
 * at save time, record a 0..1 fraction here keyed by a hash of the document URL. The corpus model
 * reads it back by the same key for each tile. Kept apart from the position store so a schema
 * change to one never disturbs the other. Local-only, in the app config.
 */
namespace ReadingProgress
{
/** The per-document key: a hash of the URL, matching pdfview's positionKeyForUrl. */
QString keyForUrl(const QUrl &url);

/** Record how far through @p url the reader is, as a fraction clamped to [0, 1]. */
void record(const QUrl &url, double fraction);

/**
 * Record progress keyed by BOTH the document URL and its (normalised) title. A book exists at many
 * paths -- an iCloud copy, an imported copy, the corpus copy -- and progress recorded against one
 * path is invisible on a tile that shows another. The title key bridges them, so any copy of the
 * same book surfaces the reader's real position. Native reading recorded here outranks Apple Books.
 */
void record(const QUrl &url, const QString &title, double fraction);

/** The fraction read for a local file @p path, or a negative value when none is recorded. */
double fractionForPath(const QString &path);

/** Every recorded path-keyed fraction (hash -> fraction). Used to backfill title keys for books
    read before the title bridge existed, without re-opening each one. */
QHash<QString, double> recordedByPathKey();

/** Clamp a raw current/total into a sane fraction; total <= 0 yields "unknown" (-1). */
double fractionFromCounts(int current, int total);

/**
 * Sync Apple Books' own reading progress in, keyed by title, so a corpus book the reader is
 * reading in Apple Books shows that percentage even though the file paths differ. Titles are
 * normalised (case- and punctuation-insensitive). Held in memory; replaces any previous sync.
 */
void syncFromAppleBooks(const QHash<QString, double> &progressByTitle);

/** Apple Books progress for a title, or negative when it isn't reading that book. */
double fractionForTitle(const QString &title);

/** The normalised title key used for the Apple Books match (exposed for callers building the map). */
QString titleKey(const QString &title);
}

#endif // PAPERLIBRARY_READINGPROGRESS_H
