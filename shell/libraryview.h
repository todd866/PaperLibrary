/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_LIBRARYVIEW_H
#define PAPERLIBRARY_LIBRARYVIEW_H

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QListView>
#include <QPersistentModelIndex>
#include <QPoint>
#include <QUrl>
#include <QWidget>

#include "epubcover.h"

class CoverLoader;
class LibraryStore;
class PaperLibraryModel;
class QAction;
class QGraphicsOpacityEffect;
class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QWheelEvent;
class PaperLibrarySectionedModel;
class QPropertyAnimation;
class QProcess;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSplitter;
class QStandardItem;
class QStandardItemModel;
class QTabBar;
class QTimer;
class QToolButton;
class QKeyEvent;

/**
 * The shelf grid. Subclassed only to expose currentChanged() -- a view-level virtual Qt calls
 * whenever the current tile moves (arrow keys, click, programmatic) regardless of which selection
 * model is live. Connecting to QItemSelectionModel::currentChanged is unreliable because a shelf
 * switch can replace the selection model out from under a cached connection; this cannot be orphaned.
 */
class LibraryGridView : public QListView
{
    Q_OBJECT
public:
    using QListView::QListView;
Q_SIGNALS:
    void currentTileChanged(const QModelIndex &current);

protected:
    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
};

/**
 * The document library: a grid of cover tiles for pinned and frequently
 * opened documents, hosted as a real tab in the shell's tab strip — the
 * new-tab page, in Chrome terms. Public defaults stay generic; local/domain
 * shelves are surfaced only when the configured corpus provides a focus
 * manifest for them. When a PaperLibrary corpus is configured and present,
 * the corpus-backed shelves keep the same tile grid while grouping records by
 * reading priority, topic, project, source, year, journal, or publication type.
 */
class LibraryView : public QWidget
{
    Q_OBJECT

public:
    enum Shelf {
        PdfShelf = 0,
        BooksShelf = 1,
        TextbooksShelf = 2,
        MedicineShelf = 3,
        MndShelf = 4,
        WorkShelf = 5,
        FictionShelf = 6,
        NonfictionShelf = 7,
        StarterPackShelf = 8,
        FinishedShelf = 9,
        PapersShelf = 10,
    };
    static constexpr int DocumentShelfCount = 10;

    /** How a shelf arranges its tiles; persisted per shelf. */
    enum ViewMode {
        FrequentMode = 0, /**< type sections, entries pinned/frequency-ranked within each */
        GenreMode = 1,    /**< legacy config token; user-facing "By Type" */
        FolderMode = 2,   /**< sections by parent folder, shared prefix trimmed */
    };

    enum CorpusSearchMode {
        ShelfMetadataSearch = 0,
        FullTextSearch = 1,
    };

    /** Item roles shared by the shelf models and the tile delegate. */
    enum Role {
        UrlRole = Qt::UserRole + 1,
        CoverRole,          /**< QPixmap; unset until the thumbnail arrives */
        PinnedRole,         /**< bool */
        ProgressRole,       /**< double 0..1 from Apple Books, -1 when absent */
        HeaderRole,         /**< legacy bool: not emitted for normal shelves */
        FormatRole,         /**< "PDF"/"EPUB", drawn on the cover placeholder */
        TagsRole,           /**< QStringList; semantic/sorting tags, including publication type */
        DisplayTagsRole,    /**< QStringList; concise human metadata painted under the tile */
        DisplayTitleRole,   /**< reader-facing short tile title; DisplayRole keeps canonical title */
        DescriptionRole,    /**< stored description, else the EPUB's OPF one */
        GeneratedCoverRole, /**< bool: CoverRole holds a generated card, not a render */
        DownrankedRole,     /**< bool: thumbs-down feed signal */
        FinishedReadingRole,/**< bool: long-form item marked completed by the reader */
        LocalFileRole,      /**< QString local path, cached from UrlRole; lets coverArrived match a
                                 completed cover to its tiles without constructing a QUrl per row */
    };

    /** What the delegate paints under a tile's cover. */
    struct TileCaption {
        QString text;
        bool secondary = false; /**< muted description/tag styling, not the title's */
    };

    /**
     * The caption under a tile: the title for kept page renders and real
     * EPUB covers; for generated cards — which already display the title
     * as the artwork — the stored or OPF description, else the tag line,
     * else nothing. Corpus shelves surface the full title and rationale in
     * the inline context strip instead of native hover tooltips.
     */
    static TileCaption tileCaption(const QModelIndex &index);

