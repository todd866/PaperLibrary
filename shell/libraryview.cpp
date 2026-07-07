/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "libraryview.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QAbstractItemModel>
#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEasingCurve>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QImage>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QPointer>
#include <QProcess>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTextLayout>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#if !defined(Q_OS_MACOS)
#include <KIO/OpenFileManagerWindowJob>
#endif

#include "applebooksprogress.h"
#include "chromecolors.h"
#include "covergenerator.h"
#include "coverheuristic.h"
#include "epubcover.h"
#include "epubimporter.h"
#include "librarystore.h"
#include "paperlibrarymodel.h"

// Tile geometry: comfortable grid metrics around a book-cover sized thumbnail
static constexpr int TileWidth = 172;
static constexpr int CorpusTileWidth = 226;
static constexpr int TilePadding = 12;
static constexpr int CoverWidth = TileWidth - 2 * TilePadding;
static constexpr int CorpusCoverWidth = CorpusTileWidth - 2 * TilePadding;
static constexpr int CoverHeight = 178;
static constexpr int CoverRadius = 6;
static constexpr int TitleGap = 8;
static constexpr int TitleLines = 3;
static constexpr int MetadataLines = 2;
static constexpr int TagGap = 2;
static constexpr int ProgressGap = 7;
static constexpr int ProgressBarHeight = 4;
static constexpr int GridSpacing = 12;
static constexpr int CorpusCoverHeight = 198;
static constexpr int DownrankDragDistance = 48;
static constexpr int InitialDocumentShelfRows = 96;
static constexpr int DocumentShelfFetchBatchRows = 96;
static constexpr int DocumentShelfFetchThresholdPx = 900;

/**
 * Renders cover thumbnails asynchronously into a disk cache keyed by
 * (path, mtime), at most two renders at a time. EPUBs use their embedded
 * cover; PDFs use QtPdf in-process instead of macOS QuickLook so shelf
 * warmup does not spawn GUI helper processes. Successful PDF renders are
 * kept even when they are plain text pages: for papers, the first page is
 * still a file-derived fingerprint and is more truthful than a synthetic
 * card. Renders that fail fall back to a generated typographic card.
 * Files that yield neither render nor card are
 * remembered for the session so they keep their placeholder without a
 * retry storm.
 */
class CoverLoader : public QObject
{
    Q_OBJECT

public:
    explicit CoverLoader(QWidget *host)
        : QObject(host)
        , m_host(host)
        , m_cacheDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/library-covers"))
    {
        QDir().mkpath(m_cacheDir);
    }

    ~CoverLoader() override
    {
        const QList<QThread *> threads = findChildren<QThread *>();
        for (QThread *thread : threads) {
            thread->disconnect(this);
            thread->requestInterruption();
            thread->wait(1000);
        }
    }

    /** Whether @p coverPath names a generated card rather than a kept render. */
    static bool isGeneratedCoverPath(const QString &coverPath)
    {
        return QFileInfo(coverPath).fileName().startsWith(QString(DesignVersion) + QLatin1Char('-'));
    }

    /** Cached cover (page render or generated card) for @p filePath, or an empty string. */
    QString cachedCoverPath(const QString &filePath, const CoverGenerator::CoverSpec &spec) const
    {
        const QString rendered = coverPath(filePath);
        if (QFileInfo::exists(rendered)) {
            return rendered;
        }
        const QString generated = generatedCoverPath(filePath, spec);
        return QFileInfo::exists(generated) ? generated : QString();
    }

    int pendingWorkCount() const
    {
        return m_pending.size();
    }

    void requestCover(const QString &filePath, const CoverGenerator::CoverSpec &spec)
    {
        if (m_pending.contains(filePath)) {
            return;
        }
        if (filePath.endsWith(QLatin1String(".epub"), Qt::CaseInsensitive)) {
            // An epub carries its cover in its own zip: extract it directly
            // and avoid sending it through the PDF renderer.
            if (m_failed.contains(filePath) || !extractEpubCover(filePath)) {
                m_failed.insert(filePath); // no embedded image; don't reparse
                generateTypographic(filePath, spec);
            }
            return;
        }
        if (m_failed.contains(filePath)) {
            // Failed on this file before: straight to the typographic card.
            generateTypographic(filePath, spec);
            return;
        }
        m_pending.insert(filePath);
        m_specs.insert(filePath, spec);
        m_queue.append(filePath);
        startNext();
    }

Q_SIGNALS:
    void coverReady(const QString &filePath, const QString &coverPath);

private:
    // The cover design's version, shared by BOTH cache families. It salts
    // every cache key, so bumping it retires all stale artifacts in place —
    // including page renders cached by an older design whose classifier
    // verdicts (or lack of one) no longer hold.
    static constexpr QLatin1StringView DesignVersion {"tg5"};

    QString coverPath(const QString &filePath) const
    {
        // Keyed by (design, path, mtime): a changed file re-renders under
        // a new name, and a design bump retires the whole render family.
        const qint64 mtime = QFileInfo(filePath).lastModified().toMSecsSinceEpoch();
        const QByteArray key = QCryptographicHash::hash(QStringLiteral("%1:%2:%3").arg(DesignVersion, filePath).arg(mtime).toUtf8(), QCryptographicHash::Sha1).toHex();
        return m_cacheDir + QStringLiteral("/r-") + DesignVersion + QLatin1Char('-') + QString::fromLatin1(key) + QStringLiteral(".png");
    }

    QString generatedCoverPath(const QString &filePath, const CoverGenerator::CoverSpec &spec) const
    {
        // Same design salt as the renders, plus everything the card says
        // and the light/dark treatment
        const qint64 mtime = QFileInfo(filePath).lastModified().toMSecsSinceEpoch();
        const bool darkMode = m_host->palette().color(QPalette::Window).lightness() < 128;
        const QString key = QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8").arg(DesignVersion, filePath).arg(mtime).arg(spec.title, spec.authors, spec.yearJournal, spec.tag, darkMode ? QStringLiteral("dark") : QStringLiteral("light"));
        const QByteArray hash = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();
        return m_cacheDir + QLatin1Char('/') + DesignVersion + QLatin1Char('-') + QString::fromLatin1(hash) + QStringLiteral(".png");
    }

    /**
     * Paint and cache the typographic card for @p filePath; announces it
     * through coverReady. Returns false for untitled entries — they keep
     * the plain placeholder.
     */
    bool generateTypographic(const QString &filePath, const CoverGenerator::CoverSpec &spec)
    {
        if (spec.title.isEmpty()) {
            return false;
        }
        const QString target = generatedCoverPath(filePath, spec);
        if (!QFileInfo::exists(target)) {
            // 2x the tile's cover box so the card stays crisp on retina
            // displays; the delegate scales it down like any render
            const QImage card = CoverGenerator::generate(spec, QSize(CoverWidth, CoverHeight) * 2, m_host->palette());
            if (card.isNull() || !card.save(target)) {
                return false;
            }
        }
        // Queued: a tile may request its cover before it is in the model
        QMetaObject::invokeMethod(this, [this, filePath, target] { Q_EMIT coverReady(filePath, target); }, Qt::QueuedConnection);
        return true;
    }

    /** Extract and cache an epub's embedded cover; false when it has none. */
    bool extractEpubCover(const QString &filePath)
    {
        const QString target = coverPath(filePath);
        if (!QFileInfo::exists(target)) {
            QImage cover = EpubCover::extract(filePath);
            if (cover.isNull()) {
                return false;
            }
            if (cover.width() > 512 || cover.height() > 512) {
                // Match the PDF render cache budget so warmup stays bounded.
                cover = cover.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            if (!cover.save(target)) {
                return false;
            }
        }
        // Queued: a tile may request its cover before it is in the model
        QMetaObject::invokeMethod(this, [this, filePath, target] { Q_EMIT coverReady(filePath, target); }, Qt::QueuedConnection);
        return true;
    }

    void startNext()
    {
        while (m_running < 2 && !m_queue.isEmpty()) {
            startRender(m_queue.takeFirst());
        }
    }

    void startRender(const QString &filePath)
    {
        ++m_running;
        const QString target = coverPath(filePath);
        QPointer<CoverLoader> guard(this);
        QThread *thread = QThread::create([guard, filePath, target]() {
            QString readyPath;
            QPdfDocument document;
            if (document.load(filePath) == QPdfDocument::Error::None && document.status() == QPdfDocument::Status::Ready && document.pageCount() > 0) {
                QSizeF pageSize = document.pagePointSize(0);
                if (pageSize.isEmpty()) {
                    pageSize = QSizeF(612, 792);
                }
                QSize renderSize = pageSize.toSize().scaled(QSize(512, 512), Qt::KeepAspectRatio);
                renderSize = renderSize.expandedTo(QSize(96, 96));
                QPdfDocumentRenderOptions options;
                options.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
                const QImage image = document.render(0, renderSize, options);
                if (!image.isNull()) {
                    QFile::remove(target);
                    if (image.save(target)) {
                        readyPath = target;
                    }
                }
            }
            if (guard) {
                QMetaObject::invokeMethod(guard, [guard, filePath, readyPath]() {
                    if (guard) {
                        guard->finishRender(filePath, readyPath);
                    }
                }, Qt::QueuedConnection);
            }
        });
        thread->setParent(this);
        thread->setObjectName(QStringLiteral("PaperLibraryPdfCoverRender"));
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }

    void finishRender(const QString &filePath, const QString &readyPath)
    {
        --m_running;
        m_pending.remove(filePath);
        const CoverGenerator::CoverSpec spec = m_specs.take(filePath);
        if (!readyPath.isEmpty()) {
            Q_EMIT coverReady(filePath, readyPath);
        } else {
            // The file could not be rendered by QtPdf; do not retry this
            // session. Titled entries get the quiet typographic fallback.
            m_failed.insert(filePath);
            generateTypographic(filePath, spec);
        }
        startNext();
    }

    QWidget *m_host; // supplies the palette for generated cards
    QString m_cacheDir;
    QStringList m_queue;
    QSet<QString> m_pending;
    QHash<QString, CoverGenerator::CoverSpec> m_specs;
    QSet<QString> m_failed;
    int m_running = 0;
};

// The per-shelf view mode lives in the PaperLibrary app config.
static KConfigGroup partGeneralConfig()
{
    const QString overridePath = qEnvironmentVariable("PAPERLIBRARY_CONFIG_PATH");
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString configFilePath =
        !overridePath.isEmpty() ? overridePath : (configDir.isEmpty() ? QStringLiteral("paperlibraryrc") : configDir + QLatin1String("/paperlibraryrc"));
    return KSharedConfig::openConfig(configFilePath, KConfig::SimpleConfig)->group(QStringLiteral("General"));
}

static const char *viewModeConfigKey(LibraryView::Shelf shelf)
{
    switch (shelf) {
    case LibraryView::BooksShelf:
        return "LibraryViewModeBooks";
    case LibraryView::TextbooksShelf:
        return "LibraryViewModeTextbooks";
    case LibraryView::MedicineShelf:
        return "LibraryViewModeMedicine";
    case LibraryView::MndShelf:
        return "LibraryViewModeMnd";
    case LibraryView::WorkShelf:
        return "LibraryViewModeWork";
    case LibraryView::FictionShelf:
        return "LibraryViewModeFiction";
    case LibraryView::NonfictionShelf:
        return "LibraryViewModeNonfiction";
    case LibraryView::StarterPackShelf:
        return "LibraryViewModeStarterPack";
    case LibraryView::FinishedShelf:
        return "LibraryViewModeFinished";
    case LibraryView::PdfShelf:
        return "LibraryViewModeRecent";
    case LibraryView::PapersShelf:
        break;
    }
    return "LibraryViewModeRecent";
}

static bool appleBooksScanEnabled()
{
    return partGeneralConfig().readEntry("ScanAppleBooksOnStartup", true);
}

static QString starterPackDir()
{
    return partGeneralConfig().readEntry("StarterPackPath", QString(QDir::homePath() + QStringLiteral("/Projects/PaperLibrary/starter-public-domain")));
}

static bool focusManifestExists(const QString &corpusDir, const QString &shelfName)
{
    return !corpusDir.isEmpty() && QFileInfo::exists(corpusDir + QStringLiteral("/focus/") + shelfName + QStringLiteral("/manifest.json"));
}

// Serialized with the original enum choice names so existing config values keep
// working.
static QString viewModeName(LibraryView::ViewMode mode)
{
    switch (mode) {
    case LibraryView::GenreMode:
        return QStringLiteral("Genre");
    case LibraryView::FolderMode:
        return QStringLiteral("Folder");
    case LibraryView::FrequentMode:
        break;
    }
    return QStringLiteral("Frequent");
}

static LibraryView::ViewMode viewModeFromName(const QString &name)
{
    if (name == QLatin1String("Genre")) {
        return LibraryView::GenreMode;
    }
    if (name == QLatin1String("Folder")) {
        return LibraryView::FolderMode;
    }
    return LibraryView::FrequentMode;
}

static const char *paperSectionModeConfigKey(LibraryView::Shelf shelf)
{
    switch (shelf) {
    case LibraryView::BooksShelf:
        return "PaperSectionModeBooks";
    case LibraryView::TextbooksShelf:
        return "PaperSectionModeTextbooks";
    case LibraryView::MedicineShelf:
        return "PaperSectionModeMedicine";
    case LibraryView::MndShelf:
        return "PaperSectionModeMnd";
    case LibraryView::WorkShelf:
        return "PaperSectionModeWork";
    case LibraryView::FictionShelf:
        return "PaperSectionModeFiction";
    case LibraryView::NonfictionShelf:
        return "PaperSectionModeNonfiction";
    case LibraryView::PapersShelf:
        return "PaperSectionModePapers";
    case LibraryView::FinishedShelf:
    case LibraryView::StarterPackShelf:
    case LibraryView::PdfShelf:
        break;
    }
    return "PaperSectionModePapers";
}

static int defaultPaperSectionModeForShelf(LibraryView::Shelf shelf)
{
    switch (shelf) {
    case LibraryView::BooksShelf:
    case LibraryView::TextbooksShelf:
    case LibraryView::NonfictionShelf:
        return PaperLibrarySectionedModel::ByTopic;
    case LibraryView::WorkShelf:
        return PaperLibrarySectionedModel::ByProject;
    case LibraryView::MedicineShelf:
    case LibraryView::MndShelf:
    case LibraryView::FictionShelf:
    case LibraryView::PapersShelf:
    case LibraryView::FinishedShelf:
    case LibraryView::StarterPackShelf:
    case LibraryView::PdfShelf:
        break;
    }
    return PaperLibrarySectionedModel::ReadNext;
}

static PaperLibrarySectionedModel::SmartFilter paperFilterForShelf(LibraryView::Shelf shelf)
{
    switch (shelf) {
    case LibraryView::TextbooksShelf:
        return PaperLibrarySectionedModel::Textbooks;
    case LibraryView::MedicineShelf:
        return PaperLibrarySectionedModel::Medicine;
    case LibraryView::MndShelf:
        return PaperLibrarySectionedModel::Mnd;
    case LibraryView::WorkShelf:
        return PaperLibrarySectionedModel::Work;
    case LibraryView::FictionShelf:
        return PaperLibrarySectionedModel::Fiction;
    case LibraryView::NonfictionShelf:
        return PaperLibrarySectionedModel::Nonfiction;
    case LibraryView::PapersShelf:
    case LibraryView::StarterPackShelf:
    case LibraryView::FinishedShelf:
    case LibraryView::BooksShelf:
    case LibraryView::PdfShelf:
        break;
    }
    return PaperLibrarySectionedModel::Papers;
}

static QString paperSectionModeName(int mode)
{
    switch (mode) {
    case PaperLibrarySectionedModel::ByTopic:
        return QStringLiteral("Topic");
    case PaperLibrarySectionedModel::ByProject:
        return QStringLiteral("Project");
    case PaperLibrarySectionedModel::ByType:
        return QStringLiteral("Type");
    case PaperLibrarySectionedModel::BySource:
        return QStringLiteral("Source");
    case PaperLibrarySectionedModel::ByYear:
        return QStringLiteral("Year");
    case PaperLibrarySectionedModel::ByJournal:
        return QStringLiteral("Journal");
    case PaperLibrarySectionedModel::ReadNext:
        break;
    }
    return QStringLiteral("Focus");
}

static int paperSectionModeFromName(const QString &name, int fallback)
{
    if (name == QLatin1String("Topic")) {
        return PaperLibrarySectionedModel::ByTopic;
    }
    if (name == QLatin1String("Project")) {
        return PaperLibrarySectionedModel::ByProject;
    }
    if (name == QLatin1String("Type")) {
        return PaperLibrarySectionedModel::ByType;
    }
    if (name == QLatin1String("Source")) {
        return PaperLibrarySectionedModel::BySource;
    }
    if (name == QLatin1String("Year")) {
        return PaperLibrarySectionedModel::ByYear;
    }
    if (name == QLatin1String("Journal")) {
        return PaperLibrarySectionedModel::ByJournal;
    }
    if (name == QLatin1String("Focus")) {
        return PaperLibrarySectionedModel::ReadNext;
    }
    return fallback;
}

static QString corpusSearchModeName(LibraryView::CorpusSearchMode mode)
{
    switch (mode) {
    case LibraryView::FullTextSearch:
        return QStringLiteral("FullText");
    case LibraryView::ShelfMetadataSearch:
        break;
    }
    return QStringLiteral("Shelf");
}

static LibraryView::CorpusSearchMode corpusSearchModeFromName(const QString &name)
{
    if (name == QLatin1String("FullText")) {
        return LibraryView::FullTextSearch;
    }
    return LibraryView::ShelfMetadataSearch;
}

static QString compactPublicationTypeKey(QString label)
{
    label = label.trimmed().toCaseFolded();
    label.remove(QLatin1Char(' '));
    label.remove(QLatin1Char('-'));
    label.remove(QLatin1Char('_'));
    return label;
}

static QString titleCasedLabel(QString label)
{
    label.replace(QLatin1Char('_'), QLatin1Char(' '));
    label.replace(QLatin1Char('-'), QLatin1Char(' '));
    label = label.simplified();
    if (label.isEmpty()) {
        return label;
    }

    if (label != label.toUpper() && label != label.toLower()) {
        return label;
    }

    QStringList words = label.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (QString &word : words) {
        word.replace(0, 1, word.left(1).toUpper());
    }
    return words.join(QLatin1Char(' '));
}

static int publicationTypeRank(const QString &title)
{
    const QString key = compactPublicationTypeKey(title);
    if (key == QLatin1String("textbook") || key == QLatin1String("textbooks")) {
        return 0;
    }
    if (key == QLatin1String("book") || key == QLatin1String("books")) {
        return 1;
    }
    if (key == QLatin1String("paper") || key == QLatin1String("papers")) {
        return 2;
    }
    if (key == QLatin1String("manuscript") || key == QLatin1String("manuscripts")) {
        return 3;
    }
    if (key == QLatin1String("peerreview") || key == QLatin1String("peerreviews") || key == QLatin1String("review") || key == QLatin1String("reviews")) {
        return 4;
    }
    if (key == QLatin1String("unidentified") || key == QLatin1String("unknown") || key == QLatin1String("uncategorized") || key == QLatin1String("untagged")) {
        return 900;
    }
    if (key == QLatin1String("other") || key == QLatin1String("misc") || key == QLatin1String("miscellaneous")) {
        return 901;
    }
    return 100;
}

static bool isPublicationTypeKey(const QString &key)
{
    return key == QLatin1String("textbook") || key == QLatin1String("textbooks") || key == QLatin1String("book") || key == QLatin1String("books")
        || key == QLatin1String("paper") || key == QLatin1String("papers") || key == QLatin1String("manuscript") || key == QLatin1String("manuscripts")
        || key == QLatin1String("peerreview") || key == QLatin1String("peerreviews") || key == QLatin1String("review") || key == QLatin1String("reviews")
        || key == QLatin1String("guideline") || key == QLatin1String("guidelines") || key == QLatin1String("other") || key == QLatin1String("misc")
        || key == QLatin1String("miscellaneous");
}

static QColor blendColors(const QColor &base, const QColor &over, double amount)
{
    return QColor::fromRgbF(base.redF() * (1.0 - amount) + over.redF() * amount, base.greenF() * (1.0 - amount) + over.greenF() * amount, base.blueF() * (1.0 - amount) + over.blueF() * amount);
}

static QFont smallerFont(const QFont &base)
{
    QFont font = base;
    font.setPointSizeF(base.pointSizeF() * 0.85);
    return font;
}

static QString joinCompact(const QStringList &parts)
{
    QStringList kept;
    for (const QString &part : parts) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty() && trimmed != QLatin1String("None")) {
            kept.append(trimmed);
        }
    }
    return kept.join(QStringLiteral(" · "));
}

static QString libraryTileTooltip(const QString &title, const QStringList &detailLines)
{
    QStringList lines;
    const QString cleanTitle = title.trimmed();
    if (!cleanTitle.isEmpty()) {
        lines.append(cleanTitle);
    }
    for (const QString &line : detailLines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && trimmed != cleanTitle && !lines.contains(trimmed)) {
            lines.append(trimmed);
        }
    }
    return lines.join(QLatin1Char('\n'));
}

static QString progressTooltipLine(double progress)
{
    if (progress < 0.0) {
        return QString();
    }
    return i18nc("@info:tooltip Apple Books reading progress", "Progress: %1%", qRound(qBound(0.0, progress, 1.0) * 100));
}

static QString firstCreatorForDisplay(QString creators)
{
    creators = creators.simplified();
    if (creators.isEmpty()) {
        return QString();
    }
    const QString normalized = creators;
    const QList<QChar> hardSeparators = {QLatin1Char(';'), QLatin1Char('|')};
    int cut = -1;
    for (const QChar separator : hardSeparators) {
        const int index = normalized.indexOf(separator);
        if (index > 0 && (cut < 0 || index < cut)) {
            cut = index;
        }
    }
    const int andIndex = normalized.indexOf(QStringLiteral(" and "), 0, Qt::CaseInsensitive);
    if (andIndex > 0 && (cut < 0 || andIndex < cut)) {
        cut = andIndex;
    }
    const int commaIndex = normalized.indexOf(QStringLiteral(", "));
    if (commaIndex > 0 && (cut < 0 || commaIndex < cut)) {
        cut = commaIndex;
    }
    if (cut > 0) {
        creators = creators.left(cut).trimmed();
    }
    creators.remove(QRegularExpression(QStringLiteral("\\s*\\((?:editor|ed\\.?|author)\\)\\s*$"), QRegularExpression::CaseInsensitiveOption));
    return creators.simplified();
}

static QString compactMetadataLine(QString line)
{
    line = line.simplified();
    line.remove(QRegularExpression(QStringLiteral("\\s*,?\\s*(?:M\\.?D\\.?|Ph\\.?D\\.?)\\.?$"), QRegularExpression::CaseInsensitiveOption));
    line.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    line = line.simplified();

    static const QHash<QString, QString> replacements = {
        {QStringLiteral("World War II"), QStringLiteral("WWII history")},
        {QStringLiteral("World War I"), QStringLiteral("WWI history")},
        {QStringLiteral("Medicine"), QStringLiteral("Medical")},
    };
    const QString replacement = replacements.value(line);
    if (!replacement.isEmpty()) {
        return replacement;
    }

    // The tile face has a fixed two-line budget. Tooltips keep the full
    // metadata; here, prefer a short stable label over a clipped sentence.
    static constexpr int SoftLimit = 24;
    if (line.size() <= SoftLimit) {
        return line;
    }

    QStringList words = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.size() >= 3) {
        const QString compact = words.first().left(1) + QStringLiteral(". ") + words.last();
        if (compact.size() <= SoftLimit) {
            return compact;
        }
    }
    return line.left(SoftLimit - 1).trimmed() + QStringLiteral("…");
}

static bool hasMultipleCreators(const QString &creators)
{
    const QString normalized = creators.simplified();
    return normalized.contains(QLatin1Char(';')) || normalized.contains(QLatin1Char('|')) || normalized.contains(QStringLiteral(" and "), Qt::CaseInsensitive)
        || normalized.contains(QRegularExpression(QStringLiteral(",\\s+[^,;]+\\s+[^,;]+")));
}

static QString surnameForCitation(QString creator)
{
    creator = firstCreatorForDisplay(creator);
    creator.remove(QRegularExpression(QStringLiteral("\\s*,?\\s*(?:M\\.?D\\.?|Ph\\.?D\\.?|FRACP|FAAN)\\.?$"), QRegularExpression::CaseInsensitiveOption));
    creator = creator.simplified();
    if (creator.isEmpty()) {
        return QString();
    }
    const QStringList words = creator.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (words.size() == 1) {
        return words.constFirst();
    }
    const QString last = words.constLast();
    const QString previous = words.value(words.size() - 2);
    const QString previousKey = previous.toCaseFolded();
    static const QSet<QString> particles = {QStringLiteral("da"),  QStringLiteral("de"),  QStringLiteral("del"), QStringLiteral("della"), QStringLiteral("den"),
                                            QStringLiteral("der"), QStringLiteral("di"),  QStringLiteral("dos"), QStringLiteral("du"),    QStringLiteral("la"),
                                            QStringLiteral("le"),  QStringLiteral("van"), QStringLiteral("von")};
    if (particles.contains(previousKey)) {
        return previous + QLatin1Char(' ') + last;
    }
    return last;
}

static QString yearFromMetadataLine(const QString &line)
{
    const QRegularExpression yearExpression(QStringLiteral("\\b(19|20)\\d{2}\\b"));
    const QRegularExpressionMatch match = yearExpression.match(line);
    return match.hasMatch() ? match.captured(0) : QString();
}

static QString citationLabelForPaper(const QString &authors, const QString &year)
{
    const QString surname = surnameForCitation(authors);
    QString cleanYear = year.simplified();
    if (cleanYear.isEmpty()) {
        cleanYear = yearFromMetadataLine(authors);
    }
    if (surname.isEmpty()) {
        return cleanYear;
    }
    return joinCompact({surname + (hasMultipleCreators(authors) ? QStringLiteral(" et al.") : QString()), cleanYear}).replace(QStringLiteral(" · "), QStringLiteral(" "));
}

static QString cleanPaperTitleForThesis(QString title)
{
    title = title.simplified();
    title.remove(QRegularExpression(QStringLiteral("<[^>]+>")));
    title.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    title.remove(QRegularExpression(QStringLiteral("\\s*\\((?:P\\d+(?:\\.\\d+)*[-\\w]*|[A-Z]{2,})\\)\\s*$")));
    title.replace(QRegularExpression(QStringLiteral("\\bamyotrophic lateral sclerosis\\s*\\(\\s*ALS\\s*\\)"), QRegularExpression::CaseInsensitiveOption),
                  QStringLiteral("amyotrophic lateral sclerosis"));
    title.remove(QRegularExpression(QStringLiteral("\\s*\\((?:ALS|MND)\\)"), QRegularExpression::CaseInsensitiveOption));
    title.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return title.simplified();
}

static QString thesisLineForPaper(QString title, const QString &topic = QString())
{
    title = cleanPaperTitleForThesis(title);
    const QString lower = title.toCaseFolded();
    if (title.isEmpty()) {
        return topic;
    }
    if (lower.contains(QLatin1String("clinical factors")) && lower.contains(QLatin1String("cognitive error")) && lower.contains(QLatin1String("misdiagnos"))) {
        return QStringLiteral("Clinical factors that may drive cognitive error in ALS misdiagnosis");
    }
    if (lower.contains(QLatin1String("diagnostic criteria")) && lower.contains(QLatin1String("amyotrophic lateral sclerosis"))) {
        return QStringLiteral("How ALS diagnostic criteria perform in clinical cohorts");
    }
    if (lower.contains(QLatin1String("misdiagnos")) && (lower.contains(QLatin1String("als")) || lower.contains(QLatin1String("amyotrophic")))) {
        return QStringLiteral("Why ALS can be misdiagnosed and which mimics matter");
    }
    if (lower.contains(QLatin1String("neurofilament"))) {
        return QStringLiteral("Neurofilament evidence for ALS diagnosis, staging, or prognosis");
    }
    if (lower.contains(QLatin1String("cognitive")) && (lower.contains(QLatin1String("als")) || lower.contains(QLatin1String("amyotrophic")))) {
        return QStringLiteral("Cognitive / FTD involvement in ALS");
    }
    if (lower.startsWith(QLatin1String("letter re:"))) {
        return QStringLiteral("Letter / correction thread on ") + title.mid(QStringLiteral("letter re:").size()).trimmed();
    }
    if (lower.startsWith(QLatin1String("author response:"))) {
        return QStringLiteral("Author response on ") + title.mid(QStringLiteral("author response:").size()).trimmed();
    }
    if (!topic.isEmpty() && title.size() > 82) {
        return topic + QStringLiteral(": ") + title.left(78).trimmed() + QStringLiteral("…");
    }
    return title;
}

static bool isPaperishEntryData(const QString &format, const QString &title, const QString &description, const QStringList &tags, const QStringList &keywords, const QStringList &detailLines)
{
    if (format != QLatin1String("PDF")) {
        return false;
    }
    const QString haystack = QStringList({title, description, tags.join(QLatin1Char(' ')), keywords.join(QLatin1Char(' ')), detailLines.join(QLatin1Char(' '))})
                                 .join(QLatin1Char(' '))
                                 .toCaseFolded();
    return haystack.contains(QLatin1String("paper")) || haystack.contains(QLatin1String("journal")) || haystack.contains(QLatin1String("doi"))
        || haystack.contains(QRegularExpression(QStringLiteral("\\b10\\.\\d{4,}/")));
}

