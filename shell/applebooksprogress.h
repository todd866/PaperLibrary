/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_APPLEBOOKSPROGRESS_H
#define PAPERLIBRARY_APPLEBOOKSPROGRESS_H

#include <QList>
#include <QString>

/**
 * Read-only reader of Apple Books' library database.
 *
 * Lists books with reading progress and a real on-disk path, so the
 * library's EPUB shelf can show "n% in Books" tiles. Strictly one-way:
 * the database is opened read-only and immutable, and never modified.
 *
 * A failure (missing database, no sqlite driver, TCC denial, corrupt file,
 * schema mismatch) is reported through the optional @c ok out-parameter and
 * logged, so it is distinguishable from the genuine "no books in progress"
 * case (which also yields an empty list, but with @c ok set true).
 */
class AppleBooksProgress
{
public:
    struct BookEntry {
        QString title;
        QString path;
        double progress = 0.0;
    };

    /**
     * Read book entries with progress > 0 and a non-null path. With no
     * argument, resolves the newest BKLibrary-*.sqlite in Apple Books'
     * container.
     *
     * When @p ok is non-null it is set to true only if the database opened
     * and the query ran (independent of how many rows matched), and false on
     * any failure. Apple Books simply not being installed (no database found
     * for the default path) counts as success with an empty result.
     */
    static QList<BookEntry> read(const QString &dbPath = QString(), bool *ok = nullptr);
};

#endif