    /**
     * @param sharedCorpusModel  a PaperLibraryModel owned elsewhere (the Shell) and shared by
     *   every library tab. Passing one is what stops each new tab re-parsing the 21k-row catalog
     *   and rebuilding -- the multi-second freeze telemetry recorded when opening tabs in a burst.
     *   When null (single-view tests, the render rig) the view creates and owns its own model.
     */
    explicit LibraryView(LibraryStore *store, QWidget *parent = nullptr, bool deferInitialRefresh = false,
                         PaperLibraryModel *sharedCorpusModel = nullptr);

    /** Re-read the store and Apple Books; normal shows run it immediately, user-created tabs may defer their first run. */
    void refresh();

    /** Tile urls of a shelf in display order (section headers skipped). */
    QList<QUrl> shelfUrls(Shelf shelf) const;

    /** Rearrange @p shelf, persist the choice and rebuild the shelves. */
    void setViewMode(Shelf shelf, ViewMode mode);
    ViewMode viewMode(Shelf shelf) const;

    /**
     * Filter the shelves as if @p query had been typed into the search
     * field. While a query is active the shelves show a flat, headerless
     * list of the entries whose metadata matches it; an empty query
     * restores the shelf's chosen arrangement instantly.
     */
    void setSearchQuery(const QString &query);

    /** Switch to @p shelf when it is available in this library view. */
    bool showShelf(Shelf shelf);

    /** Enter adjacent-documents mode for the currently selected corpus tile. */
    bool showAdjacentDocumentsForCurrentTile();

public Q_SLOTS:
    /**
     * Open @p url. A @p booksProgress in 0..1 asks the shell to jump to
     * that fraction of the document once loaded; -1 means none.
     */
    void activate(const QUrl &url, double booksProgress = -1.0);

Q_SIGNALS:
    void openClicked();
    void itemActivated(const QUrl &url, double booksProgress);
    void closeRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct ShelfEntry {
        QUrl url;
        QString title;
        QStringList tags;
        QString description;
        QStringList keywords;
        bool pinned = false;
        bool downranked = false;
        bool finishedReading = false;
        int openCount = 0;
        QDateTime lastOpened;
        double progress = -1.0;
        QString format;
        QStringList detailLines;
    };

    /** An ordered group of tiles; title is sort/context metadata, not a row. */
    struct Section {
        QString title;
        QList<ShelfEntry> entries;
    };