static QString paperMetadataLineFromDescription(const QString &description, const QStringList &detailLines)
{
    for (const QString &line : detailLines) {
        if (!yearFromMetadataLine(line).isEmpty()) {
            return line.simplified();
        }
    }
    if (!yearFromMetadataLine(description).isEmpty()) {
        return description.simplified();
    }
    return QString();
}

static QString citationLabelForEntryData(const QString &description, const QStringList &detailLines)
{
    const QString metadata = paperMetadataLineFromDescription(description, detailLines);
    const QStringList parts = metadata.split(QStringLiteral(" · "), Qt::SkipEmptyParts);
    QString authors = parts.value(0).trimmed();
    QString year;
    for (const QString &part : parts) {
        year = yearFromMetadataLine(part);
        if (!year.isEmpty()) {
            break;
        }
    }
    if (authors.isEmpty()) {
        authors = description;
    }
    return citationLabelForPaper(authors, year);
}

static bool isDisplayOnlyGenericTag(const QString &tag)
{
    const QString key = compactPublicationTypeKey(tag);
    return key == QLatin1String("book") || key == QLatin1String("books") || key == QLatin1String("epub") || key == QLatin1String("ebook")
        || key == QLatin1String("ebooks") || key == QLatin1String("document") || key == QLatin1String("documents") || key == QLatin1String("other")
        || key == QLatin1String("misc") || key == QLatin1String("miscellaneous") || key == QLatin1String("unknown") || key == QLatin1String("unidentified")
        || key == QLatin1String("uncategorized") || key == QLatin1String("untagged");
}

static QString curatedLocalTitleFor(const QString &text)
{
    const QString lower = text.toCaseFolded();
    const QString simplified = lower.simplified();
    const bool bookish = lower.contains(QLatin1String("book:")) || lower.contains(QLatin1String("aa_book")) || lower.contains(QLatin1String("(book)"))
        || lower.contains(QLatin1String("anna")) || lower.contains(QLatin1String("imported-books")) || lower.contains(QLatin1String(".epub"))
        || lower.contains(QLatin1String(".azw3")) || lower.contains(QLatin1String(".mobi")) || lower.contains(QLatin1String("bantam books"))
        || lower.contains(QLatin1String("penguin")) || lower.contains(QLatin1String("melville")) || lower.contains(QLatin1String("farrar"))
        || lower.contains(QLatin1String("mit press")) || lower.contains(QLatin1String("pm press"));
    if (lower.contains(QLatin1String("aldo leopold")) && lower.contains(QLatin1String("sand county almanac"))) {
        return QStringLiteral("A Sand County Almanac & Other Writings on Ecology and Conservation");
    }
    if (lower.contains(QLatin1String("medicine for mountaineer")) || lower.contains(QLatin1String("mountaineering & other wilderness activities"))) {
        return QStringLiteral("Medicine for Mountaineering & Other Wilderness Activities");
    }
    if (lower.contains(QLatin1String("ref13 graeber 2011"))) {
        return QStringLiteral("Debt: The First 5,000 Years");
    }
    if (lower.contains(QLatin1String("parable of the sower")) || (lower.contains(QLatin1String("octavia butler")) && lower.contains(QLatin1String("parable")))) {
        return QStringLiteral("Parable of the Sower");
    }
    if (lower.contains(QLatin1String("red mars")) && (bookish || lower.contains(QLatin1String("kim stanley robinson")) || simplified.startsWith(QLatin1String("red mars")))) {
        return QStringLiteral("Red Mars");
    }
    if (lower.contains(QLatin1String("new york 2140")) && (bookish || lower.contains(QLatin1String("kim stanley robinson")) || simplified.startsWith(QLatin1String("new york 2140")))) {
        return QStringLiteral("New York 2140");
    }
    if (lower.contains(QLatin1String("antarctica")) && (lower.contains(QLatin1String("novel")) || lower.contains(QLatin1String("kim stanley robinson")))) {
        return QStringLiteral("Antarctica");
    }
    if (lower.contains(QLatin1String("game of thrones")) && (bookish || simplified.startsWith(QLatin1String("a game of thrones")))) {
        return QStringLiteral("A Game of Thrones");
    }
    if ((lower.contains(QLatin1String("everything was forever until it was no more")) || lower.contains(QLatin1String("everything was forever, until it was no more")))
        && (bookish || simplified.startsWith(QLatin1String("everything was forever")))) {
        return QStringLiteral("Everything Was Forever Until It Was No More");
    }
    if (lower.contains(QLatin1String("dawn of everything")) && (bookish || simplified.startsWith(QLatin1String("the dawn of everything")))) {
        return QStringLiteral("The Dawn of Everything");
    }
    if (lower.contains(QLatin1String("bullshit jobs")) && (bookish || simplified.startsWith(QLatin1String("bullshit jobs")))) {
        return QStringLiteral("Bullshit Jobs");
    }
    if ((lower.contains(QLatin1String("debt the first 5000 years")) || lower.contains(QLatin1String("debt the first 5,000 years")) || simplified.startsWith(QLatin1String("debt graeber")))
        && bookish) {
        return QStringLiteral("Debt: The First 5,000 Years");
    }
    if (lower.contains(QLatin1String("cities made differently")) && (bookish || simplified.startsWith(QLatin1String("cities made differently")))) {
        return QStringLiteral("Cities Made Differently");
    }
    if (lower.contains(QLatin1String("utopia of rules")) && (bookish || simplified.startsWith(QLatin1String("the utopia of rules")))) {
        return QStringLiteral("The Utopia of Rules");
    }
    if (lower.contains(QLatin1String("pirate enlightenment")) && (bookish || simplified.startsWith(QLatin1String("pirate enlightenment")))) {
        return QStringLiteral("Pirate Enlightenment");
    }
    if (lower.contains(QLatin1String("path to power")) && (bookish || simplified.startsWith(QLatin1String("the path to power")))) {
        return QStringLiteral("The Path to Power");
    }
    if (lower.contains(QLatin1String("means of ascent")) && (bookish || simplified.startsWith(QLatin1String("means of ascent")) || simplified.startsWith(QLatin1String("the years of lyndon johnson means of ascent")))) {
        return QStringLiteral("Means of Ascent");
    }
    if (lower.contains(QLatin1String("master of the senate")) && (bookish || simplified.startsWith(QLatin1String("master of the senate")))) {
        return QStringLiteral("Master of the Senate");
    }
    if (lower.contains(QLatin1String("passage of power")) && (bookish || simplified.startsWith(QLatin1String("the passage of power")) || simplified.startsWith(QLatin1String("the years of lyndon johnson 04")))) {
        return QStringLiteral("The Passage of Power");
    }
    if (lower.contains(QLatin1String("power broker")) && (bookish || simplified.startsWith(QLatin1String("the power broker")))) {
        return QStringLiteral("The Power Broker");
    }
    if (lower.contains(QLatin1String("working researching interviewing writing")) && (bookish || simplified.startsWith(QLatin1String("working")))) {
        return QStringLiteral("Working: Researching, Interviewing, Writing");
    }
    if (lower.contains(QLatin1String("why work")) && lower.contains(QLatin1String("leisure society")) && (bookish || simplified.startsWith(QLatin1String("why work")))) {
        return QStringLiteral("Why Work?");
    }
    if (lower.contains(QLatin1String("on kings")) && lower.contains(QLatin1String("graeber")) && (bookish || simplified.startsWith(QLatin1String("on kings")))) {
        return QStringLiteral("On Kings");
    }
    return QString();
}

static QString cleanedLocalTitle(QString title, const QUrl &url)
{
    const QString pathBase = QFileInfo(url.isLocalFile() ? url.toLocalFile() : url.fileName()).completeBaseName();
    const QString curated = curatedLocalTitleFor(QStringList({title, pathBase}).join(QLatin1Char(' ')));
    if (!curated.isEmpty()) {
        return curated;
    }

    if (title.trimmed().isEmpty()) {
        title = pathBase;
    }
    title.replace(QLatin1Char('_'), QLatin1Char(' '));
    title.replace(QRegularExpression(QStringLiteral("\\bAnna[’']s Archive\\b"), QRegularExpression::CaseInsensitiveOption), QString());
    title.remove(QRegularExpression(QStringLiteral("\\b[0-9a-fA-F]{32}\\b")));
    title.remove(QRegularExpression(QStringLiteral("\\b97[89][0-9]{10}\\b")));
    title.remove(QRegularExpression(QStringLiteral("\\s+[0-9a-fA-F]{8}$")));
    title.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    title = title.simplified();

    const QString curatedAfterCleanup = curatedLocalTitleFor(title);
    return curatedAfterCleanup.isEmpty() ? title : curatedAfterCleanup;
}

static QString cleanedFilenameTitle(const QUrl &url)
{
    return cleanedLocalTitle(QString(), url);
}

