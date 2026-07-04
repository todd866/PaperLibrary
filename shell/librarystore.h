/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_LIBRARYSTORE_H
#define PAPERLIBRARY_LIBRARYSTORE_H

#include <KConfigGroup>
#include <KSharedConfig>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QUrl>

/**
 * Persistent per-document usage store backing the document library.
 *
 * Tracks open counts, last-opened timestamps and a pinned flag per document
 * in a [Library] group of paperlibraryrc (or an explicit config file for tests).
 * Entries whose file has disappeared are kept on record but skipped when
 * listing, so a document that comes back (e.g. an external drive remounts)
 * keeps its ranking.
 */
class LibraryStore : public QObject
{
    Q_OBJECT

public:
    struct Entry {
        QUrl url;
        int openCount = 0;
        QDateTime lastOpened;
        bool pinned = false;
        bool downranked = false; /**< thumbs-down: keep searchable, but push below normal feed items */
        QString title;        /**< display title override; empty = unset */
        QStringList tags;     /**< genre/topic tags, most significant first */
        QString description;  /**< one-line summary */
        QStringList keywords; /**< extra search terms, never displayed */
    };

    explicit LibraryStore(QObject *parent = nullptr);
    /** Back the store with an explicit config file (for tests). */
    explicit LibraryStore(const QString &configPath, QObject *parent = nullptr);

    /** Bump the open count and stamp lastOpened; creates the entry if new. */
    void recordOpen(const QUrl &url);
    /** As above with an explicit timestamp (for tests). */
    void recordOpen(const QUrl &url, const QDateTime &when);

    void setPinned(const QUrl &url, bool pinned);
    bool isPinned(const QUrl &url) const;
    void setDownranked(const QUrl &url, bool downranked);
    bool isDownranked(const QUrl &url) const;

    /** Metadata setters; an empty value clears the field. Creates the entry if new. */
    void setTitle(const QUrl &url, const QString &title);
    void setTags(const QUrl &url, const QStringList &tags);
    void setDescription(const QUrl &url, const QString &description);
    void setKeywords(const QUrl &url, const QStringList &keywords);

    /**
     * The stored entry for @p url with all fields populated; metadata that
     * was never set reads back empty. Unlike entries() this works whether
     * or not the file currently exists.
     */
    Entry metadata(const QUrl &url) const;

    void remove(const QUrl &url);

    /**
     * All listable entries: pinned first, then by open count (desc) with
     * lastOpened as tiebreak. An optional filename-suffix filter (e.g.
     * {"pdf"} or {"epub"}, case-insensitive) restricts the result so
     * callers can build per-format shelves. Entries whose local file no
     * longer exists are skipped (but kept on record).
     */
    QList<Entry> entries(const QStringList &suffixFilter = {}) const;

    bool isEmpty() const;

    /**
     * Seed initial entries (open count 1, lastOpened from the file's
     * mtime), e.g. from the recent-files list. URLs that already have an
     * entry are left untouched.
     */
    void importUrls(const QList<QUrl> &urls);

private:
    KConfigGroup libraryGroup() const;
    void writeMetadataEntry(const QUrl &url, const char *key, const QVariant &value);
    static QString entryKey(const QUrl &url);
    static QUrl urlFromKey(const QString &key);
    static Entry readEntry(const KConfigGroup &group, const QUrl &url);

    KSharedConfig::Ptr m_config;
};

#endif
