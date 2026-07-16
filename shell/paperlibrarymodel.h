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
#include <QStringList>
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
 * catalog.db (when present and readable) is opened WAL-aware/read-only for
 * the pdf_path/pdf_evicted enrichment, and no corpus script is ever
 * invoked. corpus_state.json is the read-only backend contract for derived
 * index freshness. Record fields are opaque display strings — the model
 * stores and shows them, nothing more.
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
        RelatedCountRole,
        GenreRole, /**< librarian genre; drives shelf classification when present */
        RecordKindRole, /**< 'book'/'paper' from the librarian; authoritative book flag */
        DescriptionRole, /**< librarian synopsis/description */
        TopicsRole, /**< librarian topic strings */
        ReadingLevelRole, /**< librarian audience/reading level */
        SubgenreRole, /**< librarian content subtype */
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

    /** Freshness contract published by the backend in corpus_state.json. */
    struct CorpusHealth {
        enum Status {
            UnknownStatus, /**< state file is missing, malformed, or unsupported */
            Healthy,
            Degraded,
        };

        Status status = UnknownStatus;
        QString generatedAt;
        QStringList issues;
        QStringList warnings;
        int catalogRows = -1;
        QString catalogRevision;
        QString manifestSha256;
        QString manifestSourceRevision;
        bool manifestFresh = false;
        bool searchFresh = false;
        bool graphFresh = false;
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
        QString genre; /**< librarian genre from catalog.jsonl; authoritative for shelves when non-empty */
        QString recordKind; /**< 'book'/'paper' from the librarian; authoritative book flag when non-empty */
        QString description;
        QStringList topics;
        QString readingLevel;
        QString subgenre;
        QString addedTs;
        qint64 bytes = 0;
        QString pdfPath; /**< resolved local path; empty when none was found */
        QString lastAccessed;
        int accessCount = 0;
        bool pinned = false;
        int citedByCount = -1;
        int relatedCount = -1;
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
    /**
     * Books the reader imported locally, which the corpus does not know about.
     *
     * Nothing in the shell writes catalog.jsonl, so an imported EPUB never reaches the
     * corpus. Merging them here lets the book shelves show the whole collection instead
     * of choosing between the corpus and the reader's own imports. A local book whose
     * title already exists in the catalog is dropped -- the catalog row is richer.
     */
    void setLocalBooks(const QList<Record> &books);
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
    /** Best-effort match of a display title to a catalog row, so a local-shelf tile can borrow the
        librarian blurb/topics of its corpus twin. Returns -1 when no confident match exists. */
    int rowForLookupTitle(const QString &title) const;
    /** Like rowForLookupTitle but over ALL rows (no blurb requirement): used to find the corpus twin
        of a file-backed feed book so marking it finished can land it on the Finished shelf. */
    int rowForAnyTitle(const QString &title) const;
    /** True when the catalog row is classified as a book by the same rules used by corpus shelves. */
    bool isBookRow(int row) const;
    CorpusHealth corpusHealth() const;
    bool hasFullTextSearchIndex() const;
    bool hasSemanticGraph() const;
    /** True only when the artifact exists and corpus_state.json says it matches this catalog. */
    bool hasFreshFullTextSearchIndex() const;
    bool hasFreshSemanticGraph() const;
    QList<int> fullTextSearchRows(const QString &query, int limit = 600) const;
    QList<int> relatedRowsForSlug(const QString &slug, int limit = 120) const;

    // The pure pieces, unit-testable without a corpus directory
    /** One JSON object per line; malformed or blank lines are skipped. */
    static QList<Record> parseCatalog(const QByteArray &jsonl);
    /** Newest first by added_ts (slug as deterministic tiebreak). */
    static void sortRecords(QList<Record> &records);
    /**
     * Fill pdfPath/availability: catalog.db's pdf_path when the database is
     * readable (opened WAL-aware and read-only), the derived
     * pdfs/<slug>.pdf as fallback — either only counts when the file
     * actually exists. Without a readable database unresolved records stay
     * Unknown instead of Missing.
     */
    static void enrichRecords(QList<Record> &records, const QString &corpusDir);

Q_SIGNALS:
    void loaded(int count);

private:
    void finishLoad(QList<Record> records, CorpusHealth health, quint64 generation);
    void rebuildLookupRows();
    /** m_records := catalog rows, then any local book the catalog does not already hold. */
    void rebuildRecords();

    friend class PaperLibraryModelTest;

    QString m_corpusDir;
    QDateTime m_catalogMtime;
    QDateTime m_manifestMtime;
    QDateTime m_healthMtime;
    QThread *m_worker = nullptr;
    QSet<QThread *> m_workers;
    quint64 m_loadGeneration = 0;
    bool m_loading = false;
    bool m_loaded = false;
    QList<Record> m_records;      /**< what the view sees: catalog rows + local-only books */
    QList<Record> m_catalogRecords; /**< catalog.jsonl alone; a reload replaces only this */
    QList<Record> m_localBooks;   /**< the reader's imports; survive a corpus reload */
    QByteArray m_localBooksSignature; /**< identity of the last set, to skip a no-op reset */
    bool m_localBooksSet = false;
    CorpusHealth m_corpusHealth;
    QHash<QString, int> m_rowsByLookupSlug;
    QHash<QString, int> m_rowsByLookupDoi;
    QHash<QString, int> m_rowsByLookupPath;
    QHash<QString, int> m_rowsByLookupTitle;
    QHash<QString, int> m_rowsByAnyTitle;
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
        Finished, /**< cross-cutting: any record the user marked finished, book or paper */
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
        ThumbnailPathRole,
        ThumbnailSourceRole,
        ShelfIntentRole,
        RelationHintRole,
        PriorityHintRole,
        PdfPathRole,
        ReadingProgressRole, /**< 0..1 percent-read from ReadingProgress, or <0 when none */
        CoverPixmapRole,
        GeneratedCoverRole,
        DownrankedRole,
        FinishedRole,
    };

    explicit PaperLibrarySectionedModel(QObject *parent = nullptr);
    ~PaperLibrarySectionedModel() override;

    void setSourceModel(PaperLibraryModel *model);
    void setShelf(SmartFilter filter, SectionMode mode);
    void setSmartFilter(SmartFilter filter);
    void setSectionMode(SectionMode mode);
    void setQuery(const QString &query);
    void setExplicitSourceRows(const QList<int> &sourceRows, const QString &label, const QString &emptyText = QString(), bool bypassShelfFilter = false);
    void clearExplicitSourceRows();
    bool hasExplicitSourceRows() const;
    void setCoverForPath(const QString &path, const QVariant &cover, bool generated);
    void setDownranked(const QModelIndex &index, bool downranked);
    void setFinished(const QModelIndex &index, bool finished);
    void reloadFinishedSlugs(); // pick up finished marks made on another shelf's model (shared via config)

    SmartFilter smartFilter() const;
    SectionMode sectionMode() const;
    QString resolvePath(const QModelIndex &index) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    bool canFetchMore(const QModelIndex &parent = QModelIndex()) const override;
    void fetchMore(const QModelIndex &parent = QModelIndex()) override;
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
        QString focusThumbnailPath;
        QString focusThumbnailSource;
        QString focusPath;
        int focusOrder = -1;
        double focusScore = 0.0;
    };

    void rebuild();
    void rebuildPathIndex();
    void resetVisibleRows();
    QString cacheKey() const;
    void clearRowCache();
    QString pathForRow(const Row &row) const;
    QString storedPathForSourceRow(int sourceRow) const;
    bool acceptsSourceRow(int sourceRow) const;
    bool sourceRowDownranked(int sourceRow) const;
    QSet<QString> readDownrankedSlugs() const;
    void saveDownrankedSlugs(const QSet<QString> &slugs) const;
    void applyDownrankedSlugs(const QSet<QString> &slugs);
    void broadcastDownrankedSlugs(const QSet<QString> &slugs);
    bool sourceRowFinished(int sourceRow) const;
    bool titleIsFinished(const QString &title) const;
    void saveFinishedSlugs() const;

    PaperLibraryModel *m_source = nullptr;
    SmartFilter m_smartFilter = Papers;
    SectionMode m_sectionMode = ReadNext;
    QString m_query;
    bool m_explicitRowsActive = false;
    bool m_explicitBypassFilter = false; /**< explicit rows (e.g. adjacent) ignore the shelf smart-filter */
    QList<int> m_explicitSourceRows;
    QString m_explicitRowsLabel;
    QString m_explicitRowsEmptyText;
    QList<Row> m_rows;
    QList<Row> m_allRows;
    QHash<QString, QList<Row>> m_rowCache;
    QHash<QString, QList<int>> m_rowsByPath;
    QHash<QString, QVariant> m_coverPixmaps;
    QSet<QString> m_generatedCoverPaths;
    QString m_feedConfigPath;
    QSet<QString> m_downrankedSlugs;
    QSet<QString> m_finishedSlugs;
    QSet<QString> m_finishedTitles; /**< normalised titles of finished books, so all duplicate rows count */
    void rebuildFinishedTitles();
    // Per-source-row shelf classification (isBook/isTextbook/... as a bitfield). Stable per row —
    // depends only on the row's own text+genre, not on query/downrank/finished — so it survives
    // clearRowCache() and spares rebuild() the ~18k-row regex sweep (the downrank/finished hang).
    // Cleared only when the source data itself changes.
    QHash<int, quint16> m_classifyCache;
};

#endif