static bool titleNeedsGeneratedMetadata(const QString &title, const QUrl &url)
{
    const QString simplified = title.simplified();
    if (simplified.isEmpty()) {
        return true;
    }
    const QString pathBase = QFileInfo(url.isLocalFile() ? url.toLocalFile() : url.fileName()).completeBaseName().simplified();
    if (!pathBase.isEmpty() && simplified.compare(pathBase, Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (simplified.contains(QRegularExpression(QStringLiteral("\\b[0-9a-fA-F]{24,}\\b")))) {
        return true;
    }
    if (simplified.contains(QRegularExpression(QStringLiteral("\\b10[-.][0-9]{4,}[-._/A-Za-z0-9]+"), QRegularExpression::CaseInsensitiveOption))) {
        return true;
    }
    const QStringList words = simplified.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    return simplified.size() <= 8 || words.size() <= 2;
}

static CoverGenerator::CoverSpec corpusCoverSpecForIndex(const QModelIndex &index)
{
    CoverGenerator::CoverSpec spec;
    spec.title = index.data(Qt::DisplayRole).toString();
    const QString intent = index.data(PaperLibrarySectionedModel::ShelfIntentRole).toString();
    const QString relation = index.data(PaperLibrarySectionedModel::RelationHintRole).toString();
    spec.authors = intent.isEmpty() ? index.data(PaperLibraryModel::AuthorsRole).toString() : intent;
    spec.yearJournal = relation.isEmpty() ? joinCompact({index.data(PaperLibraryModel::YearRole).toString(), index.data(PaperLibraryModel::JournalRole).toString()}) : relation;
    const QStringList tags = index.data(PaperLibrarySectionedModel::TopicTagsRole).toStringList();
    spec.tag = tags.value(0, intent.isEmpty() ? index.data(PaperLibrarySectionedModel::ThumbnailSeedRole).toString() : intent);
    return spec;
}

/** Word-wraps @p text into at most @p maxLines lines of @p width, eliding the last. */
static QStringList wrapTitle(const QString &text, const QFont &font, int width, int maxLines)
{
    QTextLayout layout(text, font);
    QTextOption wrapOption;
    wrapOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(wrapOption);

    QStringList lines;
    layout.beginLayout();
    for (int i = 0; i < maxLines; ++i) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(width);
        const bool lastAllowed = i == maxLines - 1;
        const bool moreToCome = line.textStart() + line.textLength() < text.size();
        if (lastAllowed && moreToCome) {
            lines.append(QFontMetrics(font).elidedText(text.mid(line.textStart()).trimmed(), Qt::ElideRight, width));
        } else {
            lines.append(text.mid(line.textStart(), line.textLength()).trimmed());
        }
    }
    layout.endLayout();
    return lines;
}

static bool isLibraryHeaderIndex(const QModelIndex &index)
{
    if (!index.isValid()) {
        return false;
    }
    if (index.data(PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
        return true;
    }
    if (index.data(PaperLibrarySectionedModel::SourceRowRole).isValid()) {
        return false;
    }
    return index.data(LibraryView::HeaderRole).toBool();
}

static QPainterPath starPath(const QPointF &center, qreal outerRadius)
{
    QPainterPath path;
    const qreal innerRadius = outerRadius * 0.45;
    for (int i = 0; i < 10; ++i) {
        const qreal radius = (i % 2 == 0) ? outerRadius : innerRadius;
        const qreal angle = -M_PI / 2 + i * M_PI / 5;
        const QPointF point(center.x() + radius * std::cos(angle), center.y() + radius * std::sin(angle));
        if (i == 0) {
            path.moveTo(point);
        } else {
            path.lineTo(point);
        }
    }
    path.closeSubpath();
    return path;
}

static bool corpusPaperKeyContainsAny(const QString &key, const QStringList &needles)
{
    for (const QString &needle : needles) {
        if (key.contains(needle)) {
            return true;
        }
    }
    return false;
}

static QString corpusPaperKeyForIndex(const QModelIndex &index)
{
    return QStringList({index.data(Qt::DisplayRole).toString(),
                        index.data(PaperLibraryModel::DetailRole).toString(),
                        index.data(PaperLibraryModel::AuthorsRole).toString(),
                        index.data(PaperLibraryModel::YearRole).toString(),
                        index.data(PaperLibraryModel::JournalRole).toString(),
                        index.data(PaperLibraryModel::DoiRole).toString(),
                        index.data(PaperLibraryModel::SourceRole).toString()})
        .join(QLatin1Char(' '))
        .toCaseFolded();
}

static QString corpusPaperTopicLabelForKey(const QString &key, const QString &fallback = QString())
{
    if (corpusPaperKeyContainsAny(key,
                                  {QStringLiteral("diagnostic delay"),
                                   QStringLiteral("delay"),
                                   QStringLiteral("racial"),
                                   QStringLiteral("disparit"),
                                   QStringLiteral("health services"),
                                   QStringLiteral("utilization"),
                                   QStringLiteral("utilisation"),
                                   QStringLiteral("referral"),
                                   QStringLiteral("neurologist"),
                                   QStringLiteral("diagnostician")})) {
        return QStringLiteral("Access / delay");
    }
    if (corpusPaperKeyContainsAny(key,
                                  {QStringLiteral("neurofilament"),
                                   QStringLiteral(" nfl "),
                                   QStringLiteral("nf-l"),
                                   QStringLiteral("light chain"),
                                   QStringLiteral("csf"),
                                   QStringLiteral("serum"),
                                   QStringLiteral("plasma"),
                                   QStringLiteral("biomarker"),
                                   QStringLiteral("chitinase"),
                                   QStringLiteral("chi3l1"),
                                   QStringLiteral("sensitivity"),
                                   QStringLiteral("specificity")})) {
        return QStringLiteral("Fluid marker");
    }
    if (corpusPaperKeyContainsAny(key,
                                  {QStringLiteral("threshold tracking"),
                                   QStringLiteral("nerve conduction"),
                                   QStringLiteral("split-hand"),
                                   QStringLiteral("electrodiagnos"),
                                   QStringLiteral("electromyography"),
                                   QStringLiteral(" emg "),
                                   QStringLiteral("transcranial magnetic stimulation"),
                                   QStringLiteral(" tms "),
                                   QStringLiteral("cortical hyperexcitability"),
                                   QStringLiteral("hyperexcitability"),
                                   QStringLiteral("beta-band"),
                                   QStringLiteral("intermuscular")})) {
        return QStringLiteral("Electrophysiology");
    }
    if (corpusPaperKeyContainsAny(key,
                                  {QStringLiteral("mri"),
                                   QStringLiteral("magnetic resonance"),
                                   QStringLiteral("imaging"),
                                   QStringLiteral("sonographic"),
                                   QStringLiteral("ultrasound"),
                                   QStringLiteral("structural brain"),
                                   QStringLiteral("network"),
                                   QStringLiteral("connectivity")})) {
        return QStringLiteral("Imaging / network");
    }
    if (corpusPaperKeyContainsAny(key,
                                  {QStringLiteral("differential"),
                                   QStringLiteral("mimic"),
                                   QStringLiteral("misdiagnos"),
                                   QStringLiteral("false positive"),
                                   QStringLiteral("als-plus"),
                                   QStringLiteral("spastic paraplegia"),
                                   QStringLiteral("porphyria"),
                                   QStringLiteral("leukoencephalopathy")})) {
        return QStringLiteral("Mimic boundary");
    }
    if (corpusPaperKeyContainsAny(key,
                                  {QStringLiteral("awaji"),
                                   QStringLiteral("el escorial"),
                                   QStringLiteral("gold coast"),
                                   QStringLiteral("criteria"),
                                   QStringLiteral("criterion"),
                                   QStringLiteral("diagnosis pathway"),
                                   QStringLiteral("diagnostic pathway")})) {
        return QStringLiteral("Criteria / pathway");
    }
    if (corpusPaperKeyContainsAny(key, {QStringLiteral("prodromal"), QStringLiteral("preclinical"), QStringLiteral("onset"), QStringLiteral("early symptom")})) {
        return QStringLiteral("Prodrome");
    }
    if (corpusPaperKeyContainsAny(key,
                                  {QStringLiteral("survival"),
                                   QStringLiteral("prognosis"),
                                   QStringLiteral("natural history"),
                                   QStringLiteral("phenotype"),
                                   QStringLiteral("cohort"),
                                   QStringLiteral("risk factor"),
                                   QStringLiteral("incidence"),
                                   QStringLiteral("prevalence")})) {
        return QStringLiteral("Trajectory");
    }
    if (corpusPaperKeyContainsAny(key,
                                  {QStringLiteral("nutrition"),
                                   QStringLiteral("malnutrition"),
                                   QStringLiteral("metabolic"),
                                   QStringLiteral("bioenergetic"),
                                   QStringLiteral("energy metabolism"),
                                   QStringLiteral("mitochond"),
                                   QStringLiteral("sirt3"),
                                   QStringLiteral("hypothalamic")})) {
        return QStringLiteral("Metabolism");
    }
    if (corpusPaperKeyContainsAny(key,
                                  {QStringLiteral("sod1"),
                                   QStringLiteral("c9orf72"),
                                   QStringLiteral("tdp-43"),
                                   QStringLiteral("tdp43"),
                                   QStringLiteral("genetic"),
                                   QStringLiteral("mutation"),
                                   QStringLiteral("familial")})) {
        return QStringLiteral("Genetics / molecular");
    }
    if (corpusPaperKeyContainsAny(key,
                                  {QStringLiteral("treatment"),
                                   QStringLiteral("trial"),
                                   QStringLiteral("therapy"),
                                   QStringLiteral("riluzole"),
                                   QStringLiteral("edaravone"),
                                   QStringLiteral("tofersen"),
                                   QStringLiteral("antisense")})) {
        return QStringLiteral("Treatment signal");
    }
    if (corpusPaperKeyContainsAny(key,
                                  {QStringLiteral("cognitive"),
                                   QStringLiteral("frontotemporal"),
                                   QStringLiteral(" ftd "),
                                   QStringLiteral("executive"),
                                   QStringLiteral("behaviour"),
                                   QStringLiteral("behavior")})) {
        return QStringLiteral("Cognition / FTD");
    }
    if (corpusPaperKeyContainsAny(key,
                                  {QStringLiteral("care"),
                                   QStringLiteral("management"),
                                   QStringLiteral("respiratory"),
                                   QStringLiteral("feeding"),
                                   QStringLiteral("end-of-life"),
                                   QStringLiteral("clinical pathway")})) {
        return QStringLiteral("Care pathway");
    }
    if (corpusPaperKeyContainsAny(key, {QStringLiteral("mnd"), QStringLiteral("als"), QStringLiteral("motor neuron"), QStringLiteral("amyotrophic")})) {
        return fallback.isEmpty() ? QStringLiteral("ALS / MND frame") : fallback;
    }
    return fallback;
}

static QString corpusPaperSummaryForKey(const QString &key)
{
    const QString label = corpusPaperTopicLabelForKey(key);
    if (label == QLatin1String("Access / delay")) {
        return QStringLiteral("Where diagnosis gets delayed");
    }
    if (label == QLatin1String("Fluid marker")) {
        return QStringLiteral("Candidate marker for diagnosis or staging");
    }
    if (label == QLatin1String("Electrophysiology")) {
        return QStringLiteral("Signal from excitability / nerve testing");
    }
    if (label == QLatin1String("Imaging / network")) {
        return QStringLiteral("Structural or network evidence");
    }
    if (label == QLatin1String("Mimic boundary")) {
        return QStringLiteral("Separates ALS from mimics");
    }
    if (label == QLatin1String("Criteria / pathway")) {
        return QStringLiteral("How diagnostic criteria perform");
    }
    if (label == QLatin1String("Prodrome")) {
        return QStringLiteral("Possible pre-diagnostic window");
    }
    if (label == QLatin1String("Trajectory")) {
        return QStringLiteral("Natural history / prognosis context");
    }
    if (label == QLatin1String("Metabolism")) {
        return QStringLiteral("Metabolic mechanism or vulnerability");
    }
    if (label == QLatin1String("Genetics / molecular")) {
        return QStringLiteral("Molecular subtype or disease mechanism");
    }
    if (label == QLatin1String("Treatment signal")) {
        return QStringLiteral("Treatment evidence or trial context");
    }
    if (label == QLatin1String("Cognition / FTD")) {
        return QStringLiteral("ALS cognitive / FTD overlap");
    }
    if (label == QLatin1String("Care pathway")) {
        return QStringLiteral("Clinical management and services");
    }
    if (label == QLatin1String("ALS / MND frame")) {
        return QStringLiteral("General MND project context");
    }
    return QString();
}

static QString corpusPaperMetadataLineForIndex(const QModelIndex &index)
{
    const QString authors = index.data(PaperLibraryModel::AuthorsRole).toString();
    const QString year = index.data(PaperLibraryModel::YearRole).toString();
    const QString journal = index.data(PaperLibraryModel::JournalRole).toString();
    QString firstAuthor = authors.section(QLatin1Char(';'), 0, 0).trimmed();
    if (firstAuthor.isEmpty()) {
        firstAuthor = authors.section(QLatin1Char(','), 0, 0).trimmed();
    }
    QStringList parts;
    if (!firstAuthor.isEmpty()) {
        parts.append(firstAuthor);
    }
    if (!year.isEmpty() && year != QLatin1String("None")) {
        parts.append(year);
    }
    if (!journal.isEmpty() && journal != QLatin1String("(book)") && journal != QLatin1String("None")) {
        parts.append(journal);
    }
    return parts.join(QStringLiteral(" · "));
}

static QString corpusPaperCitationLabelForIndex(const QModelIndex &index)
{
    return citationLabelForPaper(index.data(PaperLibraryModel::AuthorsRole).toString(), index.data(PaperLibraryModel::YearRole).toString());
}

static QString corpusPaperThesisForIndex(const QModelIndex &index)
{
    const QString key = corpusPaperKeyForIndex(index);
    return thesisLineForPaper(index.data(Qt::DisplayRole).toString(), corpusPaperSummaryForKey(key));
}

/**
 * Paints library tiles: a cover (thumbnail or placeholder) with rounded
 * corners over a soft ambient shadow, the title (up to two lines) with a
 * muted tag row below, a pin badge when pinned and an Apple Books progress
 * bar when known. Space for the second title line and the tag row is always
 * reserved so tile heights stay uniform across a shelf.
 */
class LibraryTileDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (isHeader(index)) {
            // A near-viewport-wide item forces the following tile onto a new row
            const QListView *view = qobject_cast<const QListView *>(option.widget);
            const int width = view ? qMax(CorpusTileWidth, view->viewport()->width() - 2 * GridSpacing - 8) : 3 * CorpusTileWidth;
            return QSize(width, option.fontMetrics.height() + 14);
        }

        const bool corpusTile = isCorpusTile(index);
        const int coverHeight = corpusTile ? CorpusCoverHeight : CoverHeight;
        const int tileWidth = corpusTile ? CorpusTileWidth : TileWidth;
        int height = TilePadding + coverHeight;
        if (reservesProgressRow(index)) {
            height += ProgressGap + QFontMetrics(smallerFont(option.font)).height();
        }
        height += TitleGap + TitleLines * option.fontMetrics.height();
        height += TagGap + QFontMetrics(smallerFont(option.font)).height(); // tag row, reserved even when untagged
        height += TilePadding;
        return QSize(tileWidth, height);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
        QStyleOptionViewItem option = opt;
        initStyleOption(&option, index);
        const QPalette &palette = option.palette;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

        if (isHeader(index)) {
            QFont headerFont = option.font;
            headerFont.setBold(true);
            headerFont.setPointSizeF(option.font.pointSizeF() * 0.9);
            QColor headerColor = palette.color(QPalette::Text);
            headerColor.setAlphaF(0.55);
            painter->setFont(headerFont);
            painter->setPen(headerColor);
            painter->drawText(option.rect.adjusted(4, 0, -4, -3), Qt::AlignLeft | Qt::AlignBottom, index.data(Qt::DisplayRole).toString());
            painter->restore();
            return;
        }

        // Hover/selection: a subtle rounded highlight behind the whole tile
        if (option.state & (QStyle::State_MouseOver | QStyle::State_Selected)) {
            QColor highlight = palette.color(QPalette::Text);
            highlight.setAlphaF((option.state & QStyle::State_Selected) ? 0.12 : 0.06);
            painter->setPen(Qt::NoPen);
            painter->setBrush(highlight);
            painter->drawRoundedRect(option.rect, 8, 8);
        }

        const bool corpusTile = isCorpusTile(index);
        const int coverHeight = corpusTile ? CorpusCoverHeight : CoverHeight;
        const int coverWidth = corpusTile ? CorpusCoverWidth : CoverWidth;
        const QRect coverBox(option.rect.left() + TilePadding, option.rect.top() + TilePadding, coverWidth, coverHeight);

        // Real covers keep their aspect ratio, sitting on the box's bottom
        // edge like books on a shelf; placeholders fill the box
        QPixmap cover = corpusTile ? index.data(PaperLibrarySectionedModel::CoverPixmapRole).value<QPixmap>() : index.data(LibraryView::CoverRole).value<QPixmap>();
        if (!corpusTile && cover.isNull()) {
            cover = index.data(PaperLibrarySectionedModel::CoverPixmapRole).value<QPixmap>();
        }
        const bool generatedCorpusFallback = corpusTile && !cover.isNull() && index.data(PaperLibrarySectionedModel::GeneratedCoverRole).toBool();
        if (generatedCorpusFallback) {
            // Generated corpus covers are only cache/warmup artifacts. The
            // delegate paints a richer, shelf-aware paper card immediately,
            // and only a real rendered cover should replace that semantic
            // visual thumbnail.
            cover = QPixmap();
        }
        QRect coverRect;
        if (!cover.isNull()) {
            const QSize scaled = cover.size().scaled(coverBox.size(), Qt::KeepAspectRatio);
            coverRect = QRect(QPoint(coverBox.center().x() - scaled.width() / 2 + 1, coverBox.bottom() - scaled.height() + 1), scaled);
        } else {
            coverRect = coverBox.adjusted(8, 0, -8, 0);
        }

        drawCoverShadow(painter, coverRect);

        QPainterPath coverClip;
        coverClip.addRoundedRect(coverRect, CoverRadius, CoverRadius);
        if (!cover.isNull()) {
            painter->save();
            painter->setClipPath(coverClip);
            painter->drawPixmap(coverRect, cover);
            painter->restore();
        } else if (corpusTile) {
            drawCorpusCard(painter, coverRect, index, option, palette);
        } else {
            drawDocumentCard(painter, coverRect, index, option, palette);
        }

        // Translucent outline so white covers keep a soft edge (the page
        // view draws the same kind of rim around pages)
        painter->setPen(QColor(0, 0, 0, 40));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(QRectF(coverRect).adjusted(0.5, 0.5, -0.5, -0.5), CoverRadius, CoverRadius);

        const bool pinned = corpusTile ? index.data(PaperLibraryModel::PinnedRole).toBool() : index.data(LibraryView::PinnedRole).toBool();
        const bool downranked = corpusTile ? index.data(PaperLibrarySectionedModel::DownrankedRole).toBool() : index.data(LibraryView::DownrankedRole).toBool();
        if (pinned) {
            const QPointF badgeCenter(coverRect.right() - 12, coverRect.top() + 13);
            painter->setPen(Qt::NoPen);
            painter->setBrush(palette.color(QPalette::Highlight));
            painter->drawEllipse(badgeCenter, 9, 9);
            painter->setBrush(palette.color(QPalette::HighlightedText));
            painter->drawPath(starPath(badgeCenter, 5.5));
        }
        if (downranked) {
            const QPointF badgeCenter(coverRect.right() - 12, coverRect.top() + 13);
            QColor badge = palette.color(QPalette::Text);
            badge.setAlphaF(0.45);
            painter->setPen(Qt::NoPen);
            painter->setBrush(badge);
            painter->drawEllipse(badgeCenter, 9, 9);
            QPainterPath arrow;
            arrow.moveTo(badgeCenter.x() - 5, badgeCenter.y() - 2);
            arrow.lineTo(badgeCenter.x(), badgeCenter.y() + 4);
            arrow.lineTo(badgeCenter.x() + 5, badgeCenter.y() - 2);
            painter->setPen(QPen(palette.color(QPalette::Base), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->drawPath(arrow);
        }
        const QVariant relatedValue = corpusTile ? index.data(PaperLibraryModel::RelatedCountRole) : QVariant();
        if (relatedValue.isValid() && relatedValue.toInt() >= 0) {
            drawConnectionBadge(painter, coverRect, relatedValue.toInt(), option, palette);
        }

        int titleTop = coverBox.bottom() + 1;
        if (reservesProgressRow(index)) {
            const QFont progressFont = smallerFont(option.font);
            const QFontMetrics progressMetrics(progressFont);
            const double progress = index.data(LibraryView::ProgressRole).toDouble();
            if (progress >= 0.0) {
                const QString label = i18nc("Apple Books reading progress on a library tile", "%1%", qRound(progress * 100));
                const int labelWidth = progressMetrics.horizontalAdvance(label);
                const int rowTop = titleTop + ProgressGap;
                const QRect barRect(coverBox.left(), rowTop + (progressMetrics.height() - ProgressBarHeight) / 2, coverBox.width() - labelWidth - 6, ProgressBarHeight);

                QColor track = palette.color(QPalette::Text);
                track.setAlphaF(0.15);
                painter->setPen(Qt::NoPen);
                painter->setBrush(track);
                painter->drawRoundedRect(barRect, 2, 2);

                QRect fillRect = barRect;
                fillRect.setWidth(qBound(ProgressBarHeight, qRound(barRect.width() * progress), barRect.width()));
                painter->setBrush(palette.color(QPalette::Highlight));
                painter->drawRoundedRect(fillRect, 2, 2);

                QColor labelColor = palette.color(QPalette::Text);
                labelColor.setAlphaF(0.6);
                painter->setFont(progressFont);
                painter->setPen(labelColor);
                painter->drawText(QRect(barRect.right() + 6, rowTop, labelWidth + 2, progressMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter, label);
            }
            titleTop += ProgressGap + progressMetrics.height();
        }

        const int textLeft = option.rect.left() + 6;
        const int textWidth = option.rect.width() - 12;

        const LibraryView::TileCaption caption = LibraryView::tileCaption(index);
        if (caption.secondary) {
            // A generated card already displays the title as its artwork;
            // the caption slot carries the description or tag line instead,
            // muted, up to two lines (same slot, so heights stay uniform)
            if (!caption.text.isEmpty()) {
                const QFont captionFont = smallerFont(option.font);
                const QFontMetrics captionMetrics(captionFont);
                QColor captionColor = palette.color(QPalette::Text);
                captionColor.setAlphaF(0.6);
                painter->setFont(captionFont);
                painter->setPen(captionColor);
                const QStringList captionLines = wrapTitle(caption.text, captionFont, textWidth, TitleLines);
                int lineTop = titleTop + TitleGap;
                for (const QString &line : captionLines) {
                    painter->drawText(QRect(textLeft, lineTop, textWidth, captionMetrics.height()), Qt::AlignHCenter | Qt::AlignTop, line);
                    lineTop += captionMetrics.height();
                }
            }
        } else {
            painter->setFont(option.font);
            painter->setPen(palette.color(QPalette::Text));
            const QStringList titleLines = wrapTitle(caption.text, option.font, textWidth, TitleLines);
            int lineTop = titleTop + TitleGap;
            for (const QString &line : titleLines) {
                painter->drawText(QRect(textLeft, lineTop, textWidth, option.fontMetrics.height()), Qt::AlignHCenter | Qt::AlignTop, line);
                lineTop += option.fontMetrics.height();
            }

            // Muted tag row right under the title (its space is reserved in
            // sizeHint either way, so untagged tiles stay the same height)
            QStringList displayTags;
            QStringList shownTags;
            if (corpusTile) {
                shownTags = index.data(PaperLibrarySectionedModel::TopicTagsRole).toStringList();
            } else {
                displayTags = index.data(LibraryView::DisplayTagsRole).toStringList();
                shownTags = displayTags.isEmpty() ? index.data(LibraryView::TagsRole).toStringList() : displayTags;
            }
            if (!shownTags.isEmpty()) {
                const QFont tagFont = smallerFont(option.font);
                const QFontMetrics tagMetrics(tagFont);
                QColor tagColor = palette.color(QPalette::Text);
                tagColor.setAlphaF(0.55);
                painter->setFont(tagFont);
                painter->setPen(tagColor);
                const QStringList tagLines = wrapTitle(QStringList(shownTags.mid(0, 2)).join(QStringLiteral(" · ")), tagFont, textWidth, MetadataLines);
                int tagTop = lineTop + TagGap;
                for (const QString &line : tagLines) {
                    painter->drawText(QRect(textLeft, tagTop, textWidth, tagMetrics.height()), Qt::AlignHCenter | Qt::AlignTop, line);
                    tagTop += tagMetrics.height();
                }
            }
        }

        painter->restore();
    }

private:
    static bool isHeader(const QModelIndex &index)
    {
        return isLibraryHeaderIndex(index);
    }

    static bool isCorpusTile(const QModelIndex &index)
    {
        return index.data(PaperLibrarySectionedModel::SourceRowRole).isValid();
    }

    static bool reservesProgressRow(const QModelIndex &index)
    {
        // The Books shelf reserves the progress row on every tile so titles
        // line up whether or not Apple Books knows the document
        return index.model() && index.model()->property("booksShelf").toBool();
    }

    static bool keyContainsAny(const QString &key, const QStringList &needles)
    {
        for (const QString &needle : needles) {
            if (key.contains(needle)) {
                return true;
            }
        }
        return false;
    }

    static void drawConnectionBadge(QPainter *painter, const QRect &coverRect, int relatedCount, const QStyleOptionViewItem &option, const QPalette &palette)
    {
        const QString label = relatedCount > 99 ? QStringLiteral("99+") : QString::number(relatedCount);
        QFont badgeFont = smallerFont(option.font);
        badgeFont.setBold(true);
        const QFontMetrics metrics(badgeFont);
        const int width = qMax(34, metrics.horizontalAdvance(label) + 26);
        const QRectF badgeRect(coverRect.right() - width - 7, coverRect.bottom() - 25, width, 18);

        QColor fill = relatedCount > 0 ? palette.color(QPalette::Highlight) : palette.color(QPalette::Text);
        fill.setAlphaF(relatedCount > 0 ? 0.82 : 0.24);
        QColor stroke = relatedCount > 0 ? palette.color(QPalette::HighlightedText) : palette.color(QPalette::Base);
        stroke.setAlphaF(relatedCount > 0 ? 0.92 : 0.86);

        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRoundedRect(badgeRect, 8, 8);

        const QPointF a(badgeRect.left() + 9, badgeRect.center().y() + 2);
        const QPointF b(badgeRect.left() + 15, badgeRect.center().y() - 4);
        const QPointF c(badgeRect.left() + 21, badgeRect.center().y() + 3);
        painter->setPen(QPen(stroke, 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawLine(a, b);
        painter->drawLine(b, c);
        painter->setBrush(stroke);
        painter->drawEllipse(a, 2.0, 2.0);
        painter->drawEllipse(b, 2.0, 2.0);
        painter->drawEllipse(c, 2.0, 2.0);

        painter->setFont(badgeFont);
        painter->setPen(stroke);
        painter->drawText(badgeRect.adjusted(24, 0, -5, 0), Qt::AlignVCenter | Qt::AlignRight, label);
        painter->restore();
    }

    static QString visualKeyForCorpusCard(const QModelIndex &index)
    {
        return corpusPaperKeyForIndex(index);
    }

    static QString corpusCardTitleForIndex(const QModelIndex &index, const QString &citationTitle, const PaperLibrarySectionedModel *sections)
    {
        const QString fullTitle = index.data(Qt::DisplayRole).toString().trimmed();
        if (fullTitle.isEmpty()) {
            return citationTitle;
        }

        const QString key = QStringList({fullTitle,
                                         index.data(PaperLibraryModel::DetailRole).toString(),
                                         index.data(PaperLibrarySectionedModel::ShelfIntentRole).toString(),
                                         index.data(PaperLibrarySectionedModel::RelationHintRole).toString(),
                                         index.data(PaperLibrarySectionedModel::FocusRole).toString(),
                                         index.data(PaperLibraryModel::SourceRole).toString()})
                                .join(QLatin1Char(' '))
                                .toCaseFolded();
        const bool workShelf = sections && sections->smartFilter() == PaperLibrarySectionedModel::Work;
        const bool activeWorkObject = keyContainsAny(key,
                                                     {QStringLiteral("manuscript"),
                                                      QStringLiteral("draft"),
                                                      QStringLiteral("response to reviewer"),
                                                      QStringLiteral("response to reviewers"),
                                                      QStringLiteral("reviewer response"),
                                                      QStringLiteral("revision"),
                                                      QStringLiteral("peer review"),
                                                      QStringLiteral("highdimensional project")});
        if (workShelf || activeWorkObject) {
            return fullTitle;
        }

        if (!citationTitle.isEmpty() && fullTitle.size() > 76) {
            return citationTitle;
        }
        return fullTitle;
    }

    static qreal seededUnit(const QString &seed, uint salt)
    {
        return (qHash(seed, salt) % 1000) / 999.0;
    }

    static void drawCorpusFingerprint(QPainter *painter, const QRectF &area, const QString &seed, QColor accent, const QPalette &palette)
    {
        QColor faint = palette.color(QPalette::Text);
        faint.setAlphaF(0.15);
        accent.setAlphaF(0.48);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(QPointF(area.left(), area.bottom()), QPointF(area.right(), area.bottom()));

        const int barCount = 5;
        const qreal gap = 4.0;
        const qreal barWidth = (area.width() - gap * (barCount - 1)) / barCount;
        for (int i = 0; i < barCount; ++i) {
            const qreal unit = seededUnit(seed, 37u + i * 19u);
            const qreal height = 4.0 + unit * (area.height() - 4.0);
            QRectF bar(area.left() + i * (barWidth + gap), area.bottom() - height, barWidth, height);
            QColor barColor = (i % 2 == 0) ? accent : faint;
            barColor.setAlphaF(i % 2 == 0 ? 0.50 : 0.24);
            painter->setPen(Qt::NoPen);
            painter->setBrush(barColor);
            painter->drawRoundedRect(bar, 1.6, 1.6);
        }
        painter->restore();
    }

    static void drawCorpusMotif(QPainter *painter, const QRectF &area, const QString &key, QColor accent, const QPalette &palette)
    {
        QColor faint = palette.color(QPalette::Text);
        faint.setAlphaF(0.18);
        accent.setAlphaF(0.58);
        QPen pen(accent, 2.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);

        const bool biomarkerLike = keyContainsAny(key,
                                                  {QStringLiteral("biomarker"),
                                                   QStringLiteral("neurofilament"),
                                                   QStringLiteral(" nfl "),
                                                   QStringLiteral("nf-l"),
                                                   QStringLiteral("light chain"),
                                                   QStringLiteral("csf"),
                                                   QStringLiteral("serum"),
                                                   QStringLiteral("plasma"),
                                                   QStringLiteral("chitinase"),
                                                   QStringLiteral("chi3l1"),
                                                   QStringLiteral("sensitivity"),
                                                   QStringLiteral("specificity")});
        const bool accessLike = keyContainsAny(key,
                                               {QStringLiteral("diagnostic delay"),
                                                QStringLiteral("delay"),
                                                QStringLiteral("racial"),
                                                QStringLiteral("disparit"),
                                                QStringLiteral("health services"),
                                                QStringLiteral("utilization"),
                                                QStringLiteral("utilisation"),
                                                QStringLiteral("referral"),
                                                QStringLiteral("neurologist"),
                                                QStringLiteral("diagnostician")});
        const bool electrophysiologyLike = keyContainsAny(key,
                                                          {QStringLiteral("threshold tracking"),
                                                           QStringLiteral("nerve conduction"),
                                                           QStringLiteral("split-hand"),
                                                           QStringLiteral("electrodiagnos"),
                                                           QStringLiteral("electromyography"),
                                                           QStringLiteral(" emg "),
                                                           QStringLiteral("transcranial magnetic stimulation"),
                                                           QStringLiteral(" tms "),
                                                           QStringLiteral("cortical hyperexcitability"),
                                                           QStringLiteral("hyperexcitability"),
                                                           QStringLiteral("beta-band"),
                                                           QStringLiteral("intermuscular")});
        const bool imagingLike = keyContainsAny(key,
                                                {QStringLiteral("mri"),
                                                 QStringLiteral("magnetic resonance"),
                                                 QStringLiteral("imaging"),
                                                 QStringLiteral("sonographic"),
                                                 QStringLiteral("ultrasound"),
                                                 QStringLiteral("structural brain"),
                                                 QStringLiteral("network"),
                                                 QStringLiteral("connectivity")});
        const bool differentialLike = keyContainsAny(key,
                                                     {QStringLiteral("differential"),
                                                      QStringLiteral("mimic"),
                                                      QStringLiteral("misdiagnos"),
                                                      QStringLiteral("false positive"),
                                                      QStringLiteral("als-plus"),
                                                      QStringLiteral("spastic paraplegia"),
                                                      QStringLiteral("porphyria"),
                                                      QStringLiteral("leukoencephalopathy")});
        const bool prodromeLike = keyContainsAny(key, {QStringLiteral("prodromal"), QStringLiteral("preclinical"), QStringLiteral("onset"), QStringLiteral("early symptom")});
        const bool metabolismLike = keyContainsAny(key,
                                                   {QStringLiteral("nutrition"),
                                                    QStringLiteral("malnutrition"),
                                                    QStringLiteral("metabolic"),
                                                    QStringLiteral("bioenergetic"),
                                                    QStringLiteral("energy metabolism"),
                                                    QStringLiteral("mitochond"),
                                                    QStringLiteral("sirt3"),
                                                    QStringLiteral("hypothalamic")});
        const bool molecularLike = keyContainsAny(key,
                                                  {QStringLiteral("sod1"),
                                                   QStringLiteral("c9orf72"),
                                                   QStringLiteral("tdp-43"),
                                                   QStringLiteral("tdp43"),
                                                   QStringLiteral("genetic"),
                                                   QStringLiteral("mutation"),
                                                   QStringLiteral("familial")});
        const bool pathwayLike = keyContainsAny(key,
                                                {QStringLiteral("awaji"),
                                                 QStringLiteral("el escorial"),
                                                 QStringLiteral("gold coast"),
                                                 QStringLiteral("criteria"),
                                                 QStringLiteral("criterion"),
                                                 QStringLiteral("diagnosis pathway"),
                                                 QStringLiteral("diagnostic pathway")});
        const bool trialLike = keyContainsAny(key,
                                              {QStringLiteral("trial"),
                                               QStringLiteral("treatment"),
                                               QStringLiteral("therapy"),
                                               QStringLiteral("riluzole"),
                                               QStringLiteral("survival"),
                                               QStringLiteral("prognosis")});
        const bool methodLike = keyContainsAny(key,
                                               {QStringLiteral("bayes"),
                                                QStringLiteral("model"),
                                                QStringLiteral("statistics"),
                                                QStringLiteral("method"),
                                                QStringLiteral("inference"),
                                                QStringLiteral("prediction")});
        const bool clinicalLike = keyContainsAny(key, {QStringLiteral("medicine"), QStringLiteral("paeds"), QStringLiteral("obgyn"), QStringLiteral("clinical"), QStringLiteral("guideline")});
        const bool neuroLike = keyContainsAny(key,
                                              {QStringLiteral("mnd"),
                                               QStringLiteral("als"),
                                               QStringLiteral("motor neuron"),
                                               QStringLiteral("neuro"),
                                               QStringLiteral("cortical"),
                                               QStringLiteral("axon"),
                                               QStringLiteral("psychiat")});
        const bool responseLike = keyContainsAny(key,
                                                 {QStringLiteral("response to reviewer"),
                                                  QStringLiteral("response to reviewers"),
                                                  QStringLiteral("reviewer response"),
                                                  QStringLiteral("responses to reviewers"),
                                                  QStringLiteral("peer review"),
                                                  QStringLiteral("major revision")});
        const bool manuscriptLike = keyContainsAny(key,
                                                   {QStringLiteral("manuscript"),
                                                    QStringLiteral("draft"),
                                                    QStringLiteral("anonymous manuscript"),
                                                    QStringLiteral("submission draft"),
                                                    QStringLiteral("paper anonymous")});
        const bool highDimensionalLike = keyContainsAny(key,
                                                        {QStringLiteral("high-dimensional"),
                                                         QStringLiteral("high dimensional"),
                                                         QStringLiteral("dimensionality"),
                                                         QStringLiteral("coherence"),
                                                         QStringLiteral("phase space"),
                                                         QStringLiteral("observable dimension")});
        const bool bayesianLike = keyContainsAny(key,
                                                 {QStringLiteral("bayes"),
                                                  QStringLiteral("bayesian"),
                                                  QStringLiteral("posterior"),
                                                  QStringLiteral("prior"),
                                                  QStringLiteral("model selection"),
                                                  QStringLiteral("inference")});

        if (responseLike) {
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            const QRectF manuscript(area.left() + 8, area.top() + 7, area.width() * 0.54, area.height() - 15);
            painter->drawRoundedRect(manuscript, 3, 3);
            for (int i = 0; i < 4; ++i) {
                const qreal y = manuscript.top() + 8 + i * 7;
                painter->drawLine(QPointF(manuscript.left() + 7, y), QPointF(manuscript.right() - 6 - (i % 2) * 11, y));
            }
            painter->setPen(pen);
            const QRectF comment(area.right() - 41, area.top() + 8, 33, 18);
            painter->drawRoundedRect(comment, 5, 5);
            painter->drawLine(QPointF(comment.left() + 9, comment.bottom()), QPointF(comment.left() + 14, comment.bottom() + 6));
            painter->drawLine(QPointF(manuscript.right() - 4, manuscript.center().y()), QPointF(comment.left() - 4, comment.center().y()));
            painter->setBrush(accent);
            painter->drawEllipse(QPointF(comment.left() + 9, comment.center().y()), 2.1, 2.1);
            painter->drawEllipse(QPointF(comment.left() + 17, comment.center().y()), 2.1, 2.1);
            painter->drawEllipse(QPointF(comment.left() + 25, comment.center().y()), 2.1, 2.1);
            painter->setPen(QPen(accent, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath check;
            check.moveTo(area.right() - 35, area.bottom() - 14);
            check.lineTo(area.right() - 27, area.bottom() - 7);
            check.lineTo(area.right() - 12, area.bottom() - 27);
            painter->drawPath(check);
            return;
        }

        if (manuscriptLike) {
            QColor pageWash = palette.color(QPalette::Base);
            pageWash.setAlphaF(0.34);
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            painter->setBrush(pageWash);
            painter->drawRoundedRect(QRectF(area.left() + 16, area.top() + 6, area.width() - 30, area.height() - 18), 4, 4);
            painter->drawRoundedRect(QRectF(area.left() + 9, area.top() + 11, area.width() - 29, area.height() - 17), 4, 4);
            painter->setPen(pen);
            painter->drawLine(QPointF(area.left() + 20, area.top() + 21), QPointF(area.right() - 22, area.top() + 21));
            painter->drawLine(QPointF(area.left() + 20, area.top() + 29), QPointF(area.right() - 37, area.top() + 29));
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            for (int i = 0; i < 3; ++i) {
                const qreal y = area.top() + 42 + i * 7;
                painter->drawLine(QPointF(area.left() + 20, y), QPointF(area.right() - 18 - i * 9, y));
            }
            painter->setBrush(accent);
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(QRectF(area.left() + 20, area.bottom() - 15, area.width() - 41, 5), 2, 2);
            return;
        }

        if (highDimensionalLike) {
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            const QRectF cube(area.left() + 15, area.top() + 12, area.width() * 0.45, area.height() * 0.48);
            const QPointF offset(area.width() * 0.16, -area.height() * 0.16);
            painter->drawRect(cube);
            painter->drawRect(cube.translated(offset));
            painter->drawLine(cube.topLeft(), cube.topLeft() + offset);
            painter->drawLine(cube.topRight(), cube.topRight() + offset);
            painter->drawLine(cube.bottomLeft(), cube.bottomLeft() + offset);
            painter->drawLine(cube.bottomRight(), cube.bottomRight() + offset);
            painter->setPen(pen);
            QPainterPath manifold;
            manifold.moveTo(area.left() + 8, area.bottom() - 12);
            manifold.cubicTo(area.left() + area.width() * 0.32,
                             area.top() + 15 + seededUnit(key, 151u) * 9,
                             area.left() + area.width() * 0.62,
                             area.bottom() - 6 - seededUnit(key, 157u) * 12,
                             area.right() - 9,
                             area.top() + 17);
            painter->drawPath(manifold);
            painter->setBrush(accent);
            for (int i = 0; i < 5; ++i) {
                const qreal x = area.left() + 15 + i * area.width() * 0.18;
                const qreal y = area.bottom() - 13 - seededUnit(key, 170u + i) * (area.height() - 23);
                painter->drawEllipse(QPointF(x, y), 2.5, 2.5);
            }
            return;
        }

        if (bayesianLike) {
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(QPointF(area.left() + 8, area.bottom() - 8), QPointF(area.right() - 7, area.bottom() - 8));
            painter->drawLine(QPointF(area.left() + 8, area.bottom() - 8), QPointF(area.left() + 8, area.top() + 7));
            painter->setPen(QPen(accent, 1.7, Qt::SolidLine, Qt::RoundCap));
            QPainterPath prior;
            prior.moveTo(area.left() + 9, area.bottom() - 10);
            prior.cubicTo(area.left() + area.width() * 0.27, area.top() + 12, area.left() + area.width() * 0.44, area.top() + 12, area.left() + area.width() * 0.60, area.bottom() - 10);
            painter->drawPath(prior);
            QColor posterior = accent;
            posterior.setAlphaF(0.34);
            painter->setPen(QPen(posterior, 2.0, Qt::DashLine, Qt::RoundCap));
            QPainterPath post;
            post.moveTo(area.left() + area.width() * 0.35, area.bottom() - 10);
            post.cubicTo(area.left() + area.width() * 0.54, area.top() + 5, area.left() + area.width() * 0.72, area.top() + 8, area.right() - 8, area.bottom() - 10);
            painter->drawPath(post);
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(QPointF(area.right() - 26, area.top() + 10), QPointF(area.right() - 26, area.bottom() - 8));
            return;
        }

        if (accessLike) {
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            const qreal midY = area.center().y();
            painter->drawLine(QPointF(area.left() + 8, midY), QPointF(area.right() - 8, midY));
            painter->drawLine(QPointF(area.left() + 8, midY), QPointF(area.left() + 8, area.top() + 9));
            painter->setPen(pen);
            const QList<QPointF> stops = {
                QPointF(area.left() + 12, midY),
                QPointF(area.left() + area.width() * 0.36, midY - 10),
                QPointF(area.left() + area.width() * 0.58, midY + 9),
                QPointF(area.right() - 10, midY - 6),
            };
            for (int i = 0; i < stops.size() - 1; ++i) {
                painter->drawLine(stops.at(i), stops.at(i + 1));
            }
            painter->setBrush(accent);
            for (const QPointF &stop : stops) {
                painter->drawEllipse(stop, 3.0, 3.0);
            }
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            painter->drawRoundedRect(QRectF(area.right() - 28, area.top() + 8, 20, 13), 3, 3);
            return;
        }

        if (electrophysiologyLike) {
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(QPointF(area.left() + 7, area.center().y()), QPointF(area.right() - 7, area.center().y()));
            painter->setPen(pen);
            QPainterPath wave;
            wave.moveTo(area.left() + 8, area.center().y());
            for (int i = 0; i < 4; ++i) {
                const qreal x = area.left() + 10 + i * area.width() * 0.22;
                wave.cubicTo(x + 4, area.top() + 7, x + 12, area.bottom() - 7, x + 20, area.center().y());
            }
            painter->drawPath(wave);
            painter->setBrush(accent);
            for (int i = 0; i < 3; ++i) {
                const qreal x = area.right() - 12 - i * 13;
                painter->drawRoundedRect(QRectF(x, area.top() + 10 + i * 4, 5, area.height() - 19 - i * 4), 2, 2);
            }
            return;
        }

        if (imagingLike) {
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            painter->drawRoundedRect(area.adjusted(7, 5, -7, -5), 6, 6);
            painter->setPen(pen);
            painter->drawEllipse(QRectF(area.left() + 20, area.top() + 8, area.width() - 40, area.height() - 16));
            painter->drawArc(QRectF(area.left() + 30, area.top() + 13, area.width() - 60, area.height() - 26), 30 * 16, 260 * 16);
            painter->setBrush(accent);
            painter->drawEllipse(QPointF(area.right() - 21, area.top() + 16), 3, 3);
            return;
        }

        if (differentialLike) {
            const QPointF origin(area.left() + 10, area.center().y());
            painter->setPen(pen);
            QPainterPath upper;
            upper.moveTo(origin);
            upper.cubicTo(area.left() + area.width() * 0.40, area.top() + 6, area.left() + area.width() * 0.58, area.top() + 8, area.right() - 9, area.top() + 13);
            painter->drawPath(upper);
            QPainterPath lower;
            lower.moveTo(origin);
            lower.cubicTo(area.left() + area.width() * 0.38, area.bottom() - 7, area.left() + area.width() * 0.62, area.bottom() - 8, area.right() - 9, area.bottom() - 13);
            painter->drawPath(lower);
            painter->setPen(QPen(faint, 1.1, Qt::SolidLine, Qt::RoundCap));
            painter->drawEllipse(origin, 3, 3);
            painter->drawText(QRectF(area.right() - 19, area.center().y() - 11, 14, 18), Qt::AlignCenter, QStringLiteral("?"));
            return;
        }

        if (prodromeLike) {
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(QPointF(area.left() + 8, area.bottom() - 10), QPointF(area.right() - 7, area.bottom() - 10));
            painter->setPen(pen);
            QPainterPath trend;
            trend.moveTo(area.left() + 10, area.bottom() - 11);
            trend.cubicTo(area.left() + area.width() * 0.34, area.bottom() - 18, area.left() + area.width() * 0.50, area.top() + 23, area.right() - 17, area.top() + 12);
            painter->drawPath(trend);
            painter->setBrush(accent);
            for (int i = 0; i < 5; ++i) {
                painter->drawEllipse(QPointF(area.left() + 10 + i * area.width() * 0.18, area.bottom() - 11 - i * 5), 2.3, 2.3);
            }
            painter->drawLine(QPointF(area.right() - 16, area.top() + 10), QPointF(area.right() - 16, area.bottom() - 9));
            return;
        }

        if (metabolismLike || molecularLike) {
            painter->setPen(pen);
            const QPointF center = area.center();
            const qreal r = qMin(area.width(), area.height()) * 0.25;
            QPainterPath cell;
            for (int i = 0; i < 6; ++i) {
                const qreal angle = -M_PI / 2 + i * M_PI / 3;
                const QPointF p(center.x() + r * std::cos(angle), center.y() + r * std::sin(angle));
                i == 0 ? cell.moveTo(p) : cell.lineTo(p);
            }
            cell.closeSubpath();
            painter->drawPath(cell);
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            for (int i = 0; i < 3; ++i) {
                const QPointF a(area.left() + 13 + i * 16, area.bottom() - 11 - i * 6);
                const QPointF b(area.left() + 25 + i * 16, area.top() + 12 + i * 5);
                painter->drawLine(a, b);
                painter->drawEllipse(a, 2.0, 2.0);
                painter->drawEllipse(b, 2.0, 2.0);
            }
            return;
        }

        if (pathwayLike) {
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            const QRectF a(area.left() + 8, area.top() + 9, 21, 13);
            const QRectF b(area.center().x() - 10, area.center().y() - 6, 24, 13);
            const QRectF c(area.right() - 31, area.bottom() - 22, 23, 13);
            painter->drawRoundedRect(a, 3, 3);
            painter->drawRoundedRect(b, 3, 3);
            painter->drawRoundedRect(c, 3, 3);
            painter->setPen(pen);
            painter->drawLine(a.center(), b.center());
            painter->drawLine(b.center(), c.center());
            return;
        }

        if (neuroLike && !biomarkerLike && !trialLike && !methodLike && !clinicalLike) {
            const QPointF soma(area.left() + area.width() * 0.22, area.top() + area.height() * 0.55);
            painter->setBrush(accent);
            painter->drawEllipse(soma, area.width() * 0.08, area.width() * 0.08);
            painter->setBrush(Qt::NoBrush);

            QPainterPath axon;
            axon.moveTo(soma);
            const qreal bend = seededUnit(key, 91u) * 10.0 - 5.0;
            axon.cubicTo(area.left() + area.width() * 0.45,
                         area.top() + area.height() * 0.22 + bend,
                         area.left() + area.width() * 0.62,
                         area.top() + area.height() * 0.74 - bend,
                         area.right() - 6,
                         area.top() + area.height() * 0.38);
            painter->drawPath(axon);
            const QList<QLineF> branches = {
                QLineF(area.left() + area.width() * 0.42, area.top() + area.height() * 0.36, area.left() + area.width() * 0.28, area.top() + area.height() * 0.16),
                QLineF(area.left() + area.width() * 0.54, area.top() + area.height() * 0.49, area.left() + area.width() * 0.50, area.bottom() - 5),
                QLineF(area.left() + area.width() * 0.70, area.top() + area.height() * 0.53, area.right() - 12, area.bottom() - 7),
            };
            for (const QLineF &branch : branches) {
                painter->drawLine(branch);
            }
            painter->setPen(QPen(faint, 1.1, Qt::SolidLine, Qt::RoundCap));
            for (int i = 0; i < 3; ++i) {
                painter->drawEllipse(QPointF(area.right() - 14 - i * 13, area.top() + 11 + i * 7), 2.1, 2.1);
            }
            return;
        }

        if (biomarkerLike) {
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            for (int i = 0; i < 4; ++i) {
                const qreal y = area.top() + 8 + i * area.height() * 0.18;
                painter->drawLine(QPointF(area.left() + 7, y), QPointF(area.right() - 7, y));
            }
            painter->setPen(pen);
            QPainterPath curve;
            curve.moveTo(area.left() + 8, area.bottom() - 8);
            curve.cubicTo(area.left() + area.width() * 0.36, area.bottom() - 9, area.left() + area.width() * 0.52, area.top() + 8, area.right() - 7, area.top() + 11);
            painter->drawPath(curve);
            painter->setBrush(accent);
            for (int i = 0; i < 4; ++i) {
                const qreal x = area.left() + 14 + i * area.width() * 0.22;
                painter->drawRoundedRect(QRectF(x, area.bottom() - 9 - i * 5, 5, 9 + i * 5), 2.0, 2.0);
            }
            return;
        }

        if (trialLike) {
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(QPointF(area.left() + 8, area.top() + 9), QPointF(area.left() + 8, area.bottom() - 8));
            painter->drawLine(QPointF(area.left() + 8, area.bottom() - 8), QPointF(area.right() - 7, area.bottom() - 8));
            painter->setPen(pen);
            QPainterPath survival;
            survival.moveTo(area.left() + 10, area.top() + 13);
            survival.lineTo(area.left() + 26, area.top() + 13);
            survival.lineTo(area.left() + 26, area.top() + 23);
            survival.lineTo(area.left() + 44, area.top() + 23);
            survival.lineTo(area.left() + 44, area.top() + 34);
            survival.lineTo(area.right() - 8, area.top() + 34);
            painter->drawPath(survival);
            painter->setBrush(accent);
            painter->drawRoundedRect(QRectF(area.right() - 34, area.bottom() - 25, 8, 17), 2, 2);
            painter->drawRoundedRect(QRectF(area.right() - 21, area.bottom() - 33, 8, 25), 2, 2);
            return;
        }

        if (methodLike) {
            const QPointF origin(area.left() + 8, area.bottom() - 8);
            painter->drawLine(origin, QPointF(area.right() - 6, area.bottom() - 8));
            painter->drawLine(origin, QPointF(area.left() + 8, area.top() + 6));
            painter->setBrush(accent);
            const QList<QPointF> points = {
                QPointF(area.left() + 18, area.bottom() - 16),
                QPointF(area.left() + 30, area.top() + 24),
                QPointF(area.left() + 45, area.top() + 31),
                QPointF(area.right() - 13, area.top() + 12),
            };
            for (const QPointF &point : points) {
                painter->drawEllipse(point, 2.6, 2.6);
            }
            painter->setPen(QPen(faint, 1.1, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(points.at(0), points.at(1));
            painter->drawLine(points.at(1), points.at(2));
            painter->drawLine(points.at(2), points.at(3));
            return;
        }

        if (clinicalLike) {
            const QPointF center = area.center();
            painter->drawLine(QPointF(center.x(), area.top() + 8), QPointF(center.x(), area.bottom() - 8));
            painter->drawLine(QPointF(area.left() + 8, center.y()), QPointF(area.right() - 8, center.y()));
            painter->setPen(QPen(faint, 1.0, Qt::SolidLine, Qt::RoundCap));
            painter->drawRoundedRect(QRectF(area.left() + 8, area.top() + 9, area.width() - 16, area.height() - 18), 5, 5);
            return;
        }

        if (keyContainsAny(key, {QStringLiteral("fiction"), QStringLiteral("book")})) {
            QPainterPath book;
            book.moveTo(area.left() + 7, area.top() + 10);
            book.quadTo(area.center().x() - 2, area.top() + 4, area.center().x(), area.top() + 12);
            book.quadTo(area.center().x() + 2, area.top() + 4, area.right() - 7, area.top() + 10);
            book.lineTo(area.right() - 7, area.bottom() - 8);
            book.quadTo(area.center().x() + 2, area.bottom() - 14, area.center().x(), area.bottom() - 6);
            book.quadTo(area.center().x() - 2, area.bottom() - 14, area.left() + 7, area.bottom() - 8);
            book.closeSubpath();
            painter->drawPath(book);
            painter->drawLine(QPointF(area.center().x(), area.top() + 12), QPointF(area.center().x(), area.bottom() - 6));
            return;
        }

        if (keyContainsAny(key, {QStringLiteral("non-fiction"), QStringLiteral("anthropology"), QStringLiteral("politics"), QStringLiteral("history")})) {
            painter->drawLine(QPointF(area.left() + 8, area.bottom() - 8), QPointF(area.right() - 8, area.bottom() - 8));
            for (int i = 0; i < 3; ++i) {
                const qreal x = area.left() + 13 + i * 11;
                painter->drawLine(QPointF(x, area.bottom() - 8), QPointF(x, area.top() + 11));
            }
            return;
        }

        painter->setPen(QPen(faint, 1.2, Qt::SolidLine, Qt::RoundCap));
        painter->drawRoundedRect(area.adjusted(7, 5, -7, -5), 4, 4);
        for (int i = 0; i < 3; ++i) {
            const qreal y = area.top() + 13 + i * 8;
            painter->drawLine(QPointF(area.left() + 15, y), QPointF(area.right() - 15, y));
        }
        painter->setPen(QPen(accent, 1.5, Qt::SolidLine, Qt::RoundCap));
        const QList<QPointF> nodes = {
            QPointF(area.left() + 16, area.bottom() - 10),
            QPointF(area.center().x(), area.top() + 11),
            QPointF(area.right() - 15, area.bottom() - 15),
        };
        painter->drawLine(nodes.at(0), nodes.at(1));
        painter->drawLine(nodes.at(1), nodes.at(2));
        painter->setBrush(accent);
        for (const QPointF &node : nodes) {
            painter->drawEllipse(node, 2.4, 2.4);
        }
    }

    static void drawCorpusCard(QPainter *painter, const QRect &coverRect, const QModelIndex &index, const QStyleOptionViewItem &option, const QPalette &palette)
    {
        const QString kind = index.data(PaperLibrarySectionedModel::KindRole).toString();
        const QString focus = index.data(PaperLibrarySectionedModel::FocusRole).toString();
        const QString seed = index.data(PaperLibrarySectionedModel::ThumbnailSeedRole).toString();
        const QString fullTitle = index.data(Qt::DisplayRole).toString();
        const QString citationTitle = corpusPaperCitationLabelForIndex(index);
        const QString detail = index.data(PaperLibraryModel::DetailRole).toString();
        const QString intent = index.data(PaperLibrarySectionedModel::ShelfIntentRole).toString();
        const QString relation = index.data(PaperLibrarySectionedModel::RelationHintRole).toString();
        const QString priority = index.data(PaperLibrarySectionedModel::PriorityHintRole).toString();
        const QStringList tags = index.data(PaperLibrarySectionedModel::TopicTagsRole).toStringList();
        const auto *sections = qobject_cast<const PaperLibrarySectionedModel *>(index.model());
        const QString title = corpusCardTitleForIndex(index, citationTitle, sections);
        const bool papersShelf = sections && sections->smartFilter() == PaperLibrarySectionedModel::Papers;
        const bool missing = index.data(PaperLibraryModel::MissingRole).toBool();
        const QString visualKey = visualKeyForCorpusCard(index);
        const QString paperTopic = corpusPaperTopicLabelForKey(visualKey, !focus.isEmpty() && focus != kind ? focus : kind);
        const QString paperSummary = corpusPaperSummaryForKey(visualKey);
        const QString paperThesis = corpusPaperThesisForIndex(index);
        const QString paperMetadata = corpusPaperMetadataLineForIndex(index);

        QPainterPath clip;
        clip.addRoundedRect(coverRect, CoverRadius, CoverRadius);
        const bool darkMode = palette.color(QPalette::Base).lightness() < 128;
        const QString accentSeed = joinCompact({fullTitle, detail, paperTopic, seed});
        QColor accent = CoverGenerator::accentColor(accentSeed.isEmpty() ? kind : accentSeed, darkMode);
        const QColor cardBase = blendColors(palette.color(QPalette::Base), palette.color(QPalette::Text), darkMode ? 0.10 : 0.045);
        QLinearGradient field(coverRect.topLeft(), coverRect.bottomRight());
        field.setColorAt(0.0, blendColors(cardBase, accent, darkMode ? 0.42 : 0.34));
        field.setColorAt(0.55, blendColors(cardBase, accent, darkMode ? 0.30 : 0.24));
        field.setColorAt(1.0, cardBase);
        painter->fillPath(clip, field);

        QColor rim = accent;
        rim.setAlphaF(darkMode ? 0.78 : 0.58);
        painter->setPen(QPen(rim, 1.8));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(QRectF(coverRect).adjusted(0.8, 0.8, -0.8, -0.8), CoverRadius, CoverRadius);

        accent.setAlphaF(0.88);
        painter->setPen(Qt::NoPen);
        painter->setBrush(accent);
        painter->drawRect(QRect(coverRect.left(), coverRect.top(), coverRect.width(), 7));
        painter->drawRect(QRect(coverRect.left(), coverRect.top(), 7, coverRect.height()));

        QColor panel = palette.color(QPalette::Base);
        panel.setAlphaF(darkMode ? 0.70 : 0.82);
        painter->setBrush(panel);
        painter->drawRoundedRect(QRectF(coverRect).adjusted(8, 28, -8, -8), 4, 4);

        QColor wash = accent;
        wash.setAlphaF(darkMode ? 0.26 : 0.20);
        painter->setBrush(wash);
        painter->drawEllipse(QRectF(coverRect.right() - 54, coverRect.top() + 20, 76, 76));
        painter->drawRoundedRect(QRectF(coverRect.left() + 11, coverRect.bottom() - 39, coverRect.width() - 22, 21), 4, 4);

        const qreal visualHeight = qMin<qreal>(76.0, qMax<qreal>(62.0, coverRect.height() * 0.34));
        const QRectF visualRect(coverRect.left() + 13, coverRect.top() + 31, coverRect.width() - 26, visualHeight);
        QColor visualPanel = palette.color(QPalette::Base);
        visualPanel.setAlphaF(darkMode ? 0.36 : 0.48);
        painter->setBrush(visualPanel);
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(visualRect, 5, 5);
        drawCorpusMotif(painter, visualRect.adjusted(4, 3, -4, -10), visualKey, accent, palette);
        drawCorpusFingerprint(painter, visualRect.adjusted(9, visualRect.height() - 11, -9, -4), visualKey, accent, palette);

        QFont kindFont = smallerFont(option.font);
        kindFont.setBold(true);
        QColor muted = palette.color(QPalette::Text);
        muted.setAlphaF(0.50);
        painter->setFont(kindFont);
        painter->setPen(muted);
        const QString topLabel = paperTopic.isEmpty() ? (!focus.isEmpty() && focus != kind ? focus : kind) : paperTopic;
        painter->drawText(coverRect.adjusted(10, 10, -10, -10), Qt::AlignLeft | Qt::AlignTop, QFontMetrics(kindFont).elidedText(topLabel, Qt::ElideRight, coverRect.width() - 20));

        QFont titleFont = option.font;
        titleFont.setBold(true);
        titleFont.setPointSizeF(qMax(8.0, titleFont.pointSizeF() * 0.92));
        painter->setFont(titleFont);
        painter->setPen(palette.color(QPalette::Text));
        const QStringList titleLines = wrapTitle(title, titleFont, coverRect.width() - 20, 3);
        int y = qRound(visualRect.bottom()) + 7;
        const QFontMetrics titleMetrics(titleFont);
        for (const QString &line : titleLines) {
            painter->drawText(QRect(coverRect.left() + 10, y, coverRect.width() - 20, titleMetrics.height()), Qt::AlignLeft | Qt::AlignTop, line);
            y += titleMetrics.height();
        }

        const QFont metaFont = smallerFont(option.font);
        const QFontMetrics metaMetrics(metaFont);
        QColor metaColor = palette.color(QPalette::Text);
        metaColor.setAlphaF(missing ? 0.36 : 0.56);
        painter->setFont(metaFont);
        painter->setPen(metaColor);
        const int metaTop = y + 4;
        QString meta = paperThesis.isEmpty() ? (paperSummary.isEmpty() ? intent : paperSummary) : paperThesis;
        if (!priority.isEmpty() && priority != intent && priority != detail) {
            const QString priorityTopic = corpusPaperTopicLabelForKey(priority.toCaseFolded());
            if (priorityTopic.isEmpty() && priority != topLabel) {
                meta = joinCompact({priority, meta});
            }
        }
        if (meta.isEmpty()) {
            meta = detail;
        }
        if (missing && !meta.contains(QStringLiteral("PDF not local"))) {
            meta = joinCompact({meta, i18nc("@info on a corpus tile whose PDF is not local", "PDF not local")});
        }
        if (!meta.isEmpty()) {
            const QStringList metaLines = wrapTitle(meta, metaFont, coverRect.width() - 20, 2);
            int lineTop = metaTop;
            const int bottomLimit = coverRect.bottom() - 28;
            for (const QString &line : metaLines) {
                if (lineTop + metaMetrics.height() > bottomLimit) {
                    break;
                }
                painter->drawText(QRect(coverRect.left() + 10, lineTop, coverRect.width() - 20, metaMetrics.height()), Qt::AlignLeft | Qt::AlignTop, line);
                lineTop += metaMetrics.height();
            }
        }

        QString tagRow = paperMetadata;
        if (tagRow.isEmpty()) {
            tagRow = papersShelf ? joinCompact({relation, intent}) : relation;
        }
        if (tagRow.isEmpty() && !tags.isEmpty()) {
            tagRow = QStringList(tags.mid(0, 2)).join(QStringLiteral(" · "));
        }
        if (!tagRow.isEmpty()) {
            QColor tagColor = palette.color(QPalette::Text);
            tagColor.setAlphaF(0.58);
            QFont tagFont = smallerFont(option.font);
            tagFont.setBold(true);
            painter->setFont(tagFont);
            painter->setPen(tagColor);
            painter->drawText(coverRect.adjusted(10, 0, -10, -8), Qt::AlignLeft | Qt::AlignBottom, QFontMetrics(tagFont).elidedText(tagRow, Qt::ElideRight, coverRect.width() - 20));
        }
    }

    static void drawDocumentCard(QPainter *painter, const QRect &coverRect, const QModelIndex &index, const QStyleOptionViewItem &option, const QPalette &palette)
    {
        const QString title = index.data(Qt::DisplayRole).toString();
        const QString format = index.data(LibraryView::FormatRole).toString();
        const QStringList tags = index.data(LibraryView::TagsRole).toStringList();
        const QString description = index.data(LibraryView::DescriptionRole).toString();
        const QString seed = QStringList({format, tags.value(0), title}).join(QLatin1Char(' '));
        const bool darkMode = palette.color(QPalette::Base).lightness() < 128;
        QColor accent = CoverGenerator::accentColor(seed, darkMode);

        QPainterPath clip;
        clip.addRoundedRect(coverRect, CoverRadius, CoverRadius);
        const QColor cardBase = blendColors(palette.color(QPalette::Base), palette.color(QPalette::Text), darkMode ? 0.12 : 0.055);
        painter->fillPath(clip, blendColors(cardBase, accent, darkMode ? 0.34 : 0.26));

        QColor spine = accent;
        spine.setAlphaF(0.92);
        painter->setPen(Qt::NoPen);
        painter->setBrush(spine);
        painter->drawRect(QRect(coverRect.left(), coverRect.top(), 8, coverRect.height()));
        painter->drawRect(QRect(coverRect.left(), coverRect.top(), coverRect.width(), 8));

        QColor wash = accent;
        wash.setAlphaF(darkMode ? 0.24 : 0.18);
        painter->setBrush(wash);
        painter->drawEllipse(QRectF(coverRect.right() - 52, coverRect.top() + 18, 78, 78));
        painter->drawRoundedRect(QRectF(coverRect.left() + 14, coverRect.bottom() - 40, coverRect.width() - 28, 22), 5, 5);

        QColor panel = palette.color(QPalette::Base);
        panel.setAlphaF(darkMode ? 0.70 : 0.82);
        painter->setBrush(panel);
        painter->drawRoundedRect(QRectF(coverRect).adjusted(13, 31, -10, -10), 5, 5);

        QFont labelFont = smallerFont(option.font);
        labelFont.setBold(true);
        QColor muted = palette.color(QPalette::Text);
        muted.setAlphaF(0.52);
        painter->setFont(labelFont);
        painter->setPen(muted);
        painter->drawText(coverRect.adjusted(14, 11, -10, -10), Qt::AlignLeft | Qt::AlignTop, QFontMetrics(labelFont).elidedText(format.isEmpty() ? i18nc("@label generated tile cover", "DOCUMENT") : format, Qt::ElideRight, coverRect.width() - 24));

        QFont titleFont = option.font;
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(palette.color(QPalette::Text));
        const QFontMetrics titleMetrics(titleFont);
        const QStringList lines = wrapTitle(title, titleFont, coverRect.width() - 28, 4);
        int y = coverRect.top() + 43;
        for (const QString &line : lines) {
            painter->drawText(QRect(coverRect.left() + 15, y, coverRect.width() - 30, titleMetrics.height()), Qt::AlignLeft | Qt::AlignTop, line);
            y += titleMetrics.height();
        }

        const QFont metaFont = smallerFont(option.font);
        const QFontMetrics metaMetrics(metaFont);
        QColor metaColor = palette.color(QPalette::Text);
        metaColor.setAlphaF(0.58);
        painter->setFont(metaFont);
        painter->setPen(metaColor);
        QString meta = description;
        if (meta.isEmpty() && !tags.isEmpty()) {
            meta = QStringList(tags.mid(0, 2)).join(QStringLiteral(" · "));
        }
        if (!meta.isEmpty()) {
            painter->drawText(QRect(coverRect.left() + 15, coverRect.bottom() - 35, coverRect.width() - 30, metaMetrics.height() * 2),
                              Qt::AlignLeft | Qt::AlignTop,
                              metaMetrics.elidedText(meta, Qt::ElideRight, coverRect.width() - 30));
        }

        QColor rim = accent;
        rim.setAlphaF(darkMode ? 0.70 : 0.48);
        painter->setPen(QPen(rim, 1.5));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(QRectF(coverRect).adjusted(0.7, 0.7, -0.7, -0.7), CoverRadius, CoverRadius);
    }

    static void drawCoverShadow(QPainter *painter, const QRect &coverRect)
    {
        // Ambient shadow in the page view's language: translucent black,
        // wider than dark, slightly heavier below the cover
        painter->setPen(Qt::NoPen);
        for (int i = 4; i >= 1; --i) {
            QColor shadow(0, 0, 0, 20 - 4 * i);
            painter->setBrush(shadow);
            painter->drawRoundedRect(QRectF(coverRect).adjusted(-i, -i + 1.5, i, i + 1.5), CoverRadius + i, CoverRadius + i);
        }
    }
};

LibraryView::LibraryView(LibraryStore *store, QWidget *parent, bool deferInitialRefresh)
    : QWidget(parent)
    , m_store(store)
    , m_deferInitialRefresh(deferInitialRefresh)
{
    setAcceptDrops(true); // the shell's event filter opens dropped files
    setAutoFillBackground(true);

    // Compact header metrics: the row sits flush under the tab strip in
    // the slot the toolbar row occupies on document tabs
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 6, 12, 0);
    mainLayout->setSpacing(8);

    QLabel *titleLabel = new QLabel(i18nc("@title of the document library view", "PaperLibrary"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.3);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);

    m_shelfSwitch = new QTabBar(this);
    m_shelfSwitch->setDocumentMode(true);
    m_shelfSwitch->setExpanding(false);
    m_shelfSwitch->setDrawBase(false);
    m_shelfSwitch->setFocusPolicy(Qt::NoFocus);
    addShelfTab(PdfShelf, i18nc("library shelf with recently opened documents", "Recent"));
    addShelfTab(BooksShelf, i18nc("library shelf with current long-form EPUB reading", "Books"));
    addShelfTab(FinishedShelf, i18nc("library shelf with completed long-form reading", "Finished"));
    addShelfTab(FictionShelf, i18nc("library smart shelf for fiction books", "Fiction"));
    addShelfTab(NonfictionShelf, i18nc("library smart shelf for non-fiction books", "Non-fiction"));
    addShelfTab(WorkShelf, i18nc("library smart shelf for current work documents", "Work"));
    addShelfTab(StarterPackShelf, i18nc("library shelf for public-domain starter books", "Starter Pack"));

    // Restore each shelf's persisted arrangement before the first populate
    for (int shelf = PdfShelf; shelf < DocumentShelfCount; ++shelf) {
        m_viewModes[shelf] = viewModeFromName(partGeneralConfig().readEntry(viewModeConfigKey(static_cast<Shelf>(shelf)), QString()));
    }
    for (int shelf = PdfShelf; shelf <= PapersShelf; ++shelf) {
        const Shelf typedShelf = static_cast<Shelf>(shelf);
        m_paperSectionModes[shelf] = paperSectionModeFromName(partGeneralConfig().readEntry(paperSectionModeConfigKey(typedShelf), QString()), defaultPaperSectionModeForShelf(typedShelf));
    }
    m_corpusSearchMode = corpusSearchModeFromName(partGeneralConfig().readEntry("CorpusSearchMode", QString()));

    // A quiet menu button next to the shelf switch chooses the arrangement
    m_viewModeButton = new QToolButton(this);
    m_viewModeButton->setAutoRaise(true);
    m_viewModeButton->setPopupMode(QToolButton::InstantPopup);
    m_viewModeButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_viewModeButton->setFocusPolicy(Qt::NoFocus);
    m_viewModeButton->setToolTip(i18nc("@info:tooltip on the library's view mode button", "Choose how this shelf is arranged"));
    QMenu *viewModeMenu = new QMenu(m_viewModeButton);
    QActionGroup *viewModeGroup = new QActionGroup(viewModeMenu);
    const QString viewModeNames[] = {i18nc("@item:inmenu library arrangement", "Frequent"), i18nc("@item:inmenu library arrangement", "By Type"), i18nc("@item:inmenu library arrangement", "By Folder")};
    for (int mode = FrequentMode; mode <= FolderMode; ++mode) {
        QAction *action = viewModeMenu->addAction(viewModeNames[mode]);
        action->setCheckable(true);
        action->setActionGroup(viewModeGroup);
        connect(action, &QAction::triggered, this, [this, mode]() { setViewMode(activeShelf(), static_cast<ViewMode>(mode)); });
        m_viewModeActions[mode] = action;
    }
    m_viewModeButton->setMenu(viewModeMenu);

    m_paperSectionButton = new QToolButton(this);
    m_paperSectionButton->setAutoRaise(true);
    m_paperSectionButton->setPopupMode(QToolButton::InstantPopup);
    m_paperSectionButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_paperSectionButton->setFocusPolicy(Qt::NoFocus);
    m_paperSectionButton->setToolTip(i18nc("@info:tooltip on the PaperLibrary corpus grouping button", "Choose how corpus records are grouped"));
    m_paperSectionButton->hide();
    QMenu *paperSectionMenu = new QMenu(m_paperSectionButton);
    QActionGroup *paperSectionGroup = new QActionGroup(paperSectionMenu);
    const QString sectionModeNames[] = {i18nc("@item:inmenu corpus arrangement", "For you"),
                                        i18nc("@item:inmenu corpus arrangement", "Topics"),
                                        i18nc("@item:inmenu corpus arrangement", "Projects"),
                                        i18nc("@item:inmenu corpus arrangement", "Types"),
                                        i18nc("@item:inmenu corpus arrangement", "Sources"),
                                        i18nc("@item:inmenu corpus arrangement", "Years"),
                                        i18nc("@item:inmenu corpus arrangement", "Journals")};
    for (int mode = PaperLibrarySectionedModel::ReadNext; mode <= PaperLibrarySectionedModel::ByJournal; ++mode) {
        QAction *action = paperSectionMenu->addAction(sectionModeNames[mode]);
        action->setCheckable(true);
        action->setActionGroup(paperSectionGroup);
        connect(action, &QAction::triggered, this, [this, mode]() {
            setPaperSectionMode(activeShelf(), mode);
        });
        m_paperSectionActions[mode] = action;
    }
    m_paperSectionButton->setMenu(paperSectionMenu);

    m_corpusSearchButton = new QToolButton(this);
    m_corpusSearchButton->setObjectName(QStringLiteral("corpusSearchModeButton"));
    m_corpusSearchButton->setAutoRaise(true);
    m_corpusSearchButton->setPopupMode(QToolButton::InstantPopup);
    m_corpusSearchButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_corpusSearchButton->setFocusPolicy(Qt::NoFocus);
    m_corpusSearchButton->setToolTip(i18nc("@info:tooltip on the corpus search mode button", "Choose whether search scans shelf metadata or the full-text index"));
    m_corpusSearchButton->hide();
    QMenu *corpusSearchMenu = new QMenu(m_corpusSearchButton);
    QActionGroup *corpusSearchGroup = new QActionGroup(corpusSearchMenu);
    const QString corpusSearchNames[] = {i18nc("@item:inmenu corpus search mode", "Shelf"), i18nc("@item:inmenu corpus search mode", "Full text")};
    for (int mode = ShelfMetadataSearch; mode <= FullTextSearch; ++mode) {
        QAction *action = corpusSearchMenu->addAction(corpusSearchNames[mode]);
        action->setCheckable(true);
        action->setActionGroup(corpusSearchGroup);
        connect(action, &QAction::triggered, this, [this, mode]() {
            setCorpusSearchMode(static_cast<CorpusSearchMode>(mode));
        });
        m_corpusSearchActions[mode] = action;
    }
    m_corpusSearchButton->setMenu(corpusSearchMenu);

    m_corpusResultButton = new QToolButton(this);
    m_corpusResultButton->setObjectName(QStringLiteral("corpusResultModeButton"));
    m_corpusResultButton->setAutoRaise(true);
    m_corpusResultButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_corpusResultButton->setFocusPolicy(Qt::NoFocus);
    m_corpusResultButton->setMaximumWidth(260);
    m_corpusResultButton->setToolTip(i18nc("@info:tooltip on corpus result mode chip", "Click to leave the current search result mode"));
    m_corpusResultButton->hide();
    connect(m_corpusResultButton, &QToolButton::clicked, this, &LibraryView::clearActiveCorpusResult);

    // A compact search field filters the active shelf as you type; ⌘F
    // focuses it while the library is showing (a widget-local shortcut, so
    // the part's find action keeps ⌘F while reading)
    m_searchField = new QLineEdit(this);
    m_searchField->setPlaceholderText(i18nc("@info:placeholder in the library's search field", "Search library"));
    m_searchField->setClearButtonEnabled(true);
    m_searchField->setMaximumWidth(220);
    connect(m_searchField, &QLineEdit::textChanged, this, &LibraryView::applySearch);

    QShortcut *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, this, [this] {
        m_searchField->setFocus(Qt::ShortcutFocusReason);
        m_searchField->selectAll();
    });

    // Layer 2 of the search (document contents through Spotlight) only runs
    // once the query has settled for a moment
    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(300);
    connect(m_searchDebounce, &QTimer::timeout, this, &LibraryView::startContentSearch);
    m_shelfRenderTimer = new QTimer(this);
    m_shelfRenderTimer->setSingleShot(true);
    m_shelfRenderTimer->setInterval(0);
    connect(m_shelfRenderTimer, &QTimer::timeout, this, &LibraryView::renderPendingShelf);

    m_openButton = new QPushButton(i18n("Open File…"), this);
    connect(m_openButton, &QPushButton::clicked, this, &LibraryView::openClicked);

    QHBoxLayout *headerLayout = new QHBoxLayout;
    headerLayout->addWidget(titleLabel);
    headerLayout->addSpacing(16);
    headerLayout->addWidget(m_shelfSwitch, 0, Qt::AlignBottom);
    headerLayout->addSpacing(12);
    headerLayout->addWidget(m_viewModeButton, 0, Qt::AlignBottom);
    headerLayout->addWidget(m_paperSectionButton, 0, Qt::AlignBottom);
    headerLayout->addWidget(m_corpusSearchButton, 0, Qt::AlignBottom);
    headerLayout->addWidget(m_corpusResultButton, 0, Qt::AlignBottom);
    headerLayout->addSpacing(12);
    headerLayout->addWidget(m_searchField, 0, Qt::AlignBottom);
    headerLayout->addStretch();
    headerLayout->addWidget(m_openButton);
    mainLayout->addLayout(headerLayout);

    m_pdfModel = new QStandardItemModel(this);
    m_booksModel = new QStandardItemModel(this);
    m_textbooksModel = new QStandardItemModel(this);
    m_medicineModel = new QStandardItemModel(this);
    m_mndModel = new QStandardItemModel(this);
    m_workModel = new QStandardItemModel(this);
    m_fictionModel = new QStandardItemModel(this);
    m_nonfictionModel = new QStandardItemModel(this);
    m_starterPackModel = new QStandardItemModel(this);
    m_finishedModel = new QStandardItemModel(this);
    m_booksModel->setProperty("booksShelf", true);
    m_starterPackModel->setProperty("booksShelf", true);
    m_finishedModel->setProperty("booksShelf", true);

    m_grid = new QListView(this);
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_grid->setDragDropMode(QAbstractItemView::NoDragDrop);
    m_grid->setWordWrap(false);
    m_grid->setFocusPolicy(Qt::StrongFocus);
    m_grid->setMouseTracking(true);
    m_grid->setFrameShape(QFrame::NoFrame);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_grid->viewport()->setAutoFillBackground(false);
    m_grid->viewport()->installEventFilter(this);
    m_grid->setItemDelegate(new LibraryTileDelegate(m_grid));
    configureTileGrid();
    m_grid->setModel(modelForShelf(PdfShelf));
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_gridFadeEffect = new QGraphicsOpacityEffect(m_grid);
    m_gridFadeEffect->setOpacity(1.0);
    m_grid->setGraphicsEffect(m_gridFadeEffect);
    m_gridFadeAnimation = new QPropertyAnimation(m_gridFadeEffect, "opacity", this);
    m_gridFadeAnimation->setDuration(230);
    m_gridFadeAnimation->setEasingCurve(QEasingCurve::OutCubic);
    mainLayout->addWidget(m_grid, 1);

    // The PaperLibrary corpus earns its shelf only when it is actually there
    m_paperCorpusDir = PaperLibraryModel::configuredCorpusDir();
    if (PaperLibraryModel::corpusExists(m_paperCorpusDir)) {
        setupPapersShelf();
    }

    m_coverLoader = new CoverLoader(this);
    connect(m_coverLoader, &CoverLoader::coverReady, this, &LibraryView::coverArrived);
    m_corpusCoverWarmupTimer = new QTimer(this);
    m_corpusCoverWarmupTimer->setSingleShot(true);
    m_corpusCoverWarmupTimer->setInterval(750);
    connect(m_corpusCoverWarmupTimer, &QTimer::timeout, this, &LibraryView::requestNextCorpusCoverBatch);
    if (m_paperModel) {
        QTimer::singleShot(0, this, [this]() {
            if (m_paperModel && !m_paperModel->isLoaded()) {
                m_paperModel->load(m_paperCorpusDir);
            }
        });
    }

    connect(m_shelfSwitch, &QTabBar::currentChanged, this, &LibraryView::shelfChanged);
    connect(m_grid, &QListView::doubleClicked, this, &LibraryView::tileClicked);
    connect(m_grid, &QWidget::customContextMenuRequested, this, &LibraryView::showContextMenu);
    connect(m_grid->verticalScrollBar(), &QScrollBar::valueChanged, this, &LibraryView::maybeFetchMoreRowsForActiveShelf);
    connect(m_grid->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) {
        maybeFetchMoreRowsForActiveShelf();
    });

    auto addShortcut = [this](const QKeySequence &sequence, const auto &slot) {
        QShortcut *shortcut = new QShortcut(sequence, this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, slot);
        return shortcut;
    };
    addShortcut(QKeySequence(Qt::CTRL | Qt::Key_O), [this]() {
        Q_EMIT openClicked();
    });
    addShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), [this]() {
        m_shelfSwitch->setCurrentIndex(qMax(0, m_shelfSwitch->currentIndex() - 1));
    });
    addShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketRight), [this]() {
        m_shelfSwitch->setCurrentIndex(qMin(m_shelfSwitch->count() - 1, m_shelfSwitch->currentIndex() + 1));
    });
    for (int shelf = 0; shelf < qMin(9, m_shelfSwitch->count()); ++shelf) {
        addShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + shelf)), [this, shelf]() {
            if (shelf < m_shelfSwitch->count()) {
                m_shelfSwitch->setCurrentIndex(shelf);
            }
        });
    }
    auto addGridOpenShortcut = [this](const QKeySequence &sequence) {
        QShortcut *shortcut = new QShortcut(sequence, m_grid);
        shortcut->setContext(Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, this, &LibraryView::activateCurrentTile);
    };
    addGridOpenShortcut(QKeySequence(Qt::Key_Return));
    addGridOpenShortcut(QKeySequence(Qt::Key_Enter));
    addGridOpenShortcut(QKeySequence(Qt::Key_Space));
    QShortcut *adjacentShortcut = new QShortcut(QKeySequence(Qt::Key_A), m_grid);
    adjacentShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(adjacentShortcut, &QShortcut::activated, this, [this]() {
        showAdjacentDocumentsForCurrentTile();
    });

    applyChromePalette();
    syncViewModeButton();
    if (!m_deferInitialRefresh) {
        showStartupPlaceholder();
        scheduleRefresh(180);
    }
}

