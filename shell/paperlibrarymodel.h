/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_PAPERLIBRARYMODEL_H
#define PAPERLIBRARY_PAPERLIBRARYMODEL_H

#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>
#include <QVariant>

class QThread;

/**
 * A virtualized list model over a PaperLibrary corpus: one row per record
 * of the corpus directory's catalog.jsonl (12 fields per line), newest
 * additions first. The catalog is parsed on a worker thread so an ~18k-line
 * corpus never blocks the UI; rows land in one model reset announced by
 * loaded().
 *
 * Strictly read-only towards the corpus: catalog.jsonl is only ever read,
 * catalog.db (when present and readable) is opened immutable/read-only for
 * the pdf_path/pdf_evicted enrichment, and no corpus script is ever
 * invoked. Record fields are opaque display strings — the model stores and
 * shows them, nothing more.
 */
class PaperLibraryModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        DetailRole = Qt::UserRole + 1, /**< "authors · year · journal" */
        SlugRole,
        DoiRole,
        SourceRole,
        AuthorsRole,
        YearRole,
        JournalRole,
        AddedRole,
        LastAccessedRole,
        AccessCountRole,
        PinnedRole,
        CitedByCountRole,
        HaystackRole, /**< case-folded searchable text for the filter */
        MissingRole,  /**< true when the catalog knows the PDF is not local */
        ResolvedPathRole, /**< load-time local PDF path, no fresh filesystem check */
    };

    /** What was learned about a record's local PDF at load time. */
    enum Availability {
        Available, /**< a local file was found */
        Missing,   /**< the catalog says evicted, or no path resolves */
        Unknown,   /**< no database to ask; resolution deferred to activation */
    };

    struct Record {
        QString slug;
        QString doi;
        QString pmid;
        QString citeKey;
        QString title;
        QString authors;
        QString year;
        QString journal;
        QString source;
        QString addedTs;
        qint64 bytes = 0;
        QString pdfPath; /**< resolved local path; empty when none was found */
        QString lastAccessed;
        int accessCount = 0;
        bool pinned = false;
        int citedByCount = -1;
        Availability availability = Unknown;
        QString haystack;
    };

    explicit PaperLibraryModel(QObject *parent = nullptr);
    ~PaperLibraryModel() override;

    /**
     * The corpus directory the shell is configured for.
     */
    static QString configuredCorpusDir();
    /** True when @p corpusDir holds a catalog.jsonl. */
    static bool corpusExists(const QString &corpusDir);
    QString corpusDir() const;

    /** Parse @p corpusDir's catalog off the UI thread; emits loaded(). */
    void load(const QString &corpusDir);
    /** Re-load, but only when catalog.jsonl's mtime moved since the last load. */
    void reloadIfChanged();
    bool isLoaded() const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    /**
     * The record's local PDF for opening, re-checked against the disk at
     * call time: the load-time path when it still exists, else the derived
     * pdfs/<slug>.pdf when that exists, else an empty string.
     */
    QString resolvePdfPath(int row) const;
    int rowForLookupSlug(const QString &slug) const;
    int rowForLookupDoi(const QString &doi) const;
    int rowForLookupPath(const QString &path) const;

    // The pure pieces, unit-testable without a corpus directory
    /** One JSON object per line; malformed or blank lines are skipped. */
    static QList<Record> parseCatalog(const QByteArray &jsonl);
    /** Newest first by added_ts (slug as deterministic tiebreak). */
    static void sortRecords(QList<Record> &records);
    /**
     * Fill pdfPath/availability: catalog.db's pdf_path when the database is
     * readable (opened read-only and immutable), the derived
     * pdfs/<slug>.pdf as fallback — either only counts when the file
     * actually exists. Without a readable database unresolved records stay
     * Unknown instead of Missing.
     */
    static void enrichRecords(QList<Record> &records, const QString &corpusDir);

Q_SIGNALS:
    void loaded(int count);