    static QList<Section> arrangeSections(const QList<ShelfEntry> &entries, ViewMode mode);
    static QList<Section> arrangePublicationTypeSections(const QList<ShelfEntry> &entries);
    static QString publicationTypeTitle(const ShelfEntry &entry);
    static bool isDocumentShelf(Shelf shelf);
    static QString smartShelfHaystack(const ShelfEntry &entry);
    static QString smartShelfContentHaystack(const ShelfEntry &entry);
    static bool containsAnyNeedle(const QString &haystack, const QStringList &needles);
    static bool containsAnyWord(const QString &haystack, const QStringList &words);
    static bool isTextbookEntry(const ShelfEntry &entry);
    static bool isMedicineEntry(const ShelfEntry &entry);
    static bool isPsychiatryEntry(const ShelfEntry &entry);
    static bool isMndEntry(const ShelfEntry &entry);
    static bool isAnthropologyEntry(const ShelfEntry &entry);
    static bool isPoliticsEntry(const ShelfEntry &entry);
    static bool isWorkEntry(const ShelfEntry &entry);
    static bool isFictionEntry(const ShelfEntry &entry);
    static bool isNonfictionEntry(const ShelfEntry &entry);
    static QString focusTagFor(const ShelfEntry &entry);
    static QString displaySubjectForTile(const ShelfEntry &entry, const EpubCover::Metadata *epubMetadata = nullptr);
    static QStringList displayTagsForTile(const ShelfEntry &entry, const EpubCover::Metadata *epubMetadata = nullptr);
    static void enrichShelfEntry(ShelfEntry &entry, const EpubCover::Metadata *epubMetadata = nullptr);
    static bool shelfHasReadingProgress(const QList<ShelfEntry> &entries);
    static QList<ShelfEntry> loadStarterPackEntries();
    static bool matchesQuery(const ShelfEntry &entry, const QString &query);
    QStandardItemModel *modelForShelf(Shelf shelf) const;
    void addShelfTab(Shelf shelf, const QString &label);
    int tabIndexForShelf(Shelf shelf) const;
    void populate(QStandardItemModel *model, const QList<ShelfEntry> &entries, ViewMode mode);
    void populate(Shelf shelf, QStandardItemModel *model, const QList<ShelfEntry> &entries, ViewMode mode);
    void populateSections(Shelf shelf, QStandardItemModel *model, const QList<Section> &sections);
    QList<ShelfEntry> displayEntriesForShelf(Shelf shelf) const;
    void appendMoreDocumentShelfRows(Shelf shelf);
    void maybeFetchMoreRowsForActiveShelf();
    QStandardItem *makeTileItem(const ShelfEntry &entry);
    /** Borrow generated metadata from the loaded corpus for local PDFs/books. */
    void enrichShelfEntryFromCorpus(ShelfEntry &entry) const;
    /** OPF metadata for @p filePath, parsed once per session per book. */
    const EpubCover::Metadata &epubMetadataFor(const QString &filePath);
    /** Build the corpus shelf's tab, model and sectioned tile model (corpus present). */
    void setupPapersShelf();
    void shelfChanged(int index);
    void renderPendingShelf();
    void renderShelf(Shelf shelf, bool animate);
    bool usesCorpusList(Shelf shelf) const;
    void publishLocalBooksToCorpus();
    PaperLibrarySectionedModel *paperSectionsForShelf(Shelf shelf) const;
    PaperLibrarySectionedModel *activePaperSections() const;
    void attachCorpusShelf(Shelf shelf);
    void configureCorpusShelf(Shelf shelf);
    void setPaperSectionMode(Shelf shelf, int mode);
    int paperSectionMode(Shelf shelf) const;
    void syncPaperSectionButton();
    CorpusSearchMode corpusSearchMode() const;
    void setCorpusSearchMode(CorpusSearchMode mode);
    void syncCorpusSearchButton();
    void setCorpusResultMode(const QString &label, const QString &detail = QString());
    void clearCorpusResultMode();
    void syncCorpusResultButton();
    bool clearActiveCorpusResult();
    void showShelfGuide();
    void requestCorpusCovers();
    void requestNextCorpusCoverBatch();
    int requestCorpusCoversForSections(PaperLibrarySectionedModel *sections, int startRow, int maxRequests);
    void scheduleCorpusPrewarm();
    void prewarmNextCorpusShelf();
    /** Load the corpus catalog on first entry; pick up mtime changes later. */
    void ensurePapersFresh();
    /** Keep backend-published stale/unknown index health visible on corpus shelves. */
    void updateCorpusHealthNotice();
    /** Lay the floating notices over the grid (health at top, action toast at bottom). */
    void positionOverlayNotices();
    void applyChromePalette();
    /** Non-modal inline notice over the corpus shelf; auto-hides by default. */
    void showPaperNotice(const QString &text, bool autoHide = true);
    void connectGridSelectionContext();
    void updateSelectedTileContext(const QModelIndex &index);
    bool downrankTile(const QModelIndex &index);
    void resetTileDrag();
    bool showAdjacentDocumentsForIndex(const QModelIndex &index);
    void tileClicked(const QModelIndex &index);
    void buildDetailRail();
    void backfillReadingProgressTitles();
    void updateDetailRail(const QModelIndex &current);
    void refreshDetailRail();
    void setDetailRailCollapsed(bool collapsed);
    void activateCurrentTile();
    void selectFirstTile();
    void showContextMenu(const QPoint &pos);
    // Record a reader-reported problem with a book (wrong shelf, bad cover, bad title, other) to
    // <corpus>/flags.jsonl. The morning check reads these and surfaces / acts on them.
    void flagBook(const QModelIndex &index, const QString &kind, const QString &note = QString());
    // Pop the flag-reason menu for the currently selected tile (bound to the "f" key).
    void flagCurrentTile();
    void editMetadata(const QUrl &url);
    void coverArrived(const QString &filePath, const QString &coverPath);
    void syncViewModeButton();
    void animateGridIn();
    void showStartupPlaceholder();
    /** Reassert the tiled QListView geometry after model/theme/shelf changes. */
    void configureTileGrid();
    Shelf activeShelf() const;
    QString searchQuery() const;
    /** Filter (or plainly arrange) both shelf models and (re)start the content search. */
    void applySearch();
    void rebuildShelves();
    void startContentSearch();
    void cancelContentSearch();
    void scheduleRefresh(int delayMs = 1);
    /**
     * Layer 2 of the search: reduce @p hitPaths (file paths whose text
     * content matched @p query, e.g. mdfind's output) to the entries of
     * @p shelf that layer 1 did not already match, and append them as more
     * tiles. Deterministic over its inputs so tests can feed a fake hit list.
     */
    void applyContentSearchResults(Shelf shelf, const QString &query, const QStringList &hitPaths);

    friend class MainShellTest;