void LibraryView::setViewMode(Shelf shelf, ViewMode mode)
{
    if (!isDocumentShelf(shelf)) {
        return; // the corpus shelf has a single fixed arrangement
    }
    if (m_viewModes[shelf] == mode) {
        return;
    }
    m_viewModes[shelf] = mode;
    KConfigGroup config = partGeneralConfig();
    config.writeEntry(viewModeConfigKey(shelf), viewModeName(mode));
    config.sync();
    syncViewModeButton();
    refresh();
}

LibraryView::ViewMode LibraryView::viewMode(Shelf shelf) const
{
    return isDocumentShelf(shelf) ? m_viewModes[shelf] : FrequentMode;
}

void LibraryView::syncViewModeButton()
{
    const Shelf shelf = activeShelf();
    if (!isDocumentShelf(shelf)) {
        return; // the button is hidden while the corpus shelf shows
    }
    const ViewMode mode = m_viewModes[shelf];
    m_viewModeActions[mode]->setChecked(true);
    m_viewModeButton->setText(m_viewModeActions[mode]->text());
}

void LibraryView::animateGridIn()
{
    if (!m_gridFadeEffect || !m_gridFadeAnimation) {
        return;
    }
    m_gridFadeAnimation->stop();
    m_gridFadeEffect->setOpacity(0.48);
    m_gridFadeAnimation->setDuration(230);
    m_gridFadeAnimation->setStartValue(0.48);
    m_gridFadeAnimation->setKeyValueAt(0.28, 0.72);
    m_gridFadeAnimation->setKeyValueAt(0.62, 0.91);
    m_gridFadeAnimation->setEndValue(1.0);
    m_gridFadeAnimation->start();
}

