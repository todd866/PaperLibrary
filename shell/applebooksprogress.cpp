/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "applebooksprogress.h"

#include <QAtomicInt>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>

#include <utility>

static QString newestBooksDatabase()
{
    const QDir booksDir(QDir::homePath() + QStringLiteral("/Library/Containers/com.apple.iBooksX/Data/Documents/BKLibrary"));
    // Newest first; Books keeps versioned BKLibrary-*.sqlite files
    const QFileInfoList candidates = booksDir.entryInfoList({QStringLiteral("*.sqlite")}, QDir::Files | QDir::Readable, QDir::Time);
    return candidates.isEmpty() ? QString() : candidates.first().absoluteFilePath();
}

// A read-only, immutable sqlite file: URI for @p dbPath. Percent-encoding the
// path keeps a name containing characters significant in a URI (spaces, %, ?,
// #) from producing a malformed URI. immutable=1 also tells sqlite the file
// won't change, so it takes no locks and ignores any -wal/-shm sidecars left
// behind by a running Apple Books — a plain read-only open can otherwise fail
// or read inconsistent state.
static QString readOnlyImmutableUri(const QString &dbPath)
{
    return QUrl::fromLocalFile(dbPath).toString(QUrl::FullyEncoded) + QStringLiteral("?mode=ro&immutable=1");
}

QList<AppleBooksProgress::BookEntry> AppleBooksProgress::read(const QString &dbPath, bool *ok)
{
    const auto fail = [ok]() -> QList<BookEntry> {
        if (ok) {
            *ok = false;
        }
        return {};
    };
    const auto succeed = [ok](QList<BookEntry> &&entries) -> QList<BookEntry> {
        if (ok) {
            *ok = true;
        }
        return std::move(entries);
    };

    const bool explicitPath = !dbPath.isEmpty();
    const QString path = explicitPath ? dbPath : newestBooksDatabase();
    if (path.isEmpty()) {
        // Apple Books simply isn't installed here: a genuine empty, not a failure
        return succeed({});
    }
    if (!QFileInfo::exists(path)) {
        qWarning() << "AppleBooksProgress: database not found:" << path;
        return fail();
    }
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        qWarning() << "AppleBooksProgress: no QSQLITE driver available";
        return fail();
    }

    QList<BookEntry> result;
    bool queried = false;
    static QAtomicInt connectionCounter;
    const QString connectionName = QStringLiteral("paperlibrary_apple_books_%1").arg(connectionCounter.fetchAndAddRelaxed(1));
    {
        // Strictly one-way: open Books' database read-only and immutable, never write
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(readOnlyImmutableUri(path));
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_OPEN_URI"));
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT ZTITLE, ZPATH, ZREADINGPROGRESS FROM ZBKLIBRARYASSET WHERE ZREADINGPROGRESS > 0 AND ZPATH IS NOT NULL"))) {
                queried = true;
                while (query.next()) {
                    BookEntry entry;
                    entry.title = query.value(0).toString();
                    entry.path = query.value(1).toString();
                    entry.progress = query.value(2).toDouble();
                    result.append(entry);
                }
            } else {
                qWarning() << "AppleBooksProgress: query failed:" << query.lastError().text();
            }
            db.close();
        } else {
            qWarning() << "AppleBooksProgress: open failed:" << db.lastError().text();
        }
    } // db and query must be gone before removeDatabase()
    QSqlDatabase::removeDatabase(connectionName);

    return queried ? succeed(std::move(result)) : fail();
}
