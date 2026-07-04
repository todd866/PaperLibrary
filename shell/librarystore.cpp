/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "librarystore.h"

#include <QFileInfo>

#include <algorithm>

static const char OPEN_COUNT_KEY[] = "OpenCount";
static const char LAST_OPENED_KEY[] = "LastOpened"; // msecs since epoch
static const char PINNED_KEY[] = "Pinned";
static const char DOWNRANKED_KEY[] = "Downranked";
static const char TITLE_KEY[] = "Title";
static const char TAGS_KEY[] = "Tags";
static const char DESCRIPTION_KEY[] = "Description";
static const char KEYWORDS_KEY[] = "Keywords";
static const char REMOVED_KEY[] = "Removed";

LibraryStore::LibraryStore(QObject *parent)
    : QObject(parent)
    , m_config(KSharedConfig::openConfig())
{
}

LibraryStore::LibraryStore(const QString &configPath, QObject *parent)
    : QObject(parent)
    , m_config(KSharedConfig::openConfig(configPath))
{
}

QString LibraryStore::entryKey(const QUrl &url)
{
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}

QUrl LibraryStore::urlFromKey(const QString &key)
{
    return key.startsWith(QLatin1Char('/')) ? QUrl::fromLocalFile(key) : QUrl(key);
}

KConfigGroup LibraryStore::libraryGroup() const
{
    return m_config->group(QStringLiteral("Library"));
}

void LibraryStore::recordOpen(const QUrl &url)
{
    recordOpen(url, QDateTime::currentDateTime());
}

void LibraryStore::recordOpen(const QUrl &url, const QDateTime &when)
{
    KConfigGroup group = libraryGroup().group(entryKey(url));
    group.deleteEntry(REMOVED_KEY);
    group.writeEntry(OPEN_COUNT_KEY, group.readEntry(OPEN_COUNT_KEY, 0) + 1);
    group.writeEntry(LAST_OPENED_KEY, when.toMSecsSinceEpoch());
    m_config->sync();
}

void LibraryStore::setPinned(const QUrl &url, bool pinned)
{
    KConfigGroup group = libraryGroup().group(entryKey(url));
    group.writeEntry(PINNED_KEY, pinned);
    m_config->sync();
}

bool LibraryStore::isPinned(const QUrl &url) const
{
    return libraryGroup().group(entryKey(url)).readEntry(PINNED_KEY, false);
}

void LibraryStore::setDownranked(const QUrl &url, bool downranked)
{
    KConfigGroup group = libraryGroup().group(entryKey(url));
    group.writeEntry(DOWNRANKED_KEY, downranked);
    if (downranked) {
        group.writeEntry(PINNED_KEY, false);
    }
    m_config->sync();
}

bool LibraryStore::isDownranked(const QUrl &url) const
{
    return libraryGroup().group(entryKey(url)).readEntry(DOWNRANKED_KEY, false);
}

void LibraryStore::writeMetadataEntry(const QUrl &url, const char *key, const QVariant &value)
{
    KConfigGroup group = libraryGroup().group(entryKey(url));
    const bool empty = value.userType() == QMetaType::QStringList ? value.toStringList().isEmpty() : value.toString().isEmpty();
    if (empty) {
        group.deleteEntry(key); // unset fields stay absent from the config
    } else {
        group.writeEntry(key, value);
    }
    m_config->sync();
}

void LibraryStore::setTitle(const QUrl &url, const QString &title)
{
    writeMetadataEntry(url, TITLE_KEY, title);
}

void LibraryStore::setTags(const QUrl &url, const QStringList &tags)
{
    writeMetadataEntry(url, TAGS_KEY, tags);
}

void LibraryStore::setDescription(const QUrl &url, const QString &description)
{
    writeMetadataEntry(url, DESCRIPTION_KEY, description);
}

void LibraryStore::setKeywords(const QUrl &url, const QStringList &keywords)
{
    writeMetadataEntry(url, KEYWORDS_KEY, keywords);
}