void LibraryView::showStartupPlaceholder()
{
    if (!m_pdfModel) {
        return;
    }

    m_pdfModel->clear();
    ShelfEntry entry;
    entry.title = i18nc("@title startup tile while library opens", "Opening PaperLibrary");
    entry.description = i18nc("@info startup tile while library opens", "Preparing shelves, metadata, and thumbnails");
    entry.tags = {i18nc("@label startup tile", "Startup")};
    entry.format = i18nc("@label generated tile cover", "OPENING");
    entry.detailLines = {i18nc("@info startup tile detail", "Local files stay on this device.")};
    m_pdfModel->appendRow(makeTileItem(entry));
    if (activeShelf() == PdfShelf && m_grid) {
        m_grid->setModel(m_pdfModel);
        configureTileGrid();
        selectFirstTile();
    }
}

void LibraryView::configureTileGrid()
{
    if (!m_grid) {
        return;
    }

    const QFontMetrics titleMetrics(m_grid->font());
    const QFontMetrics smallMetrics(smallerFont(m_grid->font()));
    const bool corpus = usesCorpusList(activeShelf());
    const int corpusKey = corpus ? 1 : 0;
    if (m_configuredGridCorpus == corpusKey) {
        return;
    }
    m_configuredGridCorpus = corpusKey;
    const int coverHeight = corpus ? CorpusCoverHeight : CoverHeight;
    const int tileWidth = corpus ? CorpusTileWidth : TileWidth;
    const int tileHeight = TilePadding + coverHeight + ProgressGap + smallMetrics.height() + TitleGap + TitleLines * titleMetrics.height() + TagGap + MetadataLines * smallMetrics.height() + TilePadding;

    m_grid->setViewMode(QListView::IconMode);
    m_grid->setLayoutMode(QListView::Batched);
    m_grid->setBatchSize(64);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setFlow(QListView::LeftToRight);
    m_grid->setWrapping(true);
    m_grid->setSpacing(GridSpacing);
    m_grid->setGridSize(QSize(tileWidth + GridSpacing, tileHeight + GridSpacing));
    m_grid->setUniformItemSizes(true);
}

void LibraryView::refresh()
{
    auto persistCorrectedTags = [this](const LibraryStore::Entry &stored, const ShelfEntry &shelfEntry) {
        if (stored.tags.isEmpty() || stored.tags == shelfEntry.tags) {
            return;
        }
        bool removedStoredTag = false;
        for (const QString &tag : stored.tags) {
            const QString key = compactPublicationTypeKey(tag);
            bool retained = false;
            for (const QString &candidate : shelfEntry.tags) {
                if (compactPublicationTypeKey(candidate) == key) {
                    retained = true;
                    break;
                }
            }
            if (!retained) {
                removedStoredTag = true;
                break;
            }
        }
        if (!removedStoredTag) {
            return;
        }
        m_store->setTags(shelfEntry.url, shelfEntry.tags);
    };

    const QList<LibraryStore::Entry> pdfEntries = m_store->entries({QStringLiteral("pdf")});
    QList<ShelfEntry> pdfs;
    pdfs.reserve(pdfEntries.size());
    for (const LibraryStore::Entry &entry : pdfEntries) {
        // Title precedence: curated store title, else filename sans extension
        const QString title = cleanedLocalTitle(entry.title.isEmpty() ? cleanedFilenameTitle(entry.url) : entry.title, entry.url);
        ShelfEntry shelfEntry{entry.url, title, entry.tags, entry.description, entry.keywords, entry.pinned, entry.downranked, entry.finishedReading, entry.openCount, entry.lastOpened, -1.0, QStringLiteral("PDF"), {}};
        enrichShelfEntryFromCorpus(shelfEntry);
        enrichShelfEntry(shelfEntry);
        persistCorrectedTags(entry, shelfEntry);
        pdfs.append(shelfEntry);
    }

    // Books shelf: the store's EPUB entries, annotated with Apple Books
    // reading progress where the canonical paths match, plus books that
    // exist only in Apple Books
    const QList<AppleBooksProgress::BookEntry> bookEntries = appleBooksScanEnabled() ? AppleBooksProgress::read() : QList<AppleBooksProgress::BookEntry>();
    QHash<QString, double> progressByPath;
    QHash<QString, QString> booksTitleByPath;
    const bool mirrorAppleBooks = !bookEntries.isEmpty();
    for (const AppleBooksProgress::BookEntry &book : bookEntries) {
        const QString canonical = QFileInfo(book.path).canonicalFilePath();
        if (!canonical.isEmpty()) {
            progressByPath.insert(canonical, book.progress);
            booksTitleByPath.insert(canonical, book.title);
        }
    }

    QList<ShelfEntry> books;
    QList<ShelfEntry> finishedBooks;
    QSet<QString> knownPaths;
    const QString importedBooksPrefix = EpubImporter::importDir() + QLatin1Char('/');
    const QList<LibraryStore::Entry> epubEntries = m_store->entries({QStringLiteral("epub")});
    for (const LibraryStore::Entry &entry : epubEntries) {
        // An imported copy of an Apple Books bundle is an implementation
        // detail: the bundle's own tile (with live reading progress) stands
        // in for it, so it never gets a second tile of its own.
        if (entry.url.isLocalFile() && entry.url.toLocalFile().startsWith(importedBooksPrefix)) {
            continue;
        }
        const QString canonical = entry.url.isLocalFile() ? QFileInfo(entry.url.toLocalFile()).canonicalFilePath() : QString();
        if (mirrorAppleBooks && !entry.finishedReading && (canonical.isEmpty() || !progressByPath.contains(canonical))) {
            continue;
        }
        if (!canonical.isEmpty()) {
            knownPaths.insert(canonical);
        }
        // Title precedence: curated store title, else Apple Books title, else filename
        QString title = entry.title.isEmpty() ? booksTitleByPath.value(canonical) : entry.title;
        if (title.isEmpty()) {
            title = cleanedFilenameTitle(entry.url);
        }
        title = cleanedLocalTitle(title, entry.url);
        const EpubCover::Metadata *metadata = nullptr;
        if (entry.url.isLocalFile()) {
            metadata = &epubMetadataFor(entry.url.toLocalFile());
        }
        ShelfEntry shelfEntry{
            entry.url, title, entry.tags, entry.description, entry.keywords, entry.pinned, entry.downranked, entry.finishedReading, entry.openCount, entry.lastOpened, progressByPath.value(canonical, -1.0), QStringLiteral("EPUB"), {}};
        enrichShelfEntryFromCorpus(shelfEntry);
        enrichShelfEntry(shelfEntry, metadata);
        persistCorrectedTags(entry, shelfEntry);
        if (shelfEntry.finishedReading) {
            finishedBooks.append(shelfEntry);
        } else {
            books.append(shelfEntry);
        }
    }

    QList<ShelfEntry> booksOnly;
    for (const AppleBooksProgress::BookEntry &book : bookEntries) {
        const QString canonical = QFileInfo(book.path).canonicalFilePath();
        if (canonical.isEmpty() || knownPaths.contains(canonical)) {
            continue;
        }
        const QUrl url = QUrl::fromLocalFile(book.path);
        const LibraryStore::Entry stored = m_store->metadata(url);
        QString title = stored.title.isEmpty() ? book.title : stored.title;
        if (title.isEmpty()) {
            title = cleanedFilenameTitle(url);
        }
        title = cleanedLocalTitle(title, url);
        const EpubCover::Metadata metadata = EpubCover::metadata(book.path);
        ShelfEntry shelfEntry{url, title, stored.tags, stored.description, stored.keywords, stored.pinned, stored.downranked, stored.finishedReading, stored.openCount, stored.lastOpened, book.progress, QStringLiteral("EPUB"), {}};
        enrichShelfEntry(shelfEntry, &metadata);
        persistCorrectedTags(stored, shelfEntry);
        if (shelfEntry.finishedReading) {
            finishedBooks.append(shelfEntry);
        } else {
            booksOnly.append(shelfEntry);
        }
    }
    const auto entryLikelyBefore = [](const ShelfEntry &a, const ShelfEntry &b) {
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
        return a.title.localeAwareCompare(b.title) < 0;
    };
    const auto bookLikelyBefore = [&entryLikelyBefore](const ShelfEntry &a, const ShelfEntry &b) {
        const bool aActive = a.progress > 0.0 && a.progress < 0.98;
        const bool bActive = b.progress > 0.0 && b.progress < 0.98;
        if (aActive != bActive) {
            return aActive;
        }
        const bool aTracked = a.progress >= 0.0;
        const bool bTracked = b.progress >= 0.0;
        if (aTracked != bTracked) {
            return aTracked;
        }
        return entryLikelyBefore(a, b);
    };
    std::sort(booksOnly.begin(), booksOnly.end(), bookLikelyBefore);
    books += booksOnly;
    std::stable_sort(books.begin(), books.end(), bookLikelyBefore);
    std::stable_sort(finishedBooks.begin(), finishedBooks.end(), entryLikelyBefore);
    m_shelfEntries[BooksShelf] = books;
    m_shelfEntries[FinishedShelf] = finishedBooks;

    const QList<ShelfEntry> allDocuments = pdfs + books;
    QList<ShelfEntry> recent = allDocuments;
    std::stable_sort(recent.begin(), recent.end(), entryLikelyBefore);
    m_shelfEntries[PdfShelf] = recent;

    QList<ShelfEntry> textbooks;
    QList<ShelfEntry> medicine;
    QList<ShelfEntry> mnd;
    QList<ShelfEntry> work;
    QList<ShelfEntry> fiction;
    QList<ShelfEntry> nonfiction;
    for (const ShelfEntry &entry : allDocuments) {
        if (isTextbookEntry(entry)) {
            textbooks.append(entry);
        }
        if (isMedicineEntry(entry) && isTextbookEntry(entry)) {
            medicine.append(entry);
        }
        if (isMndEntry(entry)) {
            mnd.append(entry);
        }
        if (isWorkEntry(entry)) {
            work.append(entry);
        }
        if (isFictionEntry(entry)) {
            fiction.append(entry);
        }
        if (isNonfictionEntry(entry)) {
            nonfiction.append(entry);
        }
    }
    m_shelfEntries[TextbooksShelf] = textbooks;
    m_shelfEntries[MedicineShelf] = medicine;
    m_shelfEntries[MndShelf] = mnd;
    m_shelfEntries[WorkShelf] = work;
    m_shelfEntries[FictionShelf] = fiction;
    m_shelfEntries[NonfictionShelf] = nonfiction;
    m_shelfEntries[StarterPackShelf] = loadStarterPackEntries();
    m_finishedModel->setProperty("booksShelf", true);
    m_textbooksModel->setProperty("booksShelf", shelfHasReadingProgress(textbooks));
    m_medicineModel->setProperty("booksShelf", shelfHasReadingProgress(medicine));
    m_mndModel->setProperty("booksShelf", shelfHasReadingProgress(mnd));
    m_workModel->setProperty("booksShelf", shelfHasReadingProgress(work));
    m_fictionModel->setProperty("booksShelf", shelfHasReadingProgress(fiction));
    m_nonfictionModel->setProperty("booksShelf", shelfHasReadingProgress(nonfiction));
    m_starterPackModel->setProperty("booksShelf", shelfHasReadingProgress(m_shelfEntries[StarterPackShelf]));

    for (QList<ShelfEntry> *smartShelf : {&m_shelfEntries[TextbooksShelf], &m_shelfEntries[MedicineShelf], &m_shelfEntries[MndShelf], &m_shelfEntries[WorkShelf], &m_shelfEntries[FictionShelf], &m_shelfEntries[NonfictionShelf]}) {
        std::stable_sort(smartShelf->begin(), smartShelf->end(), entryLikelyBefore);
    }

    if (m_paperModel && m_paperModel->isLoaded()) {
        m_paperModel->reloadIfChanged(); // one mtime stat; a grown catalog re-parses
    }

    applySearch(); // populates both shelf models, filtered when a query is active
}

QList<LibraryView::Section> LibraryView::arrangeSections(const QList<ShelfEntry> &entries, ViewMode mode)
{
    // Incoming entries keep the store's ranking (pinned, then open count,
    // then recency); the grouped modes preserve it within each section
    QList<Section> sections;

    if (mode == GenreMode) {
        return arrangePublicationTypeSections(entries);
    }

    if (mode == FolderMode) {
        QHash<QString, int> sectionIndexByDir;
        QStringList dirs; // one per section, in section order
        QList<ShelfEntry> nonLocal;
        for (const ShelfEntry &entry : entries) {
            if (!entry.url.isLocalFile()) {
                nonLocal.append(entry);
                continue;
            }
            const QString dir = QFileInfo(entry.url.toLocalFile()).absolutePath();
            const auto it = sectionIndexByDir.constFind(dir);
            if (it == sectionIndexByDir.cend()) {
                sectionIndexByDir.insert(dir, sections.size());
                dirs.append(dir);
                sections.append({dir, {entry}});
            } else {
                sections[it.value()].entries.append(entry);
            }
        }

        // Longest common prefix of the parent folders, in path segments
        QStringList prefix = dirs.isEmpty() ? QStringList() : dirs.first().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString &dir : std::as_const(dirs)) {
            const QStringList segments = dir.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            int common = 0;
            while (common < prefix.size() && common < segments.size() && prefix.at(common) == segments.at(common)) {
                ++common;
            }
            prefix = prefix.mid(0, common);
        }

        // Trim the shared prefix from the section titles, always keeping
        // at least the folder's own name
        for (Section &section : sections) {
            const QStringList segments = section.title.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (segments.isEmpty()) {
                continue; // the filesystem root; leave the raw path
            }
            const int drop = qMin(int(prefix.size()), int(segments.size()) - 1);
            section.title = segments.mid(drop).join(QLatin1Char('/'));
        }
        std::sort(sections.begin(), sections.end(), [](const Section &a, const Section &b) { return a.title.localeAwareCompare(b.title) < 0; });
        if (!nonLocal.isEmpty()) {
            sections.append({i18nc("library folder section for documents that are not local files", "Other"), nonLocal});
        }
        return sections;
    }

    // FrequentMode: publication-type sections, with the store's pinned /
    // frequency ranking preserved inside each type.
    return arrangePublicationTypeSections(entries);
}

QList<LibraryView::Section> LibraryView::arrangePublicationTypeSections(const QList<ShelfEntry> &entries)
{
    QHash<QString, int> sectionIndexByType;
    QList<Section> sections;

    for (const ShelfEntry &entry : entries) {
        const QString type = publicationTypeTitle(entry);
        const QString key = compactPublicationTypeKey(type);
        const auto it = sectionIndexByType.constFind(key);
        if (it == sectionIndexByType.cend()) {
            sectionIndexByType.insert(key, sections.size());
            sections.append({type, {entry}});
        } else {
            sections[it.value()].entries.append(entry);
        }
    }

    std::sort(sections.begin(), sections.end(), [](const Section &a, const Section &b) {
        const int leftRank = publicationTypeRank(a.title);
        const int rightRank = publicationTypeRank(b.title);
        if (leftRank != rightRank) {
            return leftRank < rightRank;
        }
        return a.title.localeAwareCompare(b.title) < 0;
    });
    return sections;
}

QString LibraryView::publicationTypeTitle(const ShelfEntry &entry)
{
    QString rawType;
    for (const QString &tag : entry.tags) {
        const QString candidate = tag.trimmed();
        if (isPublicationTypeKey(compactPublicationTypeKey(candidate))) {
            rawType = candidate;
            break;
        }
    }

    const QString key = compactPublicationTypeKey(rawType);
    if (key.isEmpty() || key == QLatin1String("unidentified") || key == QLatin1String("unknown") || key == QLatin1String("uncategorized") || key == QLatin1String("untagged")) {
        if (entry.format == QLatin1String("EPUB")) {
            return i18nc("library section for book documents", "Books");
        }
        if (entry.format == QLatin1String("PDF")) {
            return i18nc("library section for paper documents", "Papers");
        }
        return i18nc("library section for documents whose publication type is unknown", "Unidentified");
    }
    if (key == QLatin1String("textbook") || key == QLatin1String("textbooks")) {
        return i18nc("library section for textbook documents", "Textbooks");
    }
    if (key == QLatin1String("book") || key == QLatin1String("books")) {
        return i18nc("library section for book documents", "Books");
    }
    if (key == QLatin1String("paper") || key == QLatin1String("papers")) {
        return i18nc("library section for paper documents", "Papers");
    }
    if (key == QLatin1String("manuscript") || key == QLatin1String("manuscripts")) {
        return i18nc("library section for manuscript documents", "Manuscripts");
    }
    if (key == QLatin1String("peerreview") || key == QLatin1String("peerreviews") || key == QLatin1String("review") || key == QLatin1String("reviews")) {
        return i18nc("library section for peer review documents", "Peer Review");
    }
    if (key == QLatin1String("guideline") || key == QLatin1String("guidelines")) {
        return i18nc("library section for guideline documents", "Guidelines");
    }
    if (key == QLatin1String("other") || key == QLatin1String("misc") || key == QLatin1String("miscellaneous")) {
        return i18nc("library section for documents grouped as other", "Other");
    }
    return titleCasedLabel(rawType);
}

bool LibraryView::isDocumentShelf(Shelf shelf)
{
    return shelf == PdfShelf || shelf == BooksShelf || shelf == TextbooksShelf || shelf == MedicineShelf || shelf == MndShelf || shelf == WorkShelf || shelf == FictionShelf || shelf == NonfictionShelf || shelf == StarterPackShelf || shelf == FinishedShelf;
}

QString LibraryView::smartShelfHaystack(const ShelfEntry &entry)
{
    QStringList fields;
    fields << entry.title << entry.description << entry.url.fileName();
    if (entry.url.isLocalFile()) {
        fields << entry.url.toLocalFile();
    }
    fields << entry.tags << entry.keywords;
    return fields.join(QLatin1Char(' ')).toCaseFolded();
}

QString LibraryView::smartShelfContentHaystack(const ShelfEntry &entry)
{
    QStringList fields;
    fields << entry.title << entry.description << entry.url.fileName();
    if (entry.url.isLocalFile()) {
        fields << entry.url.toLocalFile();
    }
    fields << entry.detailLines;
    return fields.join(QLatin1Char(' ')).toCaseFolded();
}

bool LibraryView::containsAnyNeedle(const QString &haystack, const QStringList &needles)
{
    return std::any_of(needles.cbegin(), needles.cend(), [&haystack](const QString &needle) {
        return haystack.contains(needle, Qt::CaseInsensitive);
    });
}

bool LibraryView::containsAnyWord(const QString &haystack, const QStringList &words)
{
    for (const QString &word : words) {
        const QRegularExpression re(QStringLiteral("(^|[^\\p{L}\\p{N}])%1([^\\p{L}\\p{N}]|$)").arg(QRegularExpression::escape(word)),
                                    QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
        if (haystack.contains(re)) {
            return true;
        }
    }
    return false;
}

bool LibraryView::isTextbookEntry(const ShelfEntry &entry)
{
    const QString haystack = smartShelfContentHaystack(entry);
    return containsAnyWord(haystack, {QStringLiteral("textbook"), QStringLiteral("textbooks"), QStringLiteral("handbook"), QStringLiteral("manual"), QStringLiteral("atlas")})
        || containsAnyNeedle(haystack,
                             {QStringLiteral("lecture notes"),
                              QStringLiteral("course notes"),
                              QStringLiteral("/textbook"),
                              QStringLiteral("/textbooks"),
                              QStringLiteral("pathoma"),
                              QStringLiteral("first aid"),
                              QStringLiteral("fundamentals of "),
                              QStringLiteral("principles of "),
                              QStringLiteral("introduction to ")});
}

bool LibraryView::isMedicineEntry(const ShelfEntry &entry)
{
    const QString haystack = smartShelfContentHaystack(entry);
    return isMndEntry(entry) || isPsychiatryEntry(entry)
        || containsAnyNeedle(haystack,
                             {QStringLiteral("medicine"),
                              QStringLiteral("medical"),
                              QStringLiteral("clinical"),
                              QStringLiteral("diagnos"),
                              QStringLiteral("treatment"),
                              QStringLiteral("therapy"),
                              QStringLiteral("patient"),
                              QStringLiteral("anatomy"),
                              QStringLiteral("physiology"),
                              QStringLiteral("pathology"),
                              QStringLiteral("pharmacology"),
                              QStringLiteral("neuroscience"),
                              QStringLiteral("neurology"),
                              QStringLiteral("neuroanatomy"),
                              QStringLiteral("paediatric"),
                              QStringLiteral("pediatric"),
                              QStringLiteral("obstetric"),
                              QStringLiteral("gynecology"),
                              QStringLiteral("gynaecology")});
}

bool LibraryView::isMndEntry(const ShelfEntry &entry)
{
    const QString haystack = smartShelfContentHaystack(entry);
    return containsAnyWord(haystack, {QStringLiteral("mnd"), QStringLiteral("als")})
        || containsAnyNeedle(haystack,
                             {QStringLiteral("motor neurone"),
                              QStringLiteral("motor neuron"),
                              QStringLiteral("amyotrophic lateral sclerosis"),
                              QStringLiteral("neurodegeneration"),
                              QStringLiteral("neurodegenerative")});
}

bool LibraryView::isPsychiatryEntry(const ShelfEntry &entry)
{
    const QString haystack = smartShelfContentHaystack(entry);
    if (containsAnyNeedle(haystack,
                          {QStringLiteral("great depression"),
                           QStringLiteral("world war"),
                           QStringLiteral("america that went to war"),
                           QStringLiteral("went to war"),
                           QStringLiteral("submarine warfare"),
                           QStringLiteral("military history"),
                           QStringLiteral("wwii"),
                           QStringLiteral("robert caro"),
                           QStringLiteral("robert a. caro"),
                           QStringLiteral("path to power"),
                           QStringLiteral("means of ascent"),
                           QStringLiteral("master of the senate"),
                           QStringLiteral("passage of power"),
                           QStringLiteral("years of lyndon johnson"),
                           QStringLiteral("lyndon b. johnson"),
                           QStringLiteral("presidential biography")})) {
        return false;
    }
    return containsAnyNeedle(haystack,
                             {QStringLiteral("psychiat"),
                              QStringLiteral("mental health"),
                              QStringLiteral("clinical depression"),
                              QStringLiteral("major depression"),
                              QStringLiteral("major depressive"),
                              QStringLiteral("depressive disorder"),
                              QStringLiteral("postpartum depression"),
                              QStringLiteral("anxiety"),
                              QStringLiteral("bipolar"),
                              QStringLiteral("schizophrenia"),
                              QStringLiteral("psychosis"),
                              QStringLiteral("suicide"),
                              QStringLiteral("substance use"),
                              QStringLiteral("addiction"),
                              QStringLiteral("adhd"),
                              QStringLiteral("autism"),
                              QStringLiteral("ptsd"),
                              QStringLiteral("personality disorder")});
}

bool LibraryView::isAnthropologyEntry(const ShelfEntry &entry)
{
    const QString haystack = smartShelfContentHaystack(entry);
    return containsAnyNeedle(haystack,
                             {QStringLiteral("anthropolog"),
                              QStringLiteral("ethnograph"),
                              QStringLiteral("archaeolog"),
                              QStringLiteral("kinship"),
                              QStringLiteral("david graeber"),
                              QStringLiteral("graeber"),
                              QStringLiteral("bullshit jobs"),
                              QStringLiteral("dawn of everything"),
                              QStringLiteral("debt and exchange")});
}

bool LibraryView::isPoliticsEntry(const ShelfEntry &entry)
{
    const QString haystack = smartShelfContentHaystack(entry);
    return containsAnyNeedle(haystack,
                             {QStringLiteral("politics"),
                              QStringLiteral("political"),
                              QStringLiteral("congress"),
                              QStringLiteral("democracy"),
                              QStringLiteral("government"),
                              QStringLiteral("public policy"),
                              QStringLiteral("presidential biography"),
                              QStringLiteral("robert caro"),
                              QStringLiteral("robert a. caro"),
                              QStringLiteral("lyndon johnson"),
                              QStringLiteral("lyndon b. johnson"),
                              QStringLiteral("years of lyndon johnson"),
                              QStringLiteral("path to power"),
                              QStringLiteral("means of ascent"),
                              QStringLiteral("master of the senate"),
                              QStringLiteral("passage of power")});
}

bool LibraryView::isWorkEntry(const ShelfEntry &entry)
{
    const QString haystack = smartShelfContentHaystack(entry);
    return containsAnyNeedle(haystack,
                             {QStringLiteral("beyond bayes"),
                              QStringLiteral("highdimensional"),
                              QStringLiteral("high-dimensional"),
                              QStringLiteral("high dimensional"),
                              QStringLiteral("information geometry"),
                              QStringLiteral("bayesian"),
                              QStringLiteral("peer review"),
                              QStringLiteral("reviewer comments"),
                              QStringLiteral("major revisions"),
                              QStringLiteral("manuscript review"),
                              QStringLiteral("referee report")});
}

bool LibraryView::isFictionEntry(const ShelfEntry &entry)
{
    const QString haystack = smartShelfContentHaystack(entry);
    if (containsAnyNeedle(haystack, {QStringLiteral("nonfiction"), QStringLiteral("non-fiction"), QStringLiteral("non fiction")})) {
        return false;
    }
    return containsAnyNeedle(haystack,
                             {QStringLiteral("game of thrones"),
                              QStringLiteral("song of ice and fire"),
                              QStringLiteral("george r. r. martin"),
                              QStringLiteral("george rr martin"),
                              QStringLiteral("novel"),
                              QStringLiteral("fiction"),
                              QStringLiteral("fantasy")});
}

bool LibraryView::isNonfictionEntry(const ShelfEntry &entry)
{
    if (isFictionEntry(entry)) {
        return false;
    }
    const QString haystack = smartShelfContentHaystack(entry);
    return isAnthropologyEntry(entry) || isPoliticsEntry(entry)
        || containsAnyNeedle(haystack,
                             {QStringLiteral("robert caro"),
                              QStringLiteral("robert a. caro"),
                              QStringLiteral("lyndon johnson"),
                              QStringLiteral("lyndon b. johnson"),
                              QStringLiteral("lbj"),
                              QStringLiteral("years of lyndon johnson"),
                              QStringLiteral("path to power"),
                              QStringLiteral("means of ascent"),
                              QStringLiteral("master of the senate"),
                              QStringLiteral("passage of power"),
                              QStringLiteral("nonfiction"),
                              QStringLiteral("non-fiction"),
                              QStringLiteral("biography"),
                              QStringLiteral("history"),
                              QStringLiteral("world war"),
                              QStringLiteral("went to war"),
                              QStringLiteral("america that went to war"),
                              QStringLiteral("conservation"),
                              QStringLiteral("ecology"),
                              QStringLiteral("environment"),
                              QStringLiteral("almanac"),
                              QStringLiteral("aldo leopold"),
                              QStringLiteral("memoir"),
                              QStringLiteral("essay"),
                              QStringLiteral("science")});
}

QString LibraryView::focusTagFor(const ShelfEntry &entry)
{
    const QString haystack = smartShelfContentHaystack(entry);
    if (isMndEntry(entry)) {
        return QStringLiteral("MND Project");
    }
    if (isPsychiatryEntry(entry)) {
        return QStringLiteral("Psychiatry");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("paediatric"), QStringLiteral("pediatric"), QStringLiteral("neonat"), QStringLiteral("childhood"), QStringLiteral("adolescent")})) {
        return QStringLiteral("Paeds");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("obstetric"), QStringLiteral("gynecology"), QStringLiteral("gynaecology"), QStringLiteral("pregnancy"), QStringLiteral("maternal")})) {
        return QStringLiteral("OBGYN");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("neuroscience"), QStringLiteral("neurology"), QStringLiteral("neuroanatomy"), QStringLiteral("neural")})) {
        return QStringLiteral("Neuroscience");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("statistics"), QStringLiteral("statistical"), QStringLiteral("mathematics"), QStringLiteral("probability"), QStringLiteral("regression"), QStringLiteral("methods")})) {
        return QStringLiteral("Methods & Statistics");
    }
    if (isWorkEntry(entry) && containsAnyNeedle(haystack, {QStringLiteral("beyond bayes"), QStringLiteral("bayesian"), QStringLiteral("information geometry"), QStringLiteral("high-dimensional"), QStringLiteral("high dimensional")})) {
        return QStringLiteral("Beyond Bayes");
    }
    if (isWorkEntry(entry)) {
        return QStringLiteral("Peer Review");
    }
    if (isFictionEntry(entry)) {
        return QStringLiteral("Fiction");
    }
    if (isPoliticsEntry(entry)) {
        return QStringLiteral("Politics");
    }
    if (isAnthropologyEntry(entry)) {
        return QStringLiteral("Anthropology");
    }
    if (isNonfictionEntry(entry)) {
        return QStringLiteral("Non-fiction");
    }
    for (const QString &tag : entry.tags) {
        const QString trimmed = tag.trimmed();
        if (!trimmed.isEmpty()) {
            return trimmed;
        }
    }
    return publicationTypeTitle(entry);
}