    LibraryStore *m_store;
    QTabBar *m_shelfSwitch;
    QPushButton *m_openButton;
    QToolButton *m_viewModeButton;
    QAction *m_viewModeActions[3];
    QToolButton *m_paperSectionButton = nullptr;
    QAction *m_paperSectionActions[7] = {};
    QToolButton *m_corpusSearchButton = nullptr;
    QAction *m_corpusSearchActions[2] = {};
    QToolButton *m_corpusResultButton = nullptr;
    QString m_corpusResultLabel;
    QString m_corpusResultDetail;
    CorpusSearchMode m_corpusSearchMode = ShelfMetadataSearch;
    ViewMode m_viewModes[DocumentShelfCount] = {};
    int m_paperSectionModes[DocumentShelfCount + 1] = {};
    QLineEdit *m_searchField;
    QTimer *m_searchDebounce;
    QTimer *m_shelfRenderTimer = nullptr;
    QProcess *m_contentSearch = nullptr;
    bool m_applyingChromePalette = false;
    bool m_deferInitialRefresh = false;
    bool m_hasShown = false;
    bool m_refreshPending = false;
    QList<ShelfEntry> m_shelfEntries[DocumentShelfCount];
    QList<Shelf> m_visibleShelves;
    QListView *m_grid = nullptr;
    // Left detail rail: shows the selected tile's cover, metadata, blurb, topics and actions.
    QSplitter *m_shelfSplitter = nullptr;
    QWidget *m_detailRail = nullptr;
    QToolButton *m_detailToggle = nullptr;
    QLabel *m_detailCover = nullptr;
    QLabel *m_detailTitle = nullptr;
    QLabel *m_detailMeta = nullptr;
    QProgressBar *m_detailProgress = nullptr;
    QLabel *m_detailBlurb = nullptr;
    QWidget *m_detailTopicsBox = nullptr;
    QLabel *m_detailReason = nullptr;
    QLabel *m_detailProvenance = nullptr;
    QLabel *m_detailPlaceholder = nullptr;
    QWidget *m_detailBody = nullptr;
    QScrollArea *m_detailScroll = nullptr;
    QPushButton *m_detailOpen = nullptr;
    QPushButton *m_detailFinish = nullptr;
    QPersistentModelIndex m_detailIndex;
    bool m_detailRailCollapsed = false;
    bool m_progressTitlesBackfilled = false;
    QStandardItemModel *m_pdfModel;
    QStandardItemModel *m_booksModel;
    QStandardItemModel *m_textbooksModel;
    QStandardItemModel *m_medicineModel;
    QStandardItemModel *m_mndModel;
    QStandardItemModel *m_workModel;
    QStandardItemModel *m_fictionModel;
    QStandardItemModel *m_nonfictionModel;
    QStandardItemModel *m_starterPackModel;
    QStandardItemModel *m_finishedModel;
    CoverLoader *m_coverLoader;
    QTimer *m_corpusCoverWarmupTimer = nullptr;
    int m_nextCorpusCoverRow = 0;
    QList<Shelf> m_corpusPrewarmQueue;
    int m_corpusPrewarmIndex = 0;
    bool m_corpusPrewarmActive = false;
    int m_configuredGridCorpus = -1;
    QGraphicsOpacityEffect *m_gridFadeEffect = nullptr;
    QPropertyAnimation *m_gridFadeAnimation = nullptr;
    QMetaObject::Connection m_gridSelectionConnection;
    QPersistentModelIndex m_tileDragIndex;
    QPoint m_tileDragPressPos;
    bool m_tileDragCandidate = false;
    bool m_tileDragArmed = false;
    bool m_fetchingMoreRows = false;
    QElapsedTimer m_lastShelfRender;
    int m_pendingShelfIndex = -1;
    int m_documentShelfRowLimit[DocumentShelfCount] = {};
    QString m_documentShelfQuery;
    QHash<QString, EpubCover::Metadata> m_epubMetadata; // per-session OPF cache

    // The PaperLibrary corpus shelf; all null when no corpus is configured
    QString m_paperCorpusDir;
    PaperLibraryModel *m_paperModel = nullptr;      /**< the active corpus model (shared or own) */
    PaperLibraryModel *m_sharedCorpusModel = nullptr; /**< non-null when the Shell owns it, shared across tabs */
    PaperLibrarySectionedModel *m_paperSections[DocumentShelfCount + 1] = {};
    bool m_paperSectionAttached[DocumentShelfCount + 1] = {};
    QLabel *m_corpusHealthNotice = nullptr;
    QLabel *m_paperNotice = nullptr;
    QTimer *m_paperNoticeTimer = nullptr;
};

#endif