private:
    void finishLoad(const QList<Record> &records);
    void rebuildLookupRows();

    friend class PaperLibraryModelTest;

    QString m_corpusDir;
    QDateTime m_catalogMtime;
    QThread *m_worker = nullptr;
    bool m_loading = false;
    bool m_loaded = false;
    QList<Record> m_records;
    QHash<QString, int> m_rowsByLookupSlug;
    QHash<QString, int> m_rowsByLookupDoi;
    QHash<QString, int> m_rowsByLookupPath;
};

/**
 * The library search field's instant layer over the corpus: keystroke
 * substring filtering (case-insensitive) across title, authors, journal,
 * year, cite_key, DOI and slug, against the model's precomputed haystack.
 */
class PaperLibraryFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    enum SmartFilter {
        All,
        Textbooks,
        Mnd,
    };

    void setSmartFilter(SmartFilter filter);
    void setQuery(const QString &query);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    SmartFilter m_smartFilter = All;
    QString m_query; /**< case-folded, like the haystack */
};

/**
 * Reader-facing corpus model: filters the PaperLibrary catalog into shelves
 * such as Papers, Books, Textbooks and MND, then orders records by reading
 * priority, topic, project, source, year, journal, or publication type
 * without mutating the catalog.
 */
class PaperLibrarySectionedModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum SmartFilter {
        Papers,
        Books,
        Textbooks,
        Medicine,
        Psychiatry,
        Mnd,
        Work,
        Anthropology,
        Politics,
        Fiction,
        Nonfiction,
    };

    enum SectionMode {
        ReadNext = 0,
        ByTopic = 1,
        ByProject = 2,
        ByType = 3,
        BySource = 4,
        ByYear = 5,
        ByJournal = 6,
    };

    enum Role {
        SectionHeaderRole = Qt::UserRole + 100, /**< legacy bool: false for emitted tile rows */
        SourceRowRole,
        KindRole,
        TopicTagsRole,
        RelatedQueryRole,
        FocusRole,
        ThumbnailSeedRole,
        ShelfIntentRole,
        RelationHintRole,
        PriorityHintRole,
        PdfPathRole,
        CoverPixmapRole,
        GeneratedCoverRole,
        DownrankedRole,
    };

    explicit PaperLibrarySectionedModel(QObject *parent = nullptr);

    void setSourceModel(PaperLibraryModel *model);
    void setShelf(SmartFilter filter, SectionMode mode);
    void setSmartFilter(SmartFilter filter);
    void setSectionMode(SectionMode mode);
    void setQuery(const QString &query);
    void setCoverForPath(const QString &path, const QVariant &cover, bool generated);
    void setDownranked(const QModelIndex &index, bool downranked);

    QString resolvePath(const QModelIndex &index) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

private:
    struct Row {
        bool header = false;
        int sourceRow = -1;
        QString title;
        QString focusId;
        QString focusDoi;
        QString focusAuthors;
        QString focusYear;
        QString focusJournal;
        QString focusSource;
        QString focusKind;
        QString focusSection;
        QString focusReason;
        QString focusPath;
        int focusOrder = -1;
        double focusScore = 0.0;
    };

    void rebuild();
    void rebuildPathIndex();
    QString cacheKey() const;
    void clearRowCache();
    QString pathForRow(const Row &row) const;
    QString storedPathForSourceRow(int sourceRow) const;
    bool sourceRowDownranked(int sourceRow) const;
    void saveDownrankedSlugs() const;

    PaperLibraryModel *m_source = nullptr;
    SmartFilter m_smartFilter = Papers;
    SectionMode m_sectionMode = ReadNext;
    QString m_query;
    QList<Row> m_rows;
    QHash<QString, QList<Row>> m_rowCache;
    QHash<QString, QList<int>> m_rowsByPath;
    QHash<QString, QVariant> m_coverPixmaps;
    QSet<QString> m_generatedCoverPaths;
    QSet<QString> m_downrankedSlugs;
};

#endif