QString LibraryView::displaySubjectForTile(const ShelfEntry &entry, const EpubCover::Metadata *epubMetadata)
{
    QStringList fields;
    fields << entry.title << entry.description << entry.url.fileName();
    if (entry.url.isLocalFile()) {
        fields << entry.url.toLocalFile();
    }
    fields << entry.detailLines;
    if (epubMetadata) {
        fields << epubMetadata->title << epubMetadata->creators << epubMetadata->year << epubMetadata->description;
    }
    const QString haystack = fields.join(QLatin1Char(' ')).toCaseFolded();
    ShelfEntry contentEntry = entry;
    contentEntry.tags.clear();
    contentEntry.keywords.clear();

    if (containsAnyNeedle(haystack, {QStringLiteral("amyotrophic lateral sclerosis"), QStringLiteral("motor neurone"), QStringLiteral("motor neuron")})
        || containsAnyWord(haystack, {QStringLiteral("mnd"), QStringLiteral("als")})) {
        return QStringLiteral("MND / ALS");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("beyond bayes"), QStringLiteral("high-dimensional"), QStringLiteral("high dimensional"), QStringLiteral("information geometry")})) {
        return QStringLiteral("Beyond Bayes");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("peer review"), QStringLiteral("referee report"), QStringLiteral("reviewer comments")})) {
        return QStringLiteral("Peer review");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("psychiat"), QStringLiteral("mental health"), QStringLiteral("psychosis"), QStringLiteral("depressive disorder")})) {
        return QStringLiteral("Psychiatry");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("paediatric"), QStringLiteral("pediatric"), QStringLiteral("neonat"), QStringLiteral("adolescent")})) {
        return QStringLiteral("Paediatrics");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("obstetric"), QStringLiteral("gynecology"), QStringLiteral("gynaecology"), QStringLiteral("pregnancy")})) {
        return QStringLiteral("OBGYN");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("medicine for mountaineer"), QStringLiteral("wilderness medicine"), QStringLiteral("wilderness activities")})) {
        return QStringLiteral("Wilderness medicine");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("submarine"), QStringLiteral("war at sea"), QStringLiteral("naval warfare"), QStringLiteral("naval history")})) {
        return QStringLiteral("Naval history");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("world war ii"), QStringLiteral("wwii"), QStringLiteral("world war 2"), QStringLiteral("america that went to war")})) {
        return QStringLiteral("World War II");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("world war i"), QStringLiteral("world war 1"), QStringLiteral("wwi")})) {
        return QStringLiteral("World War I");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("alexei yurchak"), QStringLiteral("late socialism"), QStringLiteral("post-soviet"), QStringLiteral("soviet")})) {
        return QStringLiteral("Soviet anthropology");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("david graeber"), QStringLiteral("graeber"), QStringLiteral("anthropolog"), QStringLiteral("ethnograph"), QStringLiteral("archaeolog")})) {
        return QStringLiteral("Anthropology");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("robert caro"), QStringLiteral("robert a. caro"), QStringLiteral("lyndon johnson"), QStringLiteral("lyndon b. johnson"), QStringLiteral("robert moses"), QStringLiteral("power broker")})) {
        return QStringLiteral("Political biography");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("aldo leopold"), QStringLiteral("ecology"), QStringLiteral("conservation"), QStringLiteral("environment")})) {
        return QStringLiteral("Ecology");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("game of thrones"), QStringLiteral("song of ice and fire"), QStringLiteral("george r. r. martin"), QStringLiteral("george rr martin"), QStringLiteral("fantasy")})) {
        return QStringLiteral("Fantasy");
    }
    if (containsAnyNeedle(haystack, {QStringLiteral("kim stanley robinson"), QStringLiteral("octavia butler"), QStringLiteral("ursula k. le guin"), QStringLiteral("ursula le guin"), QStringLiteral("science fiction"), QStringLiteral("sci-fi")})) {
        return QStringLiteral("Science fiction");
    }
    if (isMedicineEntry(contentEntry)) {
        return QStringLiteral("Medicine");
    }
    if (isPoliticsEntry(contentEntry)) {
        return QStringLiteral("Politics");
    }
    if (isAnthropologyEntry(contentEntry)) {
        return QStringLiteral("Anthropology");
    }
    if (isFictionEntry(contentEntry)) {
        return QStringLiteral("Fiction");
    }
    if (isNonfictionEntry(contentEntry)) {
        return QStringLiteral("Non-fiction");
    }
    return QString();
}

QStringList LibraryView::displayTagsForTile(const ShelfEntry &entry, const EpubCover::Metadata *epubMetadata)
{
    QStringList displayTags;
    auto appendUnique = [&displayTags](const QString &tag) {
        const QString trimmed = tag.simplified();
        if (trimmed.isEmpty()) {
            return;
        }
        const QString key = compactPublicationTypeKey(trimmed);
        for (const QString &existing : std::as_const(displayTags)) {
            if (compactPublicationTypeKey(existing) == key) {
                return;
            }
        }
        displayTags.append(trimmed);
    };

    const QString firstCreator = epubMetadata ? firstCreatorForDisplay(epubMetadata->creators) : QString();
    if (entry.format == QLatin1String("EPUB")) {
        appendUnique(compactMetadataLine(firstCreator));
        appendUnique(compactMetadataLine(displaySubjectForTile(entry, epubMetadata)));
    }

    for (const QString &tag : entry.tags) {
        if (!isDisplayOnlyGenericTag(tag)) {
            appendUnique(compactMetadataLine(tag));
        }
    }

    if (displayTags.isEmpty()) {
        const QString focus = focusTagFor(entry);
        if (!isDisplayOnlyGenericTag(focus)) {
            appendUnique(compactMetadataLine(focus));
        }
    }
    return displayTags.mid(0, 2);
}

void LibraryView::enrichShelfEntry(ShelfEntry &entry, const EpubCover::Metadata *epubMetadata)
{
    if (epubMetadata && !epubMetadata->title.isEmpty()) {
        const QString metadataTitle = cleanedLocalTitle(epubMetadata->title, entry.url);
        if (!metadataTitle.isEmpty()
            && (titleNeedsGeneratedMetadata(entry.title, entry.url) || metadataTitle.size() > entry.title.simplified().size() + 8)) {
            entry.title = metadataTitle;
        }
    }

    if (entry.description.isEmpty() && epubMetadata && !epubMetadata->description.isEmpty()) {
        entry.description = epubMetadata->description.simplified();
    }

    QStringList tags;
    auto appendUnique = [&tags](const QString &tag) {
        const QString trimmed = tag.trimmed();
        if (trimmed.isEmpty()) {
            return;
        }
        const QString key = compactPublicationTypeKey(trimmed);
        for (const QString &existing : std::as_const(tags)) {
            if (compactPublicationTypeKey(existing) == key) {
                return;
            }
        }
        tags.append(trimmed);
    };
    auto prependUnique = [&tags](const QString &tag) {
        const QString trimmed = tag.trimmed();
        if (trimmed.isEmpty()) {
            return;
        }
        const QString key = compactPublicationTypeKey(trimmed);
        for (int i = 0; i < tags.size(); ++i) {
            if (compactPublicationTypeKey(tags.at(i)) == key) {
                tags.move(i, 0);
                return;
            }
        }
        tags.prepend(trimmed);
    };

    for (const QString &tag : std::as_const(entry.tags)) {
        appendUnique(tag);
    }

    ShelfEntry probe = entry;
    probe.tags.clear();
    probe.keywords.clear();
    const QString haystack = smartShelfContentHaystack(probe);

    QString focus;
    if (isMndEntry(probe)) {
        focus = QStringLiteral("MND Project");
    } else if (isPsychiatryEntry(probe)) {
        focus = QStringLiteral("Psychiatry");
    } else if (containsAnyNeedle(haystack, {QStringLiteral("paediatric"), QStringLiteral("pediatric"), QStringLiteral("neonat"), QStringLiteral("childhood"), QStringLiteral("adolescent")})) {
        focus = QStringLiteral("Paeds");
    } else if (containsAnyNeedle(haystack, {QStringLiteral("obstetric"), QStringLiteral("gynecology"), QStringLiteral("gynaecology"), QStringLiteral("pregnancy"), QStringLiteral("maternal")})) {
        focus = QStringLiteral("OBGYN");
    } else if (containsAnyNeedle(haystack, {QStringLiteral("neuroscience"), QStringLiteral("neurology"), QStringLiteral("neuroanatomy"), QStringLiteral("neural")})) {
        focus = QStringLiteral("Neuroscience");
    } else if (containsAnyNeedle(haystack, {QStringLiteral("statistics"), QStringLiteral("statistical"), QStringLiteral("mathematics"), QStringLiteral("probability"), QStringLiteral("regression"), QStringLiteral("methods")})) {
        focus = QStringLiteral("Methods & Statistics");
    } else if (isWorkEntry(probe)
               && containsAnyNeedle(haystack, {QStringLiteral("beyond bayes"), QStringLiteral("bayesian"), QStringLiteral("information geometry"), QStringLiteral("high-dimensional"), QStringLiteral("high dimensional")})) {
        focus = QStringLiteral("Beyond Bayes");
    } else if (isWorkEntry(probe)) {
        focus = QStringLiteral("Peer Review");
    } else if (isFictionEntry(probe)) {
        focus = QStringLiteral("Fiction");
    } else if (isPoliticsEntry(probe)) {
        focus = QStringLiteral("Politics");
    } else if (isAnthropologyEntry(probe)) {
        focus = QStringLiteral("Anthropology");
    } else if (isNonfictionEntry(probe)) {
        focus = QStringLiteral("Non-fiction");
    } else if (isMedicineEntry(probe)) {
        focus = QStringLiteral("Medicine");
    }

    auto tagSupportedByFocus = [&focus](const QString &tag) {
        const QString key = compactPublicationTypeKey(tag);
        if (isPublicationTypeKey(key)) {
            return true;
        }
        if (key == QLatin1String("mnd") || key == QLatin1String("mndproject")) {
            return focus == QLatin1String("MND Project");
        }
        if (key == QLatin1String("psychiatry")) {
            return focus == QLatin1String("Psychiatry");
        }
        if (key == QLatin1String("paeds") || key == QLatin1String("paediatrics") || key == QLatin1String("pediatrics")) {
            return focus == QLatin1String("Paeds");
        }
        if (key == QLatin1String("obgyn") || key == QLatin1String("obstetricsgynaecology") || key == QLatin1String("obstetricsgynecology")) {
            return focus == QLatin1String("OBGYN");
        }
        if (key == QLatin1String("medicine")) {
            return focus == QLatin1String("Medicine") || focus == QLatin1String("MND Project") || focus == QLatin1String("Psychiatry")
                || focus == QLatin1String("Paeds") || focus == QLatin1String("OBGYN") || focus == QLatin1String("Neuroscience");
        }
        if (key == QLatin1String("neuroscience")) {
            return focus == QLatin1String("Neuroscience") || focus == QLatin1String("MND Project");
        }
        if (key == QLatin1String("methodsstatistics") || key == QLatin1String("methods")) {
            return focus == QLatin1String("Methods & Statistics") || focus == QLatin1String("Beyond Bayes");
        }
        if (key == QLatin1String("beyondbayes")) {
            return focus == QLatin1String("Beyond Bayes");
        }
        if (key == QLatin1String("peerreview")) {
            return focus == QLatin1String("Peer Review");
        }
        if (key == QLatin1String("fiction")) {
            return focus == QLatin1String("Fiction");
        }
        if (key == QLatin1String("nonfiction")) {
            return focus == QLatin1String("Non-fiction") || focus == QLatin1String("Politics") || focus == QLatin1String("Anthropology");
        }
        if (key == QLatin1String("politics")) {
            return focus == QLatin1String("Politics");
        }
        if (key == QLatin1String("anthropology")) {
            return focus == QLatin1String("Anthropology");
        }
        return true;
    };
    QStringList supportedTags;
    for (const QString &tag : std::as_const(tags)) {
        if (tagSupportedByFocus(tag)) {
            supportedTags.append(tag);
        }
    }
    tags = supportedTags;

    if (!focus.isEmpty()) {
        prependUnique(focus);
    }

    QString type;
    if (isTextbookEntry(probe)) {
        type = QStringLiteral("Textbook");
    } else if (focus == QLatin1String("Peer Review") || containsAnyNeedle(haystack, {QStringLiteral("referee report"), QStringLiteral("reviewer comments"), QStringLiteral("peer review")})) {
        type = QStringLiteral("Peer Review");
    } else if (entry.format == QLatin1String("EPUB") || isFictionEntry(probe) || isNonfictionEntry(probe)) {
        type = QStringLiteral("Book");
    } else if (containsAnyNeedle(haystack, {QStringLiteral("guideline"), QStringLiteral("guidelines"), QStringLiteral("clinical practice guideline")})) {
        type = QStringLiteral("Guidelines");
    } else if (entry.format == QLatin1String("PDF")) {
        type = QStringLiteral("Paper");
    }
    appendUnique(type);
    entry.tags = tags;

    if (entry.description.isEmpty()) {
        if (focus == QLatin1String("MND Project")) {
            entry.description = QStringLiteral("MND / ALS research and MD project material");
        } else if (focus == QLatin1String("Psychiatry")) {
            entry.description = QStringLiteral("Psychiatry reading");
        } else if (focus == QLatin1String("Paeds")) {
            entry.description = QStringLiteral("Paediatrics rotation reading");
        } else if (focus == QLatin1String("OBGYN")) {
            entry.description = QStringLiteral("OBGYN rotation reading");
        } else if (focus == QLatin1String("Neuroscience")) {
            entry.description = QStringLiteral("Neuroscience and neurology");
        } else if (focus == QLatin1String("Methods & Statistics")) {
            entry.description = QStringLiteral("Methods and statistics");
        } else if (focus == QLatin1String("Beyond Bayes")) {
            entry.description = QStringLiteral("Beyond Bayes and revision work");
        } else if (focus == QLatin1String("Peer Review")) {
            entry.description = QStringLiteral("Peer review queue");
        } else if (focus == QLatin1String("Anthropology")) {
            entry.description = QStringLiteral("Anthropology and social theory");
        } else if (focus == QLatin1String("Politics")) {
            entry.description = QStringLiteral("Politics, biography, and history");
        } else if (type == QLatin1String("Textbook")) {
            entry.description = QStringLiteral("Textbook");
        }
    }

    appendUnique(focus);
    appendUnique(type);
    for (const QString &tag : std::as_const(tags)) {
        if (!entry.keywords.contains(tag, Qt::CaseInsensitive)) {
            entry.keywords.append(tag);
        }
    }
}

bool LibraryView::shelfHasReadingProgress(const QList<ShelfEntry> &entries)
{
    return std::any_of(entries.cbegin(), entries.cend(), [](const ShelfEntry &entry) {
        return entry.progress >= 0.0;
    });
}

QList<LibraryView::ShelfEntry> LibraryView::loadStarterPackEntries()
{
    QList<ShelfEntry> entries;
    const QDir root(starterPackDir());
    QFile catalog(root.filePath(QStringLiteral("catalog.jsonl")));
    if (!catalog.open(QIODevice::ReadOnly)) {
        ShelfEntry setup;
        setup.title = i18nc("@title starter pack setup tile", "Install Starter Pack");
        setup.description = i18nc("@info starter pack setup tile", "Public-domain EPUBs can be installed locally.");
        setup.format = i18nc("@label generated tile cover", "SETUP");
        setup.tags = {i18nc("@label starter pack tile tag", "Starter Pack"), i18nc("@label starter pack tile tag", "Setup")};
        setup.detailLines = {i18nc("@info starter pack setup detail", "Run scripts/dev/fetch-public-domain-starter.sh from the PaperLibrary checkout."),
                             i18nc("@info starter pack setup detail", "Set StarterPackPath if you install the catalog somewhere else.")};
        entries.append(setup);
        return entries;
    }

    while (!catalog.atEnd()) {
        const QByteArray line = catalog.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) {
            continue;
        }
        const QJsonObject object = document.object();
        const QString relativePath = object.value(QLatin1String("epub_path")).toString().trimmed();
        if (relativePath.isEmpty()) {
            continue;
        }
        const QString filePath = root.filePath(relativePath);
        if (!QFileInfo::exists(filePath)) {
            continue;
        }

        ShelfEntry entry;
        entry.url = QUrl::fromLocalFile(filePath);
        entry.title = object.value(QLatin1String("title")).toString().trimmed();
        const QString authors = object.value(QLatin1String("authors")).toString().trimmed();
        const QString year = object.value(QLatin1String("year")).toString().trimmed();
        entry.description = authors.isEmpty() ? year : (year.isEmpty() ? authors : authors + QStringLiteral(" · ") + year);
        entry.format = QStringLiteral("EPUB");
        entry.lastOpened = QDateTime::fromString(object.value(QLatin1String("added_ts")).toString(), Qt::ISODate);
        entry.tags = {QStringLiteral("Starter Pack"), QStringLiteral("Public Domain")};
        const QJsonArray tags = object.value(QLatin1String("tags")).toArray();
        for (const QJsonValue &tag : tags) {
            const QString value = tag.toString().trimmed();
            if (!value.isEmpty() && !entry.tags.contains(value, Qt::CaseInsensitive)) {
                entry.tags.append(value);
            }
        }
        entry.keywords = entry.tags;
        const QString sourceUrl = object.value(QLatin1String("source_url")).toString().trimmed();
        const QString source = object.value(QLatin1String("source")).toString().trimmed();
        const QString sourceId = object.value(QLatin1String("source_id")).toString().trimmed();
        const QString rights = object.value(QLatin1String("rights")).toString().trimmed();
        if (!source.isEmpty()) {
            entry.detailLines.append(sourceId.isEmpty() ? i18nc("@info starter pack source", "Source: %1", source)
                                                        : i18nc("@info starter pack source", "Source: %1 (%2)", source, sourceId));
        }
        if (!rights.isEmpty()) {
            entry.detailLines.append(i18nc("@info starter pack rights", "Rights: %1", rights));
        }
        if (!sourceUrl.isEmpty()) {
            entry.detailLines.append(i18nc("@info starter pack source url", "Source URL: %1", sourceUrl));
        }
        if (!sourceUrl.isEmpty()) {
            entry.keywords.append(sourceUrl);
        }
        if (!rights.isEmpty()) {
            entry.keywords.append(rights);
        }
        enrichShelfEntry(entry);
        entries.append(entry);
    }

    if (entries.isEmpty()) {
        ShelfEntry setup;
        setup.title = i18nc("@title starter pack setup tile", "Starter Pack Needs Files");
        setup.description = i18nc("@info starter pack setup tile", "The starter-pack catalog exists, but no EPUB files were found.");
        setup.format = i18nc("@label generated tile cover", "SETUP");
        setup.tags = {i18nc("@label starter pack tile tag", "Starter Pack"), i18nc("@label starter pack tile tag", "Setup")};
        setup.detailLines = {i18nc("@info starter pack setup detail", "Re-run scripts/dev/fetch-public-domain-starter.sh."),
                             i18nc("@info starter pack setup detail", "Expected catalog: %1", root.filePath(QStringLiteral("catalog.jsonl")))};
        entries.append(setup);
    }

    std::stable_sort(entries.begin(), entries.end(), [](const ShelfEntry &left, const ShelfEntry &right) {
        return left.title.localeAwareCompare(right.title) < 0;
    });
    return entries;
}

QStandardItemModel *LibraryView::modelForShelf(Shelf shelf) const
{
    switch (shelf) {
    case BooksShelf:
        return m_booksModel;
    case TextbooksShelf:
        return m_textbooksModel;
    case MedicineShelf:
        return m_medicineModel;
    case MndShelf:
        return m_mndModel;
    case WorkShelf:
        return m_workModel;
    case FictionShelf:
        return m_fictionModel;
    case NonfictionShelf:
        return m_nonfictionModel;
    case StarterPackShelf:
        return m_starterPackModel;
    case FinishedShelf:
        return m_finishedModel;
    case PdfShelf:
    case PapersShelf:
        break;
    }
    return m_pdfModel;
}

void LibraryView::addShelfTab(Shelf shelf, const QString &label)
{
    if (!m_shelfSwitch || m_visibleShelves.contains(shelf)) {
        return;
    }
    m_visibleShelves.append(shelf);
    m_shelfSwitch->addTab(label);
}

int LibraryView::tabIndexForShelf(Shelf shelf) const
{
    return m_visibleShelves.indexOf(shelf);
}

void LibraryView::populate(QStandardItemModel *model, const QList<ShelfEntry> &entries, ViewMode mode)
{
    populate(PdfShelf, model, entries, mode);
}

void LibraryView::populate(Shelf shelf, QStandardItemModel *model, const QList<ShelfEntry> &entries, ViewMode mode)
{
    populateSections(shelf, model, arrangeSections(entries, mode));
}

void LibraryView::populateSections(Shelf shelf, QStandardItemModel *model, const QList<Section> &sections)
{
    model->clear();

    QList<ShelfEntry> flatEntries;
    for (const Section &section : sections) {
        for (const ShelfEntry &entry : section.entries) {
            flatEntries.append(entry);
        }
    }

    int limit = flatEntries.size();
    if (shelf >= PdfShelf && shelf < DocumentShelfCount) {
        if (m_documentShelfRowLimit[shelf] <= 0) {
            m_documentShelfRowLimit[shelf] = InitialDocumentShelfRows;
        }
        limit = qMin(flatEntries.size(), m_documentShelfRowLimit[shelf]);
    }

    for (int row = 0; row < limit; ++row) {
        model->appendRow(makeTileItem(flatEntries.at(row)));
    }

    model->setProperty("paperlibraryShelf", static_cast<int>(shelf));
    model->setProperty("paperlibraryTotalRows", flatEntries.size());
    model->setProperty("paperlibraryRenderedRows", limit);
    model->setProperty("paperlibraryHasMoreRows", limit < flatEntries.size());
}

QList<LibraryView::ShelfEntry> LibraryView::displayEntriesForShelf(Shelf shelf) const
{
    if (shelf < PdfShelf || shelf >= DocumentShelfCount) {
        return {};
    }

    const QString query = searchQuery();
    if (!query.isEmpty()) {
        QList<ShelfEntry> matches;
        for (const ShelfEntry &entry : std::as_const(m_shelfEntries[shelf])) {
            if (matchesQuery(entry, query)) {
                matches.append(entry);
            }
        }
        return matches;
    }

    if (shelf == PdfShelf && m_viewModes[shelf] == FrequentMode) {
        return m_shelfEntries[shelf];
    }

    QList<ShelfEntry> entries;
    const QList<Section> sections = arrangeSections(m_shelfEntries[shelf], m_viewModes[shelf]);
    for (const Section &section : sections) {
        entries.append(section.entries);
    }
    return entries;
}

void LibraryView::appendMoreDocumentShelfRows(Shelf shelf)
{
    if (shelf < PdfShelf || shelf >= DocumentShelfCount) {
        return;
    }

    QStandardItemModel *const model = modelForShelf(shelf);
    if (!model || m_grid->model() != model) {
        return;
    }

    const int rendered = model->property("paperlibraryRenderedRows").toInt();
    const int total = model->property("paperlibraryTotalRows").toInt();
    if (rendered >= total) {
        return;
    }

    const QList<ShelfEntry> entries = displayEntriesForShelf(shelf);
    const int nextLimit = qMin(entries.size(), qMax(rendered + DocumentShelfFetchBatchRows, rendered + 1));
    if (nextLimit <= rendered) {
        return;
    }

    m_documentShelfRowLimit[shelf] = nextLimit;
    for (int row = rendered; row < nextLimit; ++row) {
        model->appendRow(makeTileItem(entries.at(row)));
    }

    model->setProperty("paperlibraryTotalRows", entries.size());
    model->setProperty("paperlibraryRenderedRows", nextLimit);
    model->setProperty("paperlibraryHasMoreRows", nextLimit < entries.size());
}

void LibraryView::maybeFetchMoreRowsForActiveShelf()
{
    if (!m_grid || m_fetchingMoreRows) {
        return;
    }

    QScrollBar *const bar = m_grid->verticalScrollBar();
    if (!bar) {
        return;
    }
    const bool hasMoreRows = m_grid->model() && m_grid->model()->property("paperlibraryHasMoreRows").toBool();
    if (bar->maximum() <= 0) {
        if (!hasMoreRows) {
            return;
        }
    } else {
        const int remaining = bar->maximum() - bar->value();
        if (remaining > qMax(bar->pageStep(), DocumentShelfFetchThresholdPx)) {
            return;
        }
    }

    m_fetchingMoreRows = true;
    const Shelf shelf = activeShelf();
    if (PaperLibrarySectionedModel *sections = activePaperSections(); sections && usesCorpusList(shelf)) {
        if (sections->canFetchMore()) {
            sections->fetchMore();
            requestCorpusCovers();
        }
    } else if (isDocumentShelf(shelf)) {
        appendMoreDocumentShelfRows(shelf);
    }
    m_fetchingMoreRows = false;
}

LibraryView::Shelf LibraryView::activeShelf() const
{
    const int index = m_shelfSwitch->currentIndex();
    if (index >= 0 && index < m_visibleShelves.size()) {
        return m_visibleShelves.at(index);
    }
    return PdfShelf;
}

QString LibraryView::searchQuery() const
{
    return m_searchField->text().trimmed();
}

void LibraryView::setSearchQuery(const QString &query)
{
    m_searchField->setText(query); // textChanged applies the filter
}

