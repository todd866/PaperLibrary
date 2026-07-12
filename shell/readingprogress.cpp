/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "readingprogress.h"

#include <QCryptographicHash>

#include <KConfigGroup>
#include <KSharedConfig>

namespace
{
constexpr auto ProgressGroup = "ReadingProgress";
constexpr auto ProgressByTitleGroup = "ReadingProgressByTitle"; // native PL reading, keyed by title
QHash<QString, double> g_appleBooksByTitle; // in-memory Apple Books sync, keyed by normalised title
}

QString ReadingProgress::titleKey(const QString &title)
{
    QString key;
    key.reserve(title.size());
    for (const QChar ch : title) {
        if (ch.isLetterOrNumber()) {
            key.append(ch.toCaseFolded());
        }
    }
    return key;
}

void ReadingProgress::syncFromAppleBooks(const QHash<QString, double> &progressByTitle)
{
    g_appleBooksByTitle = progressByTitle;
}

double ReadingProgress::fractionForTitle(const QString &title)
{
    const QString key = titleKey(title);
    if (key.isEmpty()) {
        return -1.0;
    }
    // Native PaperLibrary reading (persisted by title) outranks the Apple Books sync: if the reader
    // has read this book here, that is the authoritative position even when Apple Books also knows it.
    const KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(ProgressByTitleGroup));
    const double native = group.readEntry(key, -1.0);
    if (native >= 0.0) {
        return native;
    }
    return g_appleBooksByTitle.value(key, -1.0);
}

QString ReadingProgress::keyForUrl(const QUrl &url)
{
    // Same hash pdfview uses for its position key, so a document has one identity across stores.
    return QString::fromLatin1(
        QCryptographicHash::hash(url.toEncoded(QUrl::FullyEncoded), QCryptographicHash::Sha256).toHex());
}

double ReadingProgress::fractionFromCounts(int current, int total)
{
    if (total <= 0) {
        return -1.0; // unknown: the reader could not say how long the document is
    }
    if (current < 0) {
        current = 0;
    }
    if (current > total) {
        current = total;
    }
    return double(current) / double(total);
}

void ReadingProgress::record(const QUrl &url, double fraction)
{
    if (url.isEmpty() || fraction < 0.0) {
        return; // nothing to record; never write a negative "unknown"
    }
    fraction = qBound(0.0, fraction, 1.0);
    KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(ProgressGroup));
    group.writeEntry(keyForUrl(url), fraction);
    group.sync();
}

void ReadingProgress::record(const QUrl &url, const QString &title, double fraction)
{
    record(url, fraction);
    const QString key = titleKey(title);
    if (key.isEmpty() || fraction < 0.0) {
        return; // path is recorded above; a titleless document just has no title bridge
    }
    KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(ProgressByTitleGroup));
    group.writeEntry(key, qBound(0.0, fraction, 1.0));
    group.sync();
}

double ReadingProgress::fractionForPath(const QString &path)
{
    if (path.isEmpty()) {
        return -1.0;
    }
    const KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(ProgressGroup));
    return group.readEntry(keyForUrl(QUrl::fromLocalFile(path)), -1.0);
}

QHash<QString, double> ReadingProgress::recordedByPathKey()
{
    const KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(ProgressGroup));
    QHash<QString, double> result;
    const QStringList keys = group.keyList();
    result.reserve(keys.size());
    for (const QString &key : keys) {
        bool ok = false;
        const double value = group.readEntry(key, QString()).toDouble(&ok);
        if (ok && value >= 0.0) {
            result.insert(key, value);
        }
    }
    return result;
}