LibraryStore::Entry LibraryStore::metadata(const QUrl &url) const
{
    return readEntry(libraryGroup().group(entryKey(url)), url);
}

void LibraryStore::remove(const QUrl &url)
{
    const QString key = entryKey(url);
    KConfigGroup library = libraryGroup();
    library.group(key).deleteGroup();
    library.group(key).writeEntry(REMOVED_KEY, true);
    m_config->sync();
}

QList<LibraryStore::Entry> LibraryStore::entries(const QStringList &suffixFilter) const
{
    const KConfigGroup library = libraryGroup();
    const QStringList keys = library.groupList();

    QList<Entry> result;
    result.reserve(keys.size());
    for (const QString &key : keys) {
        const KConfigGroup entryGroup = library.group(key);
        if (entryGroup.readEntry(REMOVED_KEY, false)) {
            continue;
        }
        const QUrl url = urlFromKey(key);

        if (!suffixFilter.isEmpty()) {
            const QString suffix = QFileInfo(url.fileName()).suffix();
            const bool matches = std::any_of(suffixFilter.cbegin(), suffixFilter.cend(), [&suffix](const QString &wanted) { return suffix.compare(wanted, Qt::CaseInsensitive) == 0; });
            if (!matches) {
                continue;
            }
        }

        // Skip (but keep on record) entries whose file is currently gone
        if (url.isLocalFile() && !QFileInfo::exists(url.toLocalFile())) {
            continue;
        }

        result.append(readEntry(entryGroup, url));
    }

    std::sort(result.begin(), result.end(), [](const Entry &a, const Entry &b) {
        if (a.downranked != b.downranked) {
            return !a.downranked;
        }
        if (a.pinned != b.pinned) {
            return a.pinned;
        }
        if (a.openCount != b.openCount) {
            return a.openCount > b.openCount;
        }
        if (a.lastOpened != b.lastOpened) {
            return a.lastOpened > b.lastOpened;
        }
        return a.url.toString() < b.url.toString();
    });
    return result;
}

LibraryStore::Entry LibraryStore::readEntry(const KConfigGroup &group, const QUrl &url)
{
    Entry entry;
    entry.url = url;
    entry.openCount = group.readEntry(OPEN_COUNT_KEY, 0);
    const qint64 msecs = group.readEntry(LAST_OPENED_KEY, qint64(0));
    if (msecs > 0) {
        entry.lastOpened = QDateTime::fromMSecsSinceEpoch(msecs);
    }
    entry.pinned = group.readEntry(PINNED_KEY, false);
    entry.downranked = group.readEntry(DOWNRANKED_KEY, false);
    entry.title = group.readEntry(TITLE_KEY, QString());
    entry.tags = group.readEntry(TAGS_KEY, QStringList());
    entry.description = group.readEntry(DESCRIPTION_KEY, QString());
    entry.keywords = group.readEntry(KEYWORDS_KEY, QStringList());
    return entry;
}

bool LibraryStore::isEmpty() const
{
    return libraryGroup().groupList().isEmpty();
}

void LibraryStore::importUrls(const QList<QUrl> &urls)
{
    KConfigGroup library = libraryGroup();
    for (const QUrl &url : urls) {
        const QString key = entryKey(url);
        if (library.hasGroup(key)) {
            if (library.group(key).readEntry(REMOVED_KEY, false)) {
                continue; // user removed it; opening the file explicitly re-adds it
            }
            continue; // never clobber an existing entry's count
        }
        KConfigGroup group = library.group(key);
        group.writeEntry(OPEN_COUNT_KEY, 1);
        QDateTime stamp;
        if (url.isLocalFile()) {
            const QFileInfo info(url.toLocalFile());
            if (info.exists()) {
                stamp = info.lastModified();
            }
        }
        if (!stamp.isValid()) {
            stamp = QDateTime::currentDateTime();
        }
        group.writeEntry(LAST_OPENED_KEY, stamp.toMSecsSinceEpoch());
    }
    m_config->sync();
}