bool LibraryView::showShelf(Shelf shelf)
{
    const int target = tabIndexForShelf(shelf);
    if (!m_shelfSwitch || target < 0) {
        return false;
    }
    m_shelfSwitch->setCurrentIndex(target);
    return true;
}

static QString compactModeText(const QString &label, const QString &detail)
{
    const QString cleanLabel = label.trimmed();
    const QString cleanDetail = detail.trimmed();
    if (cleanDetail.isEmpty()) {
        return cleanLabel;
    }
    if (cleanLabel.isEmpty()) {
        return cleanDetail;
    }
    return cleanLabel + QStringLiteral(": ") + cleanDetail;
}

bool LibraryView::matchesQuery(const ShelfEntry &entry, const QString &query)
{
    const auto contains = [&query](const QString &text) { return text.contains(query, Qt::CaseInsensitive); };
    return contains(entry.title) || contains(entry.url.fileName()) || contains(entry.description) || std::any_of(entry.tags.cbegin(), entry.tags.cend(), contains) || std::any_of(entry.keywords.cbegin(), entry.keywords.cend(), contains);
}

void LibraryView::applySearch()
{
    cancelContentSearch(); // whatever was in flight answers a stale query
    rebuildShelves();
    const QString query = searchQuery();
    const bool activeCorpus = usesCorpusList(activeShelf());
    const bool fullTextCorpusSearch = activeCorpus && corpusSearchMode() == FullTextSearch && !query.isEmpty();
    if (!fullTextCorpusSearch && !query.isEmpty()) {
        clearCorpusResultMode();
    }

    for (PaperLibrarySectionedModel *sections : m_paperSections) {
        if (!sections) {
            continue;
        }
        if (sections->hasExplicitSourceRows()) {
            sections->clearExplicitSourceRows();
        }
        sections->setQuery(fullTextCorpusSearch ? QString() : query);
    }

    if (fullTextCorpusSearch) {
        PaperLibrarySectionedModel *sections = activePaperSections();
        if (!sections || !m_paperModel || !m_paperModel->isLoaded()) {
            return;
        }
        if (!m_paperModel->hasFullTextSearchIndex()) {
            showPaperNotice(i18nc("@info when full-text search is selected but not built", "Full-text index missing — run the PaperLibrary search indexer"), true);
            return;
        }
        const QList<int> resultRows = m_paperModel->fullTextSearchRows(query, 720);
        sections->setExplicitSourceRows(resultRows,
                                        i18nc("@label corpus search result group", "Full-text search"),
                                        i18nc("@title empty full-text corpus search tile", "No full-text matches"));
        setCorpusResultMode(i18nc("@label compact corpus result mode chip", "Full text"), query);
        if (resultRows.isEmpty()) {
            showPaperNotice(i18nc("@info empty full-text corpus search", "No full-text matches for: %1", query), true);
        } else if (m_paperNotice) {
            m_paperNotice->hide();
        }
        requestCorpusCovers();
        selectFirstTile();
        return;
    }

    if (!query.isEmpty()) {
        m_searchDebounce->start();
    } else if (m_paperNotice && usesCorpusList(activeShelf())) {
        clearCorpusResultMode();
        m_paperNotice->hide();
    }
}

void LibraryView::rebuildShelves()
{
    const QString query = searchQuery();
    if (query != m_documentShelfQuery) {
        m_documentShelfQuery = query;
        for (int shelf = PdfShelf; shelf < DocumentShelfCount; ++shelf) {
            m_documentShelfRowLimit[shelf] = InitialDocumentShelfRows;
        }
    }
    for (int shelf = PdfShelf; shelf < DocumentShelfCount; ++shelf) {
        QStandardItemModel *const model = modelForShelf(static_cast<Shelf>(shelf));
        if (query.isEmpty()) {
            if (shelf == PdfShelf && m_viewModes[shelf] == FrequentMode) {
                populateSections(static_cast<Shelf>(shelf), model, {{QString(), m_shelfEntries[shelf]}});
            } else {
                populate(static_cast<Shelf>(shelf), model, m_shelfEntries[shelf], m_viewModes[shelf]);
            }
            continue;
        }
        // Layer 1, on every keystroke: the entries whose metadata matches,
        // as one flat headerless list that keeps the shelf's ranking
        QList<ShelfEntry> matches;
        for (const ShelfEntry &entry : std::as_const(m_shelfEntries[shelf])) {
            if (matchesQuery(entry, query)) {
                matches.append(entry);
            }
        }
        populateSections(static_cast<Shelf>(shelf), model, {{QString(), matches}});
    }
    if (!usesCorpusList(activeShelf())) {
        selectFirstTile();
    }
}

void LibraryView::startContentSearch()
{
    const QString query = searchQuery();
    if (query.isEmpty()) {
        return;
    }
    const Shelf shelf = activeShelf();
    if (usesCorpusList(shelf)) {
        return; // the corpus searches its catalog metadata only — never Spotlight
    }

    // Spotlight's query syntax quotes the term; drop characters that would
    // escape it (asterisks are left alone and act as wildcards)
    QString sanitized = query;
    sanitized.remove(QLatin1Char('"')).remove(QLatin1Char('\\'));
    if (sanitized.isEmpty()) {
        return;
    }

    // Scope the search to the distinct folders the shelf's documents live
    // in — or just the home folder when there are too many to enumerate
    QStringList dirs;
    for (const ShelfEntry &entry : std::as_const(m_shelfEntries[shelf])) {
        if (entry.url.isLocalFile()) {
            const QString dir = QFileInfo(entry.url.toLocalFile()).absolutePath();
            if (!dirs.contains(dir)) {
                dirs.append(dir);
            }
        }
    }
    if (dirs.isEmpty()) {
        return;
    }
    if (dirs.size() > 10) {
        dirs = QStringList(QDir::homePath());
    }

    QStringList arguments;
    for (const QString &dir : std::as_const(dirs)) {
        arguments << QStringLiteral("-onlyin") << dir;
    }
    arguments << QStringLiteral("kMDItemTextContent == \"*%1*\"cd").arg(sanitized);

    m_contentSearch = new QProcess(this);
    QProcess *process = m_contentSearch;
    connect(process, &QProcess::finished, this, [this, process, query, shelf](int exitCode, QProcess::ExitStatus exitStatus) {
        process->deleteLater();
        if (m_contentSearch == process) {
            m_contentSearch = nullptr;
        }
        // Only append while the answer is still the current question
        if (exitStatus != QProcess::NormalExit || exitCode != 0 || query != searchQuery() || shelf != activeShelf()) {
            return;
        }
        const QStringList hits = QString::fromUtf8(process->readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        applyContentSearchResults(shelf, query, hits);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { // no mdfind on this platform
            process->deleteLater();
            if (m_contentSearch == process) {
                m_contentSearch = nullptr;
            }
        }
    });
    process->start(QStringLiteral("mdfind"), arguments);
}

void LibraryView::cancelContentSearch()
{
    m_searchDebounce->stop();
    if (m_contentSearch) {
        m_contentSearch->disconnect(this);
        connect(m_contentSearch, &QProcess::finished, m_contentSearch, &QObject::deleteLater);
        m_contentSearch->kill();
        m_contentSearch = nullptr;
    }
}

void LibraryView::applyContentSearchResults(Shelf shelf, const QString &query, const QStringList &hitPaths)
{
    // Canonical hit set: mdfind may report duplicates or symlinked spellings
    QSet<QString> hits;
    for (const QString &hitPath : hitPaths) {
        const QString canonical = QFileInfo(hitPath).canonicalFilePath();
        if (!canonical.isEmpty()) {
            hits.insert(canonical);
        }
    }

    QList<ShelfEntry> found;
    for (const ShelfEntry &entry : std::as_const(m_shelfEntries[shelf])) {
        if (matchesQuery(entry, query)) {
            continue; // already shown by the metadata layer
        }
        if (entry.url.isLocalFile() && hits.contains(QFileInfo(entry.url.toLocalFile()).canonicalFilePath())) {
            found.append(entry);
        }
    }
    if (found.isEmpty()) {
        return;
    }

    QStandardItemModel *const model = modelForShelf(shelf);
    for (const ShelfEntry &entry : std::as_const(found)) {
        model->appendRow(makeTileItem(entry));
    }
}

QStandardItem *LibraryView::makeTileItem(const ShelfEntry &entry)
{
    ShelfEntry displayEntry = entry;
    displayEntry.title = cleanedLocalTitle(displayEntry.title, displayEntry.url);
    enrichShelfEntry(displayEntry);
    const bool paperish = isPaperishEntryData(displayEntry.format, displayEntry.title, displayEntry.description, displayEntry.tags, displayEntry.keywords, displayEntry.detailLines);
    const QString paperCitationTitle = paperish ? citationLabelForEntryData(displayEntry.description, displayEntry.detailLines) : QString();
    const QString paperThesis = paperish ? thesisLineForPaper(displayEntry.title, displaySubjectForTile(displayEntry)) : QString();

    QStandardItem *item = new QStandardItem(displayEntry.title);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setData(displayEntry.url, UrlRole);
    item->setData(displayEntry.pinned, PinnedRole);
    item->setData(displayEntry.downranked, DownrankedRole);
    item->setData(displayEntry.finishedReading, FinishedReadingRole);
    item->setData(displayEntry.progress, ProgressRole);
    item->setData(displayEntry.format, FormatRole);
    if (!paperCitationTitle.isEmpty()) {
        item->setData(paperCitationTitle, DisplayTitleRole);
    }
    QStringList shownTags = displayEntry.tags;
    if (shownTags.isEmpty()) {
        const QString inferred = focusTagFor(displayEntry);
        if (!inferred.isEmpty()) {
            shownTags.append(inferred);
        }
    }
    item->setData(shownTags, TagsRole);
    item->setData(displayEntry.description, DescriptionRole);
    item->setAccessibleText(displayEntry.title);
    QString tooltipDescription = displayEntry.description;
    const EpubCover::Metadata *epubMetadata = nullptr;
    if (displayEntry.url.isLocalFile()) {
        const QString filePath = displayEntry.url.toLocalFile();
        if (displayEntry.format == QLatin1String("EPUB")) {
            epubMetadata = &epubMetadataFor(filePath);
            if (displayEntry.description.isEmpty() && !epubMetadata->description.isEmpty()) {
                item->setData(epubMetadata->description, DescriptionRole);
                tooltipDescription = epubMetadata->description;
                displayEntry.description = epubMetadata->description;
            }
        }
        QStringList displayTags = displayTagsForTile(displayEntry, epubMetadata);
        if (paperish && !paperThesis.isEmpty()) {
            displayTags = {paperThesis};
        }
        item->setData(displayTags, DisplayTagsRole);
        // What a typographic card would say for this entry. Books get
        // their byline, foot and fallback description from the EPUB's own
        // OPF metadata — curated store metadata, when present, wins
        CoverGenerator::CoverSpec spec {paperCitationTitle.isEmpty() ? displayEntry.title : paperCitationTitle, QString(), QString(), displayTags.value(1, displayTags.value(0, shownTags.value(0)))};
        if (epubMetadata) {
            spec.authors = epubMetadata->creators;
            spec.yearJournal = epubMetadata->year;
        }
        const QString cached = m_coverLoader->cachedCoverPath(filePath, spec);
        if (!cached.isEmpty()) {
            item->setData(QVariant::fromValue(QPixmap(cached)), CoverRole);
            item->setData(CoverLoader::isGeneratedCoverPath(cached), GeneratedCoverRole);
        } else {
            m_coverLoader->requestCover(filePath, spec);
        }
    } else {
        QStringList displayTags = displayTagsForTile(displayEntry, nullptr);
        if (paperish && !paperThesis.isEmpty()) {
            displayTags = {paperThesis};
        }
        item->setData(displayTags, DisplayTagsRole);
    }
    const QStringList displayTags = item->data(DisplayTagsRole).toStringList();
    QStringList tooltipLines;
    if (epubMetadata) {
        tooltipLines << joinCompact({epubMetadata->creators, epubMetadata->year});
        tooltipLines << displaySubjectForTile(displayEntry, epubMetadata);
    } else {
        tooltipLines << joinCompact({displayEntry.format, displayTags.mid(0, 2).join(QStringLiteral(" · "))});
    }
    if (!paperCitationTitle.isEmpty()) {
        tooltipLines << paperCitationTitle;
    }
    if (paperish && !paperThesis.isEmpty()) {
        tooltipLines << paperThesis;
    }
    if (tooltipLines.isEmpty() && !shownTags.isEmpty()) {
        tooltipLines << joinCompact({displayEntry.format, shownTags.mid(0, 2).join(QStringLiteral(" · "))});
    }
    tooltipLines << progressTooltipLine(displayEntry.progress) << tooltipDescription;
    tooltipLines.append(displayEntry.detailLines);
    item->setToolTip(libraryTileTooltip(displayEntry.title, tooltipLines));
    return item;
}

LibraryView::TileCaption LibraryView::tileCaption(const QModelIndex &index)
{
    if (index.data(PaperLibrarySectionedModel::SourceRowRole).isValid()) {
        const QPixmap corpusCover = index.data(PaperLibrarySectionedModel::CoverPixmapRole).value<QPixmap>();
        const bool generatedCardShowing = index.data(PaperLibrarySectionedModel::GeneratedCoverRole).toBool() && !corpusCover.isNull();
        if (!corpusCover.isNull() && !generatedCardShowing) {
            return {index.data(Qt::DisplayRole).toString(), false};
        }
        const auto *sections = qobject_cast<const PaperLibrarySectionedModel *>(index.model());
        const QString metadata = corpusPaperMetadataLineForIndex(index);
        const QString paperThesis = corpusPaperThesisForIndex(index);
        const QString relation = index.data(PaperLibrarySectionedModel::RelationHintRole).toString();
        const QString intent = index.data(PaperLibrarySectionedModel::ShelfIntentRole).toString();
        const QString priority = index.data(PaperLibrarySectionedModel::PriorityHintRole).toString();
        if (sections && sections->smartFilter() == PaperLibrarySectionedModel::Papers) {
            const QString paperSummary = corpusPaperSummaryForKey(corpusPaperKeyForIndex(index));
            const QString rationale = joinCompact({paperThesis, paperSummary == paperThesis ? QString() : paperSummary, relation});
            if (!rationale.isEmpty()) {
                return {rationale, true};
            }
            if (!intent.isEmpty()) {
                return {intent, true};
            }
        }
        const QString paperSummary = corpusPaperSummaryForKey(corpusPaperKeyForIndex(index));
        if (!metadata.isEmpty()) {
            QString context = paperThesis.isEmpty() ? paperSummary : paperThesis;
            if (context.isEmpty() || context == metadata) {
                context = relation;
            }
            if (context.isEmpty() || context == metadata) {
                context = intent;
            }
            if (context.isEmpty() || context == metadata) {
                context = priority;
            }
            return {joinCompact({metadata, context}), true};
        }
        if (!paperSummary.isEmpty()) {
            return {metadata.isEmpty() ? paperSummary : joinCompact({paperSummary, metadata}), true};
        }
        if (!relation.isEmpty()) {
            return {relation, true};
        }
        if (!intent.isEmpty()) {
            return {intent, true};
        }
        if (!priority.isEmpty()) {
            return {priority, true};
        }
        return {QStringList(index.data(PaperLibrarySectionedModel::TopicTagsRole).toStringList().mid(0, 2)).join(QStringLiteral(" · ")), true};
    }
    // Only a tile actually showing a generated card gives up its title
    // caption — the card displays the title as the artwork already
    const bool generatedCardShowing = index.data(GeneratedCoverRole).toBool() && !index.data(CoverRole).value<QPixmap>().isNull();
    if (!generatedCardShowing) {
        const QString displayTitle = index.data(DisplayTitleRole).toString().trimmed();
        return {displayTitle.isEmpty() ? index.data(Qt::DisplayRole).toString() : displayTitle, false};
    }
    const QString description = index.data(DescriptionRole).toString();
    if (!description.isEmpty()) {
        return {description, true};
    }
    const QStringList displayTags = index.data(DisplayTagsRole).toStringList();
    const QStringList tags = displayTags.isEmpty() ? index.data(TagsRole).toStringList() : displayTags;
    return {QStringList(tags.mid(0, 2)).join(QStringLiteral(" · ")), true};
}

void LibraryView::enrichShelfEntryFromCorpus(ShelfEntry &entry) const
{
    if (!m_paperModel || !m_paperModel->isLoaded() || !entry.url.isLocalFile()) {
        return;
    }

    const QString localPath = entry.url.toLocalFile();
    const QString canonical = QFileInfo(localPath).canonicalFilePath();
    int row = canonical.isEmpty() ? -1 : m_paperModel->rowForLookupPath(canonical);
    if (row < 0) {
        row = m_paperModel->rowForLookupPath(localPath);
    }
    if (row < 0) {
        row = m_paperModel->rowForLookupSlug(QFileInfo(localPath).completeBaseName());
    }
    if (row < 0) {
        return;
    }

    const QModelIndex index = m_paperModel->index(row, 0);
    const QString title = index.data(Qt::DisplayRole).toString().simplified();
    if (!title.isEmpty() && (titleNeedsGeneratedMetadata(entry.title, entry.url) || title.size() > entry.title.simplified().size() + 8)) {
        entry.title = title;
    }

    const QString authors = index.data(PaperLibraryModel::AuthorsRole).toString().simplified();
    const QString year = index.data(PaperLibraryModel::YearRole).toString().simplified();
    const QString journal = index.data(PaperLibraryModel::JournalRole).toString().simplified();
    const QString detail = joinCompact({authors, year, journal});
    if (!detail.isEmpty()) {
        entry.description = detail;
        entry.detailLines.append(detail);
    }

    const QString doi = index.data(PaperLibraryModel::DoiRole).toString().simplified();
    const QString source = index.data(PaperLibraryModel::SourceRole).toString().simplified();
    for (const QString &keyword : {authors, year, journal, doi, source}) {
        if (!keyword.isEmpty() && !entry.keywords.contains(keyword)) {
            entry.keywords.append(keyword);
        }
    }
}

const EpubCover::Metadata &LibraryView::epubMetadataFor(const QString &filePath)
{
    auto it = m_epubMetadata.constFind(filePath);
    if (it == m_epubMetadata.cend()) {
        it = m_epubMetadata.insert(filePath, EpubCover::metadata(filePath));
    }
    return it.value();
}

void LibraryView::setupPapersShelf()
{
    if (focusManifestExists(m_paperCorpusDir, QStringLiteral("Textbooks"))) {
        addShelfTab(TextbooksShelf, i18nc("library smart shelf with textbook documents", "Textbooks"));
    }
    if (focusManifestExists(m_paperCorpusDir, QStringLiteral("Medicine"))) {
        addShelfTab(MedicineShelf, i18nc("library smart shelf for medicine documents", "Medicine"));
    }
    if (focusManifestExists(m_paperCorpusDir, QStringLiteral("MND"))) {
        addShelfTab(MndShelf, i18nc("library smart shelf for MND project documents", "MND"));
    }
    addShelfTab(PapersShelf, i18nc("library shelf listing the PaperLibrary corpus", "Papers"));

    m_paperModel = new PaperLibraryModel(this);
    for (Shelf shelf : {TextbooksShelf, MedicineShelf, MndShelf, WorkShelf, FictionShelf, NonfictionShelf, PapersShelf}) {
        auto *sections = new PaperLibrarySectionedModel(this);
        sections->setShelf(paperFilterForShelf(shelf), static_cast<PaperLibrarySectionedModel::SectionMode>(paperSectionMode(shelf)));
        m_paperSections[shelf] = sections;
        connect(sections, &QAbstractItemModel::modelReset, this, [this, sections]() {
            QTimer::singleShot(0, this, [this, sections]() {
                if (activePaperSections() == sections) {
                    selectFirstTile();
                    requestCorpusCovers();
                }
            });
        });
    }
    connect(m_paperModel, &PaperLibraryModel::loaded, this, [this]() {
        syncCorpusSearchButton();
        refresh();
        prebuildCorpusShelves();
        showShelfGuide();
        requestCorpusCovers();
        scheduleCorpusPrewarm();
    });

    // The non-modal notice slot under the header ("Loading catalog…",
    // "PDF not local — …"); most of the time it is hidden
    m_paperNotice = new QLabel(this);
    m_paperNotice->setMargin(8);
    m_paperNotice->setTextFormat(Qt::PlainText);
    m_paperNotice->setWordWrap(false);
    m_paperNotice->hide();
    m_paperNoticeTimer = new QTimer(this);
    m_paperNoticeTimer->setSingleShot(true);
    m_paperNoticeTimer->setInterval(5000);
    connect(m_paperNoticeTimer, &QTimer::timeout, m_paperNotice, &QWidget::hide);

    QVBoxLayout *mainLayout = qobject_cast<QVBoxLayout *>(layout());
    mainLayout->insertWidget(1, m_paperNotice); // right under the header row
}

void LibraryView::shelfChanged(int index)
{
    m_pendingShelfIndex = index;
    const Shelf shelf = activeShelf();
    const bool corpus = usesCorpusList(shelf);
    m_viewModeButton->setVisible(!corpus);
    m_paperSectionButton->setVisible(corpus);
    syncCorpusSearchButton();
    syncCorpusResultButton();
    m_grid->setVisible(true);
    if (corpus) {
        syncPaperSectionButton();
    } else {
        syncViewModeButton();
        if (m_paperNotice) {
            m_paperNotice->hide();
        }
    }
    if (m_corpusCoverWarmupTimer) {
        m_corpusCoverWarmupTimer->stop();
    }
    if (corpus && m_shelfRenderTimer) {
        m_shelfRenderTimer->start();
    } else {
        renderPendingShelf();
    }
}

void LibraryView::renderPendingShelf()
{
    if (m_pendingShelfIndex < 0 || m_pendingShelfIndex >= m_visibleShelves.size() || m_pendingShelfIndex != m_shelfSwitch->currentIndex()) {
        m_pendingShelfIndex = m_shelfSwitch->currentIndex();
    }
    const Shelf shelf = activeShelf();
    const bool animate = !m_lastShelfRender.isValid() || m_lastShelfRender.elapsed() > 160;
    m_pendingShelfIndex = -1;
    renderShelf(shelf, animate);
    m_lastShelfRender.restart();
}

void LibraryView::renderShelf(Shelf shelf, bool animate)
{
    const bool corpus = usesCorpusList(shelf);
    const bool wasUpdatesEnabled = m_grid->updatesEnabled();
    m_grid->setUpdatesEnabled(false);
    if (corpus) {
        configureCorpusShelf(shelf);
        PaperLibrarySectionedModel *sections = paperSectionsForShelf(shelf);
        QItemSelectionModel *oldSelection = nullptr;
        if (m_grid->model() != sections) {
            oldSelection = m_grid->selectionModel();
            m_grid->setModel(sections);
        }
        configureTileGrid();
        m_grid->scrollToTop();
        delete oldSelection;
        connectGridSelectionContext();
        ensurePapersFresh();
        showShelfGuide();
        requestCorpusCovers();
    } else {
        QAbstractItemModel *const shelfModel = modelForShelf(shelf);
        QItemSelectionModel *oldSelection = nullptr;
        if (m_grid->model() != shelfModel) {
            oldSelection = m_grid->selectionModel();
            m_grid->setModel(shelfModel);
        }
        configureTileGrid();
        m_grid->scrollToTop();
        delete oldSelection;
        connectGridSelectionContext();
        syncViewModeButton(); // the arrangement is a per-shelf choice
        if (m_paperNotice) {
            m_paperNotice->hide();
        }
    }
    selectFirstTile();
    updateSelectedTileContext(m_grid->currentIndex());
    if (animate) {
        animateGridIn();
    } else if (m_gridFadeAnimation && m_gridFadeEffect) {
        m_gridFadeAnimation->stop();
        m_gridFadeEffect->setOpacity(1.0);
    }
    if (!searchQuery().isEmpty()) {
        applySearch(); // the content search follows the active shelf
    }
    m_grid->setUpdatesEnabled(wasUpdatesEnabled);
    m_grid->viewport()->update();
}

bool LibraryView::usesCorpusList(Shelf shelf) const
{
    return paperSectionsForShelf(shelf) != nullptr;
}

PaperLibrarySectionedModel *LibraryView::paperSectionsForShelf(Shelf shelf) const
{
    if (shelf < PdfShelf || shelf > PapersShelf) {
        return nullptr;
    }
    return m_paperSections[shelf];
}

PaperLibrarySectionedModel *LibraryView::activePaperSections() const
{
    return paperSectionsForShelf(activeShelf());
}

void LibraryView::attachCorpusShelf(Shelf shelf)
{
    PaperLibrarySectionedModel *sections = paperSectionsForShelf(shelf);
    if (!sections) {
        return;
    }
    if (!m_paperSectionAttached[shelf]) {
        sections->setSourceModel(m_paperModel);
        m_paperSectionAttached[shelf] = true;
    }
    sections->setShelf(paperFilterForShelf(shelf), static_cast<PaperLibrarySectionedModel::SectionMode>(paperSectionMode(shelf)));
}

void LibraryView::configureCorpusShelf(Shelf shelf)
{
    attachCorpusShelf(shelf);
    syncPaperSectionButton();
    syncCorpusSearchButton();
    syncCorpusResultButton();
}

void LibraryView::setPaperSectionMode(Shelf shelf, int mode)
{
    if (shelf < PdfShelf || shelf > PapersShelf || mode < PaperLibrarySectionedModel::ReadNext || mode > PaperLibrarySectionedModel::ByJournal) {
        return;
    }
    m_paperSectionModes[shelf] = mode;
    KConfigGroup config = partGeneralConfig();
    config.writeEntry(paperSectionModeConfigKey(shelf), paperSectionModeName(mode));
    config.sync();
    if (PaperLibrarySectionedModel *sections = paperSectionsForShelf(shelf); sections && activeShelf() == shelf) {
        sections->setSectionMode(static_cast<PaperLibrarySectionedModel::SectionMode>(mode));
    }
    syncPaperSectionButton();
    showShelfGuide();
    requestCorpusCovers();
}

int LibraryView::paperSectionMode(Shelf shelf) const
{
    if (shelf < PdfShelf || shelf > PapersShelf) {
        return PaperLibrarySectionedModel::ReadNext;
    }
    const int mode = m_paperSectionModes[shelf];
    if (mode < PaperLibrarySectionedModel::ReadNext || mode > PaperLibrarySectionedModel::ByJournal) {
        return defaultPaperSectionModeForShelf(shelf);
    }
    return mode;
}

void LibraryView::syncPaperSectionButton()
{
    if (!m_paperSectionButton || !activePaperSections()) {
        return;
    }
    const int mode = paperSectionMode(activeShelf());
    if (mode >= PaperLibrarySectionedModel::ReadNext && mode <= PaperLibrarySectionedModel::ByJournal && m_paperSectionActions[mode]) {
        m_paperSectionActions[mode]->setChecked(true);
        m_paperSectionButton->setText(m_paperSectionActions[mode]->text());
    }
}

LibraryView::CorpusSearchMode LibraryView::corpusSearchMode() const
{
    return m_corpusSearchMode;
}

void LibraryView::setCorpusSearchMode(CorpusSearchMode mode)
{
    if (mode < ShelfMetadataSearch || mode > FullTextSearch) {
        return;
    }
    if (mode == FullTextSearch && m_paperModel && m_paperModel->isLoaded() && !m_paperModel->hasFullTextSearchIndex()) {
        showPaperNotice(i18nc("@info when the full-text search index is unavailable", "Full-text index missing — run the PaperLibrary search indexer"), true);
        mode = ShelfMetadataSearch;
    }
    if (m_corpusSearchMode == mode) {
        syncCorpusSearchButton();
        return;
    }
    m_corpusSearchMode = mode;
    KConfigGroup config = partGeneralConfig();
    config.writeEntry("CorpusSearchMode", corpusSearchModeName(mode));
    config.sync();
    syncCorpusSearchButton();
    applySearch();
}

void LibraryView::syncCorpusSearchButton()
{
    if (!m_corpusSearchButton) {
        return;
    }
    const bool corpus = usesCorpusList(activeShelf());
    m_corpusSearchButton->setVisible(corpus);
    if (!corpus) {
        return;
    }
    const bool ftsAvailable = m_paperModel && m_paperModel->isLoaded() && m_paperModel->hasFullTextSearchIndex();
    if (m_corpusSearchActions[FullTextSearch]) {
        m_corpusSearchActions[FullTextSearch]->setEnabled(ftsAvailable);
    }
    if (m_corpusSearchMode == FullTextSearch && !ftsAvailable) {
        m_corpusSearchMode = ShelfMetadataSearch;
    }
    if (m_corpusSearchActions[m_corpusSearchMode]) {
        m_corpusSearchActions[m_corpusSearchMode]->setChecked(true);
        m_corpusSearchButton->setText(m_corpusSearchActions[m_corpusSearchMode]->text());
    }
}

void LibraryView::setCorpusResultMode(const QString &label, const QString &detail)
{
    m_corpusResultLabel = label.trimmed();
    m_corpusResultDetail = detail.trimmed();
    syncCorpusResultButton();
}

void LibraryView::clearCorpusResultMode()
{
    if (m_corpusResultLabel.isEmpty() && m_corpusResultDetail.isEmpty()) {
        return;
    }
    m_corpusResultLabel.clear();
    m_corpusResultDetail.clear();
    syncCorpusResultButton();
}

void LibraryView::syncCorpusResultButton()
{
    if (!m_corpusResultButton) {
        return;
    }
    const bool visible = usesCorpusList(activeShelf()) && (!m_corpusResultLabel.isEmpty() || !m_corpusResultDetail.isEmpty());
    m_corpusResultButton->setVisible(visible);
    if (!visible) {
        return;
    }
    const QString fullText = compactModeText(m_corpusResultLabel, m_corpusResultDetail);
    const int textWidth = qMax(120, m_corpusResultButton->maximumWidth() - 24);
    m_corpusResultButton->setText(QFontMetrics(m_corpusResultButton->font()).elidedText(fullText, Qt::ElideRight, textWidth));
    m_corpusResultButton->setToolTip(i18nc("@info:tooltip on corpus result mode chip", "%1\nClick to clear this result mode.", fullText));
}

bool LibraryView::clearActiveCorpusResult()
{
    bool cleared = false;
    for (PaperLibrarySectionedModel *sections : m_paperSections) {
        if (sections && sections->hasExplicitSourceRows()) {
            sections->clearExplicitSourceRows();
            cleared = true;
        }
    }
    if (!m_searchField->text().isEmpty()) {
        m_searchField->clear();
        cleared = true;
    }
    clearCorpusResultMode();
    if (m_paperNotice) {
        m_paperNotice->hide();
    }
    if (cleared) {
        requestCorpusCovers();
        selectFirstTile();
    }
    return cleared;
}

void LibraryView::showShelfGuide()
{
    if (!m_paperNotice) {
        return;
    }
    if (!usesCorpusList(activeShelf())) {
        m_paperNotice->hide();
        return;
    }
    if (m_paperModel && !m_paperModel->isLoaded()) {
        return; // leave the loading notice alone
    }
    // Normal shelf orientation belongs in the tiles and the grouping control,
    // not in a persistent banner that steals browsing space.
    m_paperNotice->hide();
}

void LibraryView::requestCorpusCovers()
{
    m_nextCorpusCoverRow = 0;
    if (m_corpusCoverWarmupTimer) {
        m_corpusCoverWarmupTimer->start(320);
    } else {
        requestNextCorpusCoverBatch();
    }
}

void LibraryView::requestNextCorpusCoverBatch()
{
    PaperLibrarySectionedModel *sections = activePaperSections();
    if (!sections || !m_coverLoader) {
        return;
    }
    if (m_coverLoader->pendingWorkCount() > 160) {
        m_corpusCoverWarmupTimer->start();
        return;
    }

    // Keep each pass bounded. Local PDFs are rendered from the actual file;
    // manifest thumbnails are only a fallback for rows without a local file.
    static constexpr int MaxCorpusCoverRequests = 16;
    const int rows = sections->rowCount();
    m_nextCorpusCoverRow = requestCorpusCoversForSections(sections, m_nextCorpusCoverRow, MaxCorpusCoverRequests);
    if (m_nextCorpusCoverRow < rows) {
        m_corpusCoverWarmupTimer->start();
    }
}

int LibraryView::requestCorpusCoversForSections(PaperLibrarySectionedModel *sections, int startRow, int maxRequests)
{
    if (!sections || !m_coverLoader || maxRequests <= 0) {
        return startRow;
    }

    int requested = 0;
    const int rows = sections->rowCount();
    int row = qMax(0, startRow);
    for (; row < rows && requested < maxRequests; ++row) {
        const QModelIndex index = sections->index(row);
        if (!index.isValid() || index.data(PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
            continue;
        }
        if (!index.data(PaperLibrarySectionedModel::CoverPixmapRole).value<QPixmap>().isNull()) {
            continue;
        }
        const QString coverKey = index.data(PaperLibrarySectionedModel::PdfPathRole).toString();
        if (coverKey.isEmpty()) {
            continue;
        }
        const QString pdfPath = sections->resolvePath(index);
        if (!pdfPath.isEmpty()) {
            const CoverGenerator::CoverSpec spec = corpusCoverSpecForIndex(index);
            const QString cached = m_coverLoader->cachedCoverPath(pdfPath, spec);
            if (!cached.isEmpty()) {
                sections->setCoverForPath(coverKey, QVariant::fromValue(QPixmap(cached)), CoverLoader::isGeneratedCoverPath(cached));
            } else {
                m_coverLoader->requestCover(pdfPath, spec);
            }
            ++requested;
            continue;
        }
        const QString thumbnailPath = index.data(PaperLibrarySectionedModel::ThumbnailPathRole).toString();
        if (!thumbnailPath.isEmpty()) {
            const QPixmap thumbnail(thumbnailPath);
            if (!thumbnail.isNull()) {
                sections->setCoverForPath(coverKey, QVariant::fromValue(thumbnail), false);
                ++requested;
            }
        }
    }
    return row;
}

void LibraryView::prebuildCorpusShelves()
{
    if (!m_paperModel || !m_paperModel->isLoaded()) {
        return;
    }

    for (const Shelf shelf : std::as_const(m_visibleShelves)) {
        PaperLibrarySectionedModel *sections = paperSectionsForShelf(shelf);
        if (!sections) {
            continue;
        }
        attachCorpusShelf(shelf);
        sections->rowCount(); // setSourceModel() already rebuilt rows; keep this path hot before first click.
    }
}

void LibraryView::scheduleCorpusPrewarm()
{
    if (!m_paperModel || !m_paperModel->isLoaded()) {
        return;
    }
    m_corpusPrewarmQueue.clear();
    const Shelf current = activeShelf();
    const QList<Shelf> order = {WorkShelf, PapersShelf, NonfictionShelf, FictionShelf, MedicineShelf, MndShelf, TextbooksShelf};
    if (usesCorpusList(current)) {
        m_corpusPrewarmQueue.append(current);
    }
    for (const Shelf shelf : order) {
        if (shelf != current && tabIndexForShelf(shelf) >= 0 && paperSectionsForShelf(shelf)) {
            m_corpusPrewarmQueue.append(shelf);
        }
    }
    m_corpusPrewarmIndex = 0;
    m_corpusPrewarmActive = true;
    QTimer::singleShot(0, this, &LibraryView::prewarmNextCorpusShelf);
}

void LibraryView::prewarmNextCorpusShelf()
{
    if (!m_corpusPrewarmActive) {
        return;
    }
    if (!m_paperModel || !m_paperModel->isLoaded()) {
        m_corpusPrewarmActive = false;
        return;
    }
    if (m_corpusPrewarmIndex >= m_corpusPrewarmQueue.size()) {
        m_corpusPrewarmActive = false;
        return;
    }

    const Shelf shelf = m_corpusPrewarmQueue.at(m_corpusPrewarmIndex++);
    attachCorpusShelf(shelf);
    if ((!m_coverLoader || m_coverLoader->pendingWorkCount() <= 130) && paperSectionsForShelf(shelf)) {
        PaperLibrarySectionedModel *sections = paperSectionsForShelf(shelf);
        requestCorpusCoversForSections(sections, 0, 12);
    }
    QTimer::singleShot(45, this, &LibraryView::prewarmNextCorpusShelf);
}

void LibraryView::ensurePapersFresh()
{
    if (!m_paperModel) {
        return;
    }
    if (m_paperModel->isLoaded()) {
        return; // tab switches should be instant; refresh() owns mtime checks
    } else {
        showPaperNotice(i18nc("@info while the corpus catalog parses in the background", "Loading catalog…"), false);
        m_paperModel->load(m_paperCorpusDir);
    }
}

void LibraryView::applyChromePalette()
{
    if (m_applyingChromePalette) {
        return;
    }

    m_applyingChromePalette = true;

    const QPalette source = qApp ? qApp->palette() : palette();
    const QColor body = ChromeColors::toolbar(source);
    const QColor text = ChromeColors::toolbarText(source);

    QPalette chrome = palette();
    chrome.setColor(QPalette::Window, body);
    chrome.setColor(QPalette::Base, body);
    chrome.setColor(QPalette::Button, body);
    chrome.setColor(QPalette::WindowText, text);
    chrome.setColor(QPalette::Text, text);
    chrome.setColor(QPalette::ButtonText, text);

    if (chrome != palette()) {
        setPalette(chrome);
    }
    if (m_grid) {
        m_grid->setPalette(chrome);
        m_grid->viewport()->setPalette(chrome);
        configureTileGrid();
    }
    m_applyingChromePalette = false;
}

void LibraryView::showPaperNotice(const QString &text, bool autoHide)
{
    if (!m_paperNotice) {
        return;
    }
    // A quiet palette-derived strip, styled at show time so it follows
    // light/dark appearance changes
    QPalette chip = palette();
    const QColor base = palette().color(QPalette::Base);
    const QColor textColor = palette().color(QPalette::Text);
    const bool dark = base.lightnessF() < 0.5;
    chip.setColor(QPalette::Window, blendColors(base, textColor, dark ? 0.12 : 0.045));
    QColor noticeColor = palette().color(QPalette::Text);
    noticeColor.setAlphaF(0.72);
    chip.setColor(QPalette::WindowText, noticeColor);
    m_paperNotice->setPalette(chip);
    m_paperNotice->setAutoFillBackground(true);
    const int availableWidth = m_paperNotice->width() - 20;
    m_paperNotice->setText(availableWidth > 200 ? QFontMetrics(m_paperNotice->font()).elidedText(text, Qt::ElideRight, availableWidth) : text);
    m_paperNotice->show();
    if (autoHide) {
        m_paperNoticeTimer->start();
    } else {
        m_paperNoticeTimer->stop();
    }
}

bool LibraryView::eventFilter(QObject *watched, QEvent *event)
{
    if (m_grid && watched == m_grid->viewport()) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            resetTileDrag();
            if (mouseEvent->button() == Qt::LeftButton) {
                const QModelIndex index = m_grid->indexAt(mouseEvent->position().toPoint());
                if (index.isValid() && !isLibraryHeaderIndex(index)) {
                    m_tileDragIndex = index;
                    m_tileDragPressPos = mouseEvent->position().toPoint();
                    m_tileDragCandidate = true;
                }
            }
            break;
        }
        case QEvent::MouseMove: {
            if (!m_tileDragCandidate || !(static_cast<QMouseEvent *>(event)->buttons() & Qt::LeftButton)) {
                break;
            }
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QPoint delta = mouseEvent->position().toPoint() - m_tileDragPressPos;
            if (delta.y() >= DownrankDragDistance && delta.y() > std::abs(delta.x())) {
                if (!m_tileDragArmed) {
                    m_tileDragArmed = true;
                    m_grid->viewport()->setCursor(Qt::ClosedHandCursor);
                    if (usesCorpusList(activeShelf())) {
                        showPaperNotice(i18nc("@info while dragging a library tile downward", "Release to move this lower in the feed"), false);
                    }
                }
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() != Qt::LeftButton || !m_tileDragCandidate) {
                break;
            }
            const QPoint delta = mouseEvent->position().toPoint() - m_tileDragPressPos;
            const bool shouldDownrank = m_tileDragArmed || (delta.y() >= DownrankDragDistance && delta.y() > std::abs(delta.x()));
            const QModelIndex index = QModelIndex(m_tileDragIndex);
            resetTileDrag();
            if (shouldDownrank) {
                downrankTile(index);
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::Leave:
        case QEvent::MouseButtonDblClick:
            resetTileDrag();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LibraryView::connectGridSelectionContext()
{
    if (m_gridSelectionConnection) {
        disconnect(m_gridSelectionConnection);
        m_gridSelectionConnection = {};
    }
    if (!m_grid || !m_grid->selectionModel()) {
        return;
    }
    m_gridSelectionConnection = connect(m_grid->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &current) {
        updateSelectedTileContext(current);
    });
}

void LibraryView::updateSelectedTileContext(const QModelIndex &index)
{
    if (!usesCorpusList(activeShelf()) || !m_paperNotice) {
        if (m_paperNotice) {
            m_paperNotice->hide();
        }
        return;
    }
    if (m_paperModel && !m_paperModel->isLoaded()) {
        return;
    }
    Q_UNUSED(index);
    m_paperNotice->hide();
}

bool LibraryView::downrankTile(const QModelIndex &index)
{
    if (!index.isValid() || isLibraryHeaderIndex(index)) {
        return false;
    }

    const QString title = index.data(Qt::DisplayRole).toString().trimmed();
    if (index.data(PaperLibrarySectionedModel::SourceRowRole).isValid()) {
        if (index.data(PaperLibrarySectionedModel::DownrankedRole).toBool()) {
            return false;
        }
        auto *sections = qobject_cast<PaperLibrarySectionedModel *>(const_cast<QAbstractItemModel *>(index.model()));
        if (!sections) {
            return false;
        }
        sections->setDownranked(index, true);
        showPaperNotice(title.isEmpty() ? i18nc("@info after dragging a corpus tile downward", "Moved lower in the feed")
                                        : i18nc("@info after dragging a corpus tile downward", "Moved lower in the feed: %1", title),
                        true);
        return true;
    }

    const QUrl url = index.data(UrlRole).toUrl();
    if (!url.isValid() || index.data(DownrankedRole).toBool()) {
        return false;
    }
    m_store->setDownranked(url, true);
    refresh();
    if (m_paperNotice) {
        showPaperNotice(title.isEmpty() ? i18nc("@info after dragging a library tile downward", "Moved lower in the feed")
                                        : i18nc("@info after dragging a library tile downward", "Moved lower in the feed: %1", title),
                        true);
    }
    return true;
}

void LibraryView::resetTileDrag()
{
    m_tileDragCandidate = false;
    m_tileDragArmed = false;
    m_tileDragIndex = QPersistentModelIndex();
    if (m_grid && m_grid->viewport()) {
        m_grid->viewport()->unsetCursor();
    }
}

QList<QUrl> LibraryView::shelfUrls(Shelf shelf) const
{
    if (!isDocumentShelf(shelf)) {
        return {}; // the corpus shelf keeps its own model
    }
    const QStandardItemModel *const model = modelForShelf(shelf);
    QList<QUrl> urls;
    for (int row = 0; row < model->rowCount(); ++row) {
        const QStandardItem *item = model->item(row);
        const QUrl url = item->data(UrlRole).toUrl();
        if (!item->data(HeaderRole).toBool() && url.isValid()) {
            urls.append(url);
        }
    }
    return urls;
}

void LibraryView::activate(const QUrl &url, double booksProgress)
{
    if (url.isValid()) {
        Q_EMIT itemActivated(url, booksProgress);
    }
}

bool LibraryView::showAdjacentDocumentsForIndex(const QModelIndex &index)
{
    if (!index.isValid() || isLibraryHeaderIndex(index) || !index.data(PaperLibrarySectionedModel::SourceRowRole).isValid()) {
        return false;
    }

    const QString title = index.data(Qt::DisplayRole).toString().trimmed();
    const QString metadata = corpusPaperMetadataLineForIndex(index);
    const QString slug = index.data(PaperLibraryModel::SlugRole).toString().trimmed();
    if (m_paperModel && m_paperModel->isLoaded() && !slug.isEmpty()) {
        const QList<int> relatedRows = m_paperModel->relatedRowsForSlug(slug, 160);
        if (!relatedRows.isEmpty()) {
            PaperLibrarySectionedModel *sections = paperSectionsForShelf(PapersShelf);
            if (sections) {
                attachCorpusShelf(PapersShelf);
                sections->setExplicitSourceRows(relatedRows,
                                                i18nc("@label adjacent corpus result group", "Adjacent documents"),
                                                i18nc("@title empty adjacent corpus search tile", "No adjacent documents"));
                {
                    const QSignalBlocker blocker(m_searchField);
                    m_searchField->clear();
                }
                showShelf(PapersShelf);
                const QString target = title.isEmpty() ? slug : title;
                setCorpusResultMode(i18nc("@label compact corpus result mode chip", "Adjacent"), target);
                showPaperNotice(metadata.isEmpty() ? i18nc("@info after entering graph-adjacent mode", "Adjacent documents for %1", target)
                                                   : i18nc("@info after entering graph-adjacent mode", "Adjacent documents for %1 — %2", target, metadata),
                                false);
                requestCorpusCovers();
                selectFirstTile();
                return true;
            }
        }
    }

    QString relatedQuery = index.data(PaperLibrarySectionedModel::RelatedQueryRole).toString().trimmed();
    if (relatedQuery.isEmpty()) {
        const QStringList tags = index.data(PaperLibrarySectionedModel::TopicTagsRole).toStringList();
        for (const QString &tag : tags) {
            if (!tag.trimmed().isEmpty()) {
                relatedQuery = tag.trimmed();
                break;
            }
        }
    }
    if (relatedQuery.isEmpty()) {
        relatedQuery = index.data(Qt::DisplayRole).toString().trimmed();
    }
    if (relatedQuery.isEmpty()) {
        return false;
    }

    setSearchQuery(relatedQuery);
    const QString target = title.isEmpty() ? relatedQuery : title;
    setCorpusResultMode(i18nc("@label compact corpus result mode chip", "Related"), target);
    const QString detail = joinCompact({metadata, relatedQuery});
    showPaperNotice(detail.isEmpty() ? i18nc("@info after entering adjacent-documents mode", "Adjacent documents: %1", target)
                                     : i18nc("@info after entering adjacent-documents mode", "Adjacent documents for %1 — %2", target, detail),
                    false);
    return true;
}

bool LibraryView::showAdjacentDocumentsForCurrentTile()
{
    return m_grid && showAdjacentDocumentsForIndex(m_grid->currentIndex());
}

void LibraryView::tileClicked(const QModelIndex &index)
{
    if (!index.isValid() || isLibraryHeaderIndex(index)) {
        return;
    }
    if (index.data(PaperLibrarySectionedModel::SourceRowRole).isValid()) {
        const auto *sections = qobject_cast<const PaperLibrarySectionedModel *>(index.model());
        const QString pdfPath = sections ? sections->resolvePath(index) : QString();
        if (pdfPath.isEmpty()) {
            showPaperNotice(i18nc("@info after activating a corpus tile with no PDF on disk", "PDF not local — restore in PaperLibrary"));
            return;
        }
        const QUrl url = QUrl::fromLocalFile(pdfPath);
        const QString title = index.data(Qt::DisplayRole).toString().trimmed();
        const QString detail = index.data(PaperLibraryModel::DetailRole).toString().trimmed();
        const QString priority = index.data(PaperLibrarySectionedModel::PriorityHintRole).toString().trimmed();
        const QString intent = index.data(PaperLibrarySectionedModel::ShelfIntentRole).toString().trimmed();
        const QString relation = index.data(PaperLibrarySectionedModel::RelationHintRole).toString().trimmed();
        const QString relatedQuery = index.data(PaperLibrarySectionedModel::RelatedQueryRole).toString().trimmed();
        QStringList tags = index.data(PaperLibrarySectionedModel::TopicTagsRole).toStringList();
        tags.removeAll(QString());
        if (!title.isEmpty()) {
            m_store->setTitle(url, title);
        }
        if (!tags.isEmpty()) {
            m_store->setTags(url, tags);
        }
        const QString adjacentHint = relatedQuery.isEmpty() ? QString() : i18nc("@info stored metadata hint for finding nearby papers", "Adjacent: %1", relatedQuery);
        const QString description = joinCompact({detail, priority, intent, relation, adjacentHint});
        if (!description.isEmpty()) {
            m_store->setDescription(url, description);
        }
        activate(url, -1.0);
        return;
    }
    activate(index.data(UrlRole).toUrl(), index.data(ProgressRole).toDouble());
}

void LibraryView::activateCurrentTile()
{
    if (!m_grid) {
        return;
    }
    QModelIndex index = m_grid->currentIndex();
    if (!index.isValid() && m_grid->selectionModel() && !m_grid->selectionModel()->selectedIndexes().isEmpty()) {
        index = m_grid->selectionModel()->selectedIndexes().constFirst();
    }
    tileClicked(index);
}

void LibraryView::selectFirstTile()
{
    if (!m_grid || !m_grid->model()) {
        return;
    }
    QItemSelectionModel *selection = m_grid->selectionModel();
    for (int row = 0; row < m_grid->model()->rowCount(); ++row) {
        const QModelIndex index = m_grid->model()->index(row, 0);
        if (!isLibraryHeaderIndex(index)) {
            m_grid->setCurrentIndex(index);
            if (selection) {
                selection->select(index, QItemSelectionModel::ClearAndSelect);
            }
            return;
        }
    }
}

void LibraryView::showContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_grid->indexAt(pos);
    if (!index.isValid() || isLibraryHeaderIndex(index)) {
        return;
    }
    if (index.data(PaperLibrarySectionedModel::SourceRowRole).isValid()) {
        const QString relatedQuery = index.data(PaperLibrarySectionedModel::RelatedQueryRole).toString();
        const QString relationHint = index.data(PaperLibrarySectionedModel::RelationHintRole).toString();
        auto *sections = qobject_cast<PaperLibrarySectionedModel *>(const_cast<QAbstractItemModel *>(index.model()));
        const QString pdfPath = sections ? sections->resolvePath(index) : QString();
        const bool downranked = index.data(PaperLibrarySectionedModel::DownrankedRole).toBool();

        QMenu menu(this);
        QString relatedLabel = relationHint;
        if (relatedLabel.startsWith(QStringLiteral("Related: "))) {
            relatedLabel.remove(0, 9);
        }
        if (relatedLabel.isEmpty()) {
            relatedLabel = relatedQuery;
        }
        QAction *relatedAction = menu.addAction(relatedQuery.isEmpty() ? i18nc("@action:inmenu on a corpus tile", "Show Adjacent Documents")
                                                                       : i18nc("@action:inmenu on a corpus tile", "Show Adjacent Documents: %1", relatedLabel));
        QAction *downrankAction = menu.addAction(downranked ? i18nc("@action:inmenu on a corpus tile", "Undo Thumbs Down") : i18nc("@action:inmenu on a corpus tile", "Thumbs Down"));
        QAction *clearSearchAction = menu.addAction(i18nc("@action:inmenu on a corpus tile", "Clear Search"));
        clearSearchAction->setEnabled(!searchQuery().isEmpty() || !m_corpusResultLabel.isEmpty() || !m_corpusResultDetail.isEmpty() || (sections && sections->hasExplicitSourceRows()));
        menu.addSeparator();
#if defined(Q_OS_MACOS)
        QAction *revealAction = menu.addAction(i18nc("@action:inmenu on a corpus tile", "Show in Finder"));
#else
        QAction *revealAction = menu.addAction(i18nc("@action:inmenu on a corpus tile", "Open Containing Folder"));
#endif
        revealAction->setEnabled(!pdfPath.isEmpty());

        const QAction *chosen = menu.exec(m_grid->viewport()->mapToGlobal(pos));
        if (chosen == relatedAction) {
            showAdjacentDocumentsForIndex(index);
        } else if (chosen == downrankAction) {
            if (sections) {
                sections->setDownranked(index, !downranked);
                QTimer::singleShot(0, this, &LibraryView::requestCorpusCovers);
            }
        } else if (chosen == clearSearchAction) {
            clearActiveCorpusResult();
        } else if (chosen == revealAction) {
#if defined(Q_OS_MACOS)
            QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), pdfPath});
#else
            KIO::highlightInFileManager({QUrl::fromLocalFile(pdfPath)});
#endif
        }
        return;
    }
    const QUrl url = index.data(UrlRole).toUrl();
    if (!url.isValid()) {
        return;
    }
    const bool pinned = index.data(PinnedRole).toBool();
    const bool downranked = index.data(DownrankedRole).toBool();
    const bool finished = index.data(FinishedReadingRole).toBool();
    const bool canFinish = index.data(FormatRole).toString() == QLatin1String("EPUB") || url.fileName().endsWith(QStringLiteral(".epub"), Qt::CaseInsensitive);

    QMenu menu(this);
    QAction *pinAction = menu.addAction(pinned ? i18nc("@action:inmenu on a library tile", "Unpin") : i18nc("@action:inmenu on a library tile", "Pin"));
    QAction *downrankAction = menu.addAction(downranked ? i18nc("@action:inmenu on a library tile", "Undo Thumbs Down") : i18nc("@action:inmenu on a library tile", "Thumbs Down"));
    QAction *finishedAction = nullptr;
    if (canFinish) {
        finishedAction = menu.addAction(finished ? i18nc("@action:inmenu on a library tile", "Mark as Reading") : i18nc("@action:inmenu on a library tile", "Finished Reading"));
    }
    QAction *editAction = menu.addAction(i18nc("@action:inmenu on a library tile", "Edit Title && Tags…"));
    QAction *removeAction = menu.addAction(i18nc("@action:inmenu on a library tile", "Remove from Library"));
    menu.addSeparator();
