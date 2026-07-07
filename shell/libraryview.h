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
class PaperLibrarySectionedModel;
class QPropertyAnimation;
class QProcess;
class QPushButton;
class QStandardItem;
class QStandardItemModel;
class QTabBar;
class QTimer;
class QToolButton;

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

    explicit LibraryView(LibraryStore *store, QWidget *parent = nullptr, bool deferInitialRefresh = false);

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
    void prebuildCorpusShelves();
    void scheduleCorpusPrewarm();
    void prewarmNextCorpusShelf();
    /** Load the corpus catalog on first entry; pick up mtime changes later. */
    void ensurePapersFresh();
    void applyChromePalette();
    /** Non-modal inline notice over the corpus shelf; auto-hides by default. */
    void showPaperNotice(const QString &text, bool autoHide = true);
    void connectGridSelectionContext();
    void updateSelectedTileContext(const QModelIndex &index);
    bool downrankTile(const QModelIndex &index);
    void resetTileDrag();
    bool showAdjacentDocumentsForIndex(const QModelIndex &index);
    void tileClicked(const QModelIndex &index);
    void activateCurrentTile();
    void selectFirstTile();
    void showContextMenu(const QPoint &pos);
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
    PaperLibraryModel *m_paperModel = nullptr;
    PaperLibrarySectionedModel *m_paperSections[DocumentShelfCount + 1] = {};
    bool m_paperSectionAttached[DocumentShelfCount + 1] = {};
    QLabel *m_paperNotice = nullptr;
    QTimer *m_paperNoticeTimer = nullptr;
};

#endif