#if defined(Q_OS_MACOS)
    QAction *revealAction = menu.addAction(i18nc("@action:inmenu on a library tile", "Show in Finder"));
#else
    QAction *revealAction = menu.addAction(i18nc("@action:inmenu on a library tile", "Open Containing Folder"));
#endif
    revealAction->setEnabled(url.isLocalFile());

    const QAction *chosen = menu.exec(m_grid->viewport()->mapToGlobal(pos));
    if (chosen == pinAction) {
        m_store->setPinned(url, !pinned);
        refresh();
    } else if (chosen == downrankAction) {
        m_store->setDownranked(url, !downranked);
        refresh();
    } else if (chosen == finishedAction) {
        m_store->setFinishedReading(url, !finished);
        refresh();
        const int target = tabIndexForShelf(!finished ? FinishedShelf : BooksShelf);
        if (target >= 0) {
            m_shelfSwitch->setCurrentIndex(target);
        }
    } else if (chosen == editAction) {
        editMetadata(url);
    } else if (chosen == removeAction) {
        m_store->remove(url);
        refresh();
    } else if (chosen == revealAction) {
#if defined(Q_OS_MACOS)
        QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), url.toLocalFile()});
#else
        KIO::highlightInFileManager({url});
#endif
    }
}

void LibraryView::editMetadata(const QUrl &url)
{
    const LibraryStore::Entry entry = m_store->metadata(url);

    QDialog dialog(this);
    dialog.setWindowTitle(i18nc("@title:window", "Edit Title & Tags"));

    QLineEdit *titleEdit = new QLineEdit(entry.title, &dialog);
    titleEdit->setPlaceholderText(QFileInfo(url.fileName()).completeBaseName());
    QLineEdit *tagsEdit = new QLineEdit(entry.tags.join(QStringLiteral(", ")), &dialog);
    tagsEdit->setPlaceholderText(i18nc("@info:placeholder comma-separated tags", "Manuscript, Neuroscience, …"));
    QLineEdit *descriptionEdit = new QLineEdit(entry.description, &dialog);
    descriptionEdit->setPlaceholderText(i18nc("@info:placeholder", "One-line summary"));

    QFormLayout *form = new QFormLayout(&dialog);
    form->addRow(i18nc("@label:textbox", "Title:"), titleEdit);
    form->addRow(i18nc("@label:textbox", "Tags:"), tagsEdit);
    form->addRow(i18nc("@label:textbox", "Description:"), descriptionEdit);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    dialog.setMinimumWidth(380);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QStringList tags;
    const QStringList rawTags = tagsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &rawTag : rawTags) {
        const QString tag = rawTag.trimmed();
        if (!tag.isEmpty()) {
            tags.append(tag);
        }
    }
    m_store->setTitle(url, titleEdit->text().trimmed());
    m_store->setTags(url, tags);
    m_store->setDescription(url, descriptionEdit->text().trimmed());
    refresh();
}

void LibraryView::coverArrived(const QString &filePath, const QString &coverPath)
{
    const QPixmap cover(coverPath);
    if (cover.isNull()) {
        return;
    }
    for (int shelf = PdfShelf; shelf < DocumentShelfCount; ++shelf) {
        QStandardItemModel *const model = modelForShelf(static_cast<Shelf>(shelf));
        for (int row = 0; row < model->rowCount(); ++row) {
            QStandardItem *item = model->item(row);
            if (!item->data(HeaderRole).toBool() && item->data(UrlRole).toUrl().toLocalFile() == filePath) {
                item->setData(QVariant::fromValue(cover), CoverRole);
                item->setData(CoverLoader::isGeneratedCoverPath(coverPath), GeneratedCoverRole);
            }
        }
    }
    for (PaperLibrarySectionedModel *sections : m_paperSections) {
        if (sections) {
            sections->setCoverForPath(filePath, QVariant::fromValue(cover), CoverLoader::isGeneratedCoverPath(coverPath));
        }
    }
}

void LibraryView::scheduleRefresh(int delayMs)
{
    if (m_refreshPending) {
        return;
    }

    m_refreshPending = true;
    // Let the tab strip and the empty page paint before rebuilding shelves.
    QTimer::singleShot(qMax(1, delayMs), this, [this]() {
        m_refreshPending = false;
        refresh();
    });
}

void LibraryView::showEvent(QShowEvent *event)
{
    const bool firstShow = !m_hasShown;
    m_hasShown = true;
    if (m_deferInitialRefresh) {
        m_deferInitialRefresh = false;
        showStartupPlaceholder();
        scheduleRefresh(firstShow ? 180 : 1);
    } else if (!firstShow) {
        scheduleRefresh(1);
    }
    // Focus the visible list; keeps the ⌘F shortcut's widget-local context live
    m_grid->setFocus();
    QWidget::showEvent(event);
}

void LibraryView::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange && !m_applyingChromePalette) {
        applyChromePalette();
        refresh(); // typographic covers are palette-keyed (light vs dark)
    }
    QWidget::changeEvent(event);
}

void LibraryView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        // The first Escape clears an active search; only the next one
        // hands the library back to the document
        if (!m_searchField->text().isEmpty()) {
            m_searchField->clear(); // textChanged restores the shelf's arrangement
            m_grid->setFocus();
        } else if (clearActiveCorpusResult()) {
            m_grid->setFocus();
        } else {
            Q_EMIT closeRequested();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) {
        if (m_grid->hasFocus() || m_grid->viewport()->hasFocus()) {
            activateCurrentTile();
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

#include "libraryview.moc"
