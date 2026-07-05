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
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QShortcut>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTextLayout>
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
static constexpr int TileWidth = 160;
static constexpr int TilePadding = 12;
static constexpr int CoverWidth = TileWidth - 2 * TilePadding;
static constexpr int CoverHeight = 170;
static constexpr int CoverRadius = 6;
static constexpr int TitleGap = 8;
static constexpr int TitleLines = 2;
static constexpr int TagGap = 2;
static constexpr int ProgressGap = 7;
static constexpr int ProgressBarHeight = 4;
static constexpr int GridSpacing = 12;
static constexpr int CorpusCoverHeight = 144;

/**
 * Renders cover thumbnails asynchronously through macOS QuickLook
 * (qlmanage ships with the OS) into a disk cache keyed by (path, mtime),
 * at most two renders at a time. EPUBs never go near QuickLook (it hangs
 * on them): their real cover is pulled straight from the archive.
 * Renders the heuristic judges to be mostly-white text pages are
 * discarded for a generated typographic card, as are renders that fail
 * or time out — so on platforms without QuickLook every titled entry
 * still gets a cover. Files that yield neither render nor card are
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
#if defined(Q_OS_MACOS)
        const QString systemQlmanage = QStringLiteral("/usr/bin/qlmanage");
        m_qlmanage = QFileInfo::exists(systemQlmanage) ? systemQlmanage : QStandardPaths::findExecutable(QStringLiteral("qlmanage"));
#endif
    }

    ~CoverLoader() override
    {
        const QList<QProcess *> processes = findChildren<QProcess *>();
        for (QProcess *process : processes) {
            process->disconnect(this);
            process->kill();
            process->waitForFinished(1000);
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
            // and never involve QuickLook, which hangs forever on EPUBs here
            if (m_failed.contains(filePath) || !extractEpubCover(filePath)) {
                m_failed.insert(filePath); // no embedded image; don't reparse
                generateTypographic(filePath, spec);
            }
            return;
        }
        if (m_qlmanage.isEmpty() || m_failed.contains(filePath)) {
            // No QuickLook on this platform, or it failed on this file
            // before: straight to the typographic card
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
    static constexpr QLatin1StringView DesignVersion {"tg3"};

    QString coverPath(const QString &filePath) const
    {
        // Keyed by (design, path, mtime): a changed file re-renders under
        // a new name, and a design bump retires the whole render family.
        // Only renders that PASSED the classifier are ever stored here.
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
                // qlmanage-render sized, so the cache stays small
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
        // Each render gets its own output dir: qlmanage names the thumbnail
        // after the source file, and basenames may collide across requests
        const QString outDir = m_cacheDir + QStringLiteral("/render-%1-%2").arg(QCoreApplication::applicationPid()).arg(++m_renderSerial);
        QDir().mkpath(outDir);

        ++m_running;
        QProcess *process = new QProcess(this);
        auto finalize = [this, process, filePath, outDir]() {
            if (process->property("finalized").toBool()) {
                return;
            }
            process->setProperty("finalized", true);
            process->deleteLater();
            --m_running;
            m_pending.remove(filePath);
            const CoverGenerator::CoverSpec spec = m_specs.take(filePath);

            const QString produced = outDir + QLatin1Char('/') + QFileInfo(filePath).fileName() + QStringLiteral(".png");
            bool announced = false;
            if (QFileInfo::exists(produced)) {
                // The classifier gates the cache: a render only becomes a
                // cached cover after it has EARNED its place. A mostly-white
                // text page is replaced by the typographic card up front and
                // its render never touches the cache — so no future session
                // can serve a classifier-failing render, whatever it finds
                // on disk
                const bool keepRender = CoverHeuristic::analyze(QImage(produced)) == CoverHeuristic::KeepRender;
                if (!keepRender && generateTypographic(filePath, spec)) {
                    announced = true; // the card is cached and announced instead
                } else {
                    // Keep the render — also the fallback for untitled
                    // entries whose card cannot be generated
                    const QString target = coverPath(filePath);
                    QFile::remove(target);
                    if (QFile::rename(produced, target)) {
                        Q_EMIT coverReady(filePath, target);
                        announced = true;
                    }
                }
            }
            QDir(outDir).removeRecursively();

            if (!announced) {
                // QuickLook could not render it (or timed out): never retry
                // this session; titled entries get the typographic card
                m_failed.insert(filePath);
                generateTypographic(filePath, spec);
            }
            startNext();
        };
        connect(process, &QProcess::finished, this, finalize);
        connect(process, &QProcess::errorOccurred, this, [finalize](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                finalize();
            }
        });

        // QuickLook can stall for a long time (e.g. EPUBs whose extension
        // never answers, or iCloud files that need downloading first); a
        // watchdog keeps a hung render from starving the queue
        QTimer *watchdog = new QTimer(process);
        watchdog->setSingleShot(true);
        connect(watchdog, &QTimer::timeout, process, &QProcess::kill); // kill → finished(CrashExit) → finalize
        watchdog->start(20000);

        process->start(m_qlmanage, {QStringLiteral("-t"), QStringLiteral("-s"), QStringLiteral("512"), QStringLiteral("-o"), outDir, filePath});
    }

    QWidget *m_host; // supplies the palette for generated cards
    QString m_cacheDir;
    QString m_qlmanage;
    QStringList m_queue;
    QSet<QString> m_pending;
    QHash<QString, CoverGenerator::CoverSpec> m_specs;
    QSet<QString> m_failed;
    int m_running = 0;
    int m_renderSerial = 0;
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

static QString corpusShelfGuide(LibraryView::Shelf shelf, int mode)
{
    QString modeLabel;
    switch (mode) {
    case PaperLibrarySectionedModel::ByTopic:
        modeLabel = i18nc("@label corpus library grouping", "Topics");
        break;
    case PaperLibrarySectionedModel::ByProject:
        modeLabel = i18nc("@label corpus library grouping", "Projects");
        break;
    case PaperLibrarySectionedModel::ByType:
        modeLabel = i18nc("@label corpus library grouping", "Types");
        break;
    case PaperLibrarySectionedModel::BySource:
        modeLabel = i18nc("@label corpus library grouping", "Sources");
        break;
    case PaperLibrarySectionedModel::ByYear:
        modeLabel = i18nc("@label corpus library grouping", "Years");
        break;
    case PaperLibrarySectionedModel::ByJournal:
        modeLabel = i18nc("@label corpus library grouping", "Journals");
        break;
    case PaperLibrarySectionedModel::ReadNext:
    default:
        modeLabel = i18nc("@label corpus library grouping", "For you");
        break;
    }

    switch (shelf) {
    case LibraryView::MedicineShelf:
        return i18nc("@info corpus shelf guide", "%1: user-defined medicine focus shelf", modeLabel);
    case LibraryView::MndShelf:
        return i18nc("@info corpus shelf guide", "%1: user-defined motor-neuron-disease focus shelf", modeLabel);
    case LibraryView::WorkShelf:
        return i18nc("@info corpus shelf guide", "%1: current projects, active reading queues, and related work", modeLabel);
    case LibraryView::TextbooksShelf:
        return i18nc("@info corpus shelf guide", "%1: textbooks and reference books grouped for quick retrieval", modeLabel);
    case LibraryView::BooksShelf:
        return i18nc("@info corpus shelf guide", "%1: long-form books from the corpus", modeLabel);
    case LibraryView::FictionShelf:
        return i18nc("@info corpus shelf guide", "%1: fiction queue and current series reading", modeLabel);
    case LibraryView::NonfictionShelf:
        return i18nc("@info corpus shelf guide", "%1: biography, history, politics, anthropology, social theory", modeLabel);
    case LibraryView::PapersShelf:
        return i18nc("@info corpus shelf guide", "%1: papers ranked by active projects and adjacent reading", modeLabel);
    case LibraryView::StarterPackShelf:
    case LibraryView::PdfShelf:
        break;
    }
    return QString();
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

static QString cleanedFilenameTitle(const QUrl &url)
{
    QString title = QFileInfo(url.isLocalFile() ? url.toLocalFile() : url.fileName()).completeBaseName();
    title.replace(QLatin1Char('_'), QLatin1Char(' '));
    title.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    title.replace(QRegularExpression(QStringLiteral("\\s+-\\s+")), QStringLiteral(" - "));
    return title.simplified();
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
            lines.append(QFontMetrics(font).elidedText(text.mid(line.textStart()).trimmed(), Qt::ElideMiddle, width));
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
            const int width = view ? qMax(TileWidth, view->viewport()->width() - 2 * GridSpacing - 8) : 3 * TileWidth;
            return QSize(width, option.fontMetrics.height() + 14);
        }

        const int coverHeight = isCorpusTile(index) ? CorpusCoverHeight : CoverHeight;
        int height = TilePadding + coverHeight;
        if (reservesProgressRow(index)) {
            height += ProgressGap + QFontMetrics(smallerFont(option.font)).height();
        }
        height += TitleGap + TitleLines * option.fontMetrics.height();
        height += TagGap + QFontMetrics(smallerFont(option.font)).height(); // tag row, reserved even when untagged
        height += TilePadding;
        return QSize(TileWidth, height);
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
        const QRect coverBox(option.rect.left() + TilePadding, option.rect.top() + TilePadding, CoverWidth, coverHeight);

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

        if (index.data(LibraryView::PinnedRole).toBool() || index.data(PaperLibraryModel::PinnedRole).toBool()) {
            const QPointF badgeCenter(coverRect.right() - 12, coverRect.top() + 13);
            painter->setPen(Qt::NoPen);
            painter->setBrush(palette.color(QPalette::Highlight));
            painter->drawEllipse(badgeCenter, 9, 9);
            painter->setBrush(palette.color(QPalette::HighlightedText));
            painter->drawPath(starPath(badgeCenter, 5.5));
        }
        if (index.data(LibraryView::DownrankedRole).toBool() || index.data(PaperLibrarySectionedModel::DownrankedRole).toBool()) {
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
            const QStringList tags = index.data(LibraryView::TagsRole).toStringList();
            const QStringList corpusTags = index.data(PaperLibrarySectionedModel::TopicTagsRole).toStringList();
            const QStringList shownTags = tags.isEmpty() ? corpusTags : tags;
            if (!shownTags.isEmpty()) {
                const QFont tagFont = smallerFont(option.font);
                const QFontMetrics tagMetrics(tagFont);
                QColor tagColor = palette.color(QPalette::Text);
                tagColor.setAlphaF(0.55);
                painter->setFont(tagFont);
                painter->setPen(tagColor);
                const QString tagRow = QStringList(shownTags.mid(0, 2)).join(QStringLiteral(" · "));
                painter->drawText(QRect(textLeft, lineTop + TagGap, textWidth, tagMetrics.height()), Qt::AlignHCenter | Qt::AlignTop, tagMetrics.elidedText(tagRow, Qt::ElideRight, textWidth));
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

    static QString visualKeyForCorpusCard(const QModelIndex &index)
    {
        return QStringList({index.data(Qt::DisplayRole).toString(),
                            index.data(PaperLibraryModel::DetailRole).toString(),
                            index.data(PaperLibrarySectionedModel::KindRole).toString(),
                            index.data(PaperLibrarySectionedModel::FocusRole).toString(),
                            index.data(PaperLibrarySectionedModel::ShelfIntentRole).toString(),
                            index.data(PaperLibrarySectionedModel::RelationHintRole).toString(),
                            index.data(PaperLibrarySectionedModel::PriorityHintRole).toString(),
                            index.data(PaperLibrarySectionedModel::TopicTagsRole).toStringList().join(QLatin1Char(' '))})
            .join(QLatin1Char(' '))
            .toCaseFolded();
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
                                                   QStringLiteral("diagnostic"),
                                                   QStringLiteral("accuracy"),
                                                   QStringLiteral("neurofilament"),
                                                   QStringLiteral("threshold"),
                                                   QStringLiteral("sensitivity"),
                                                   QStringLiteral("specificity")});
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

        if (neuroLike && !biomarkerLike && !trialLike && !methodLike && !clinicalLike) {
            const QPointF soma(area.left() + area.width() * 0.22, area.top() + area.height() * 0.55);
            painter->setBrush(accent);
            painter->drawEllipse(soma, area.width() * 0.08, area.width() * 0.08);
            painter->setBrush(Qt::NoBrush);

            QPainterPath axon;
            axon.moveTo(soma);
            axon.cubicTo(area.left() + area.width() * 0.45, area.top() + area.height() * 0.22, area.left() + area.width() * 0.62, area.top() + area.height() * 0.74, area.right() - 6, area.top() + area.height() * 0.38);
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
        const QString title = index.data(Qt::DisplayRole).toString();
        const QString detail = index.data(PaperLibraryModel::DetailRole).toString();
        const QString intent = index.data(PaperLibrarySectionedModel::ShelfIntentRole).toString();
        const QString relation = index.data(PaperLibrarySectionedModel::RelationHintRole).toString();
        const QString priority = index.data(PaperLibrarySectionedModel::PriorityHintRole).toString();
        const QStringList tags = index.data(PaperLibrarySectionedModel::TopicTagsRole).toStringList();
        const bool missing = index.data(PaperLibraryModel::MissingRole).toBool();
        const QString visualKey = visualKeyForCorpusCard(index);

        QPainterPath clip;
        clip.addRoundedRect(coverRect, CoverRadius, CoverRadius);
        const bool darkMode = palette.color(QPalette::Base).lightness() < 128;
        const QString accentSeed = joinCompact({seed, relation, priority, QStringList(tags.mid(0, 2)).join(QStringLiteral(" · ")), title});
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

        const QRectF visualRect(coverRect.left() + 14, coverRect.top() + 31, coverRect.width() - 28, 43);
        QColor visualPanel = palette.color(QPalette::Base);
        visualPanel.setAlphaF(darkMode ? 0.36 : 0.48);
        painter->setBrush(visualPanel);
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(visualRect, 5, 5);
        drawCorpusMotif(painter, visualRect.adjusted(4, 3, -4, -3), visualKey, accent, palette);

        QFont kindFont = smallerFont(option.font);
        kindFont.setBold(true);
        QColor muted = palette.color(QPalette::Text);
        muted.setAlphaF(0.50);
        painter->setFont(kindFont);
        painter->setPen(muted);
        const QString topLabel = !focus.isEmpty() && focus != kind ? focus : kind;
        painter->drawText(coverRect.adjusted(10, 10, -10, -10), Qt::AlignLeft | Qt::AlignTop, QFontMetrics(kindFont).elidedText(topLabel, Qt::ElideRight, coverRect.width() - 20));

        QFont titleFont = option.font;
        titleFont.setBold(true);
        titleFont.setPointSizeF(qMax(8.0, titleFont.pointSizeF() * 0.92));
        painter->setFont(titleFont);
        painter->setPen(palette.color(QPalette::Text));
        const QStringList titleLines = wrapTitle(title, titleFont, coverRect.width() - 20, 3);
        int y = coverRect.top() + 80;
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
        const int metaTop = qMax(y + 3, coverRect.bottom() - 36);
        QString meta = intent;
        if (!priority.isEmpty() && priority != intent && priority != detail) {
            meta = joinCompact({priority, meta});
        }
        if (meta.isEmpty()) {
            meta = detail;
        }
        if (missing && !meta.contains(QStringLiteral("PDF not local"))) {
            meta = joinCompact({meta, i18nc("@info on a corpus tile whose PDF is not local", "PDF not local")});
        }
        if (!meta.isEmpty()) {
            painter->drawText(QRect(coverRect.left() + 10, metaTop, coverRect.width() - 20, metaMetrics.height()), Qt::AlignLeft | Qt::AlignTop, metaMetrics.elidedText(meta, Qt::ElideRight, coverRect.width() - 20));
        }

        QString tagRow = relation;
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
    m_booksModel->setProperty("booksShelf", true);
    m_starterPackModel->setProperty("booksShelf", true);

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
    const int tileHeight = TilePadding + coverHeight + ProgressGap + smallMetrics.height() + TitleGap + TitleLines * titleMetrics.height() + TagGap + smallMetrics.height() + TilePadding;

    m_grid->setViewMode(QListView::IconMode);
    m_grid->setLayoutMode(QListView::Batched);
    m_grid->setBatchSize(64);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setFlow(QListView::LeftToRight);
    m_grid->setWrapping(true);
    m_grid->setSpacing(GridSpacing);
    m_grid->setGridSize(QSize(TileWidth + GridSpacing, tileHeight + GridSpacing));
    m_grid->setUniformItemSizes(true);
}

void LibraryView::refresh()
{
    const QList<LibraryStore::Entry> pdfEntries = m_store->entries({QStringLiteral("pdf")});
    QList<ShelfEntry> pdfs;
    pdfs.reserve(pdfEntries.size());
    for (const LibraryStore::Entry &entry : pdfEntries) {
        // Title precedence: curated store title, else filename sans extension
        const QString title = entry.title.isEmpty() ? cleanedFilenameTitle(entry.url) : entry.title;
        ShelfEntry shelfEntry{entry.url, title, entry.tags, entry.description, entry.keywords, entry.pinned, entry.downranked, entry.openCount, entry.lastOpened, -1.0, QStringLiteral("PDF"), {}};
        enrichShelfEntry(shelfEntry);
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
        if (mirrorAppleBooks && (canonical.isEmpty() || !progressByPath.contains(canonical))) {
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
        const EpubCover::Metadata *metadata = nullptr;
        if (entry.url.isLocalFile()) {
            metadata = &epubMetadataFor(entry.url.toLocalFile());
        }
        ShelfEntry shelfEntry{
            entry.url, title, entry.tags, entry.description, entry.keywords, entry.pinned, entry.downranked, entry.openCount, entry.lastOpened, progressByPath.value(canonical, -1.0), QStringLiteral("EPUB"), {}};
        enrichShelfEntry(shelfEntry, metadata);
        books.append(shelfEntry);
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
        const EpubCover::Metadata metadata = EpubCover::metadata(book.path);
        ShelfEntry shelfEntry{url, title, stored.tags, stored.description, stored.keywords, stored.pinned, stored.downranked, stored.openCount, stored.lastOpened, book.progress, QStringLiteral("EPUB"), {}};
        enrichShelfEntry(shelfEntry, &metadata);
        booksOnly.append(shelfEntry);
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
    m_shelfEntries[BooksShelf] = books;

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
    return shelf == PdfShelf || shelf == BooksShelf || shelf == TextbooksShelf || shelf == MedicineShelf || shelf == MndShelf || shelf == WorkShelf || shelf == FictionShelf || shelf == NonfictionShelf || shelf == StarterPackShelf;
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
    const QString haystack = smartShelfHaystack(entry);
    if (compactPublicationTypeKey(publicationTypeTitle(entry)) == QLatin1String("textbooks")) {
        return true;
    }
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
    const QString haystack = smartShelfHaystack(entry);
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
    const QString haystack = smartShelfHaystack(entry);
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
    const QString haystack = smartShelfHaystack(entry);
    if (containsAnyNeedle(haystack,
                          {QStringLiteral("great depression"),
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
                              QStringLiteral("depression"),
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
    const QString haystack = smartShelfHaystack(entry);
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
    const QString haystack = smartShelfHaystack(entry);
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
    const QString haystack = smartShelfHaystack(entry);
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
    const QString haystack = smartShelfHaystack(entry);
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
    const QString haystack = smartShelfHaystack(entry);
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
                              QStringLiteral("memoir"),
                              QStringLiteral("essay"),
                              QStringLiteral("science")});
}

QString LibraryView::focusTagFor(const ShelfEntry &entry)
{
    for (const QString &tag : entry.tags) {
        const QString trimmed = tag.trimmed();
        if (!trimmed.isEmpty()) {
            return trimmed;
        }
    }

    const QString haystack = smartShelfHaystack(entry);
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
    return publicationTypeTitle(entry);
}

void LibraryView::enrichShelfEntry(ShelfEntry &entry, const EpubCover::Metadata *epubMetadata)
{
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
    const QString haystack = smartShelfHaystack(probe);

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
    if (!focus.isEmpty()) {
        if (focus == QLatin1String("Politics") || focus == QLatin1String("Anthropology") || focus == QLatin1String("Fiction") || focus == QLatin1String("Non-fiction")) {
            QStringList filteredTags;
            for (const QString &tag : std::as_const(tags)) {
                const QString key = compactPublicationTypeKey(tag);
                if (key != QLatin1String("psychiatry") && key != QLatin1String("medicine")) {
                    filteredTags.append(tag);
                }
            }
            tags = filteredTags;
        }
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
    populateSections(model, arrangeSections(entries, mode));
}

void LibraryView::populateSections(QStandardItemModel *model, const QList<Section> &sections)
{
    model->clear();

    for (const Section &section : sections) {
        for (const ShelfEntry &entry : section.entries) {
            model->appendRow(makeTileItem(entry));
        }
    }
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

bool LibraryView::matchesQuery(const ShelfEntry &entry, const QString &query)
{
    const auto contains = [&query](const QString &text) { return text.contains(query, Qt::CaseInsensitive); };
    return contains(entry.title) || contains(entry.url.fileName()) || contains(entry.description) || std::any_of(entry.tags.cbegin(), entry.tags.cend(), contains) || std::any_of(entry.keywords.cbegin(), entry.keywords.cend(), contains);
}

void LibraryView::applySearch()
{
    cancelContentSearch(); // whatever was in flight answers a stale query
    rebuildShelves();
    for (PaperLibrarySectionedModel *sections : m_paperSections) {
        if (!sections) {
            continue;
        }
        // The corpus's instant layer: one substring filter over ~18k
        // precomputed haystacks per keystroke
        sections->setQuery(searchQuery());
    }
    if (!searchQuery().isEmpty()) {
        m_searchDebounce->start();
    }
}

void LibraryView::rebuildShelves()
{
    const QString query = searchQuery();
    for (int shelf = PdfShelf; shelf < DocumentShelfCount; ++shelf) {
        QStandardItemModel *const model = modelForShelf(static_cast<Shelf>(shelf));
        if (query.isEmpty()) {
            if (shelf == PdfShelf && m_viewModes[shelf] == FrequentMode) {
                populateSections(model, {{QString(), m_shelfEntries[shelf]}});
            } else {
                populate(model, m_shelfEntries[shelf], m_viewModes[shelf]);
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
        populateSections(model, {{QString(), matches}});
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
    QStandardItem *item = new QStandardItem(entry.title);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setData(entry.url, UrlRole);
    item->setData(entry.pinned, PinnedRole);
    item->setData(entry.downranked, DownrankedRole);
    item->setData(entry.progress, ProgressRole);
    item->setData(entry.format, FormatRole);
    QStringList shownTags = entry.tags;
    if (shownTags.isEmpty()) {
        const QString inferred = focusTagFor(entry);
        if (!inferred.isEmpty()) {
            shownTags.append(inferred);
        }
    }
    item->setData(shownTags, TagsRole);
    item->setData(entry.description, DescriptionRole);
    item->setAccessibleText(entry.title);
    QString tooltipDescription = entry.description;
    if (entry.url.isLocalFile()) {
        const QString filePath = entry.url.toLocalFile();
        // What a typographic card would say for this entry. Books get
        // their byline, foot and fallback description from the EPUB's own
        // OPF metadata — curated store metadata, when present, wins
        CoverGenerator::CoverSpec spec {entry.title, QString(), QString(), shownTags.value(0)};
        if (entry.format == QLatin1String("EPUB")) {
            const EpubCover::Metadata &metadata = epubMetadataFor(filePath);
            spec.authors = metadata.creators;
            spec.yearJournal = metadata.year;
            if (entry.description.isEmpty()) {
                item->setData(metadata.description, DescriptionRole);
                tooltipDescription = metadata.description;
            }
        }
        const QString cached = m_coverLoader->cachedCoverPath(filePath, spec);
        if (!cached.isEmpty()) {
            item->setData(QVariant::fromValue(QPixmap(cached)), CoverRole);
            item->setData(CoverLoader::isGeneratedCoverPath(cached), GeneratedCoverRole);
        } else {
            m_coverLoader->requestCover(filePath, spec);
        }
    }
    QStringList tooltipLines = {joinCompact({entry.format, shownTags.mid(0, 2).join(QStringLiteral(" · "))}), progressTooltipLine(entry.progress), tooltipDescription};
    tooltipLines.append(entry.detailLines);
    item->setToolTip(libraryTileTooltip(entry.title, tooltipLines));
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
        const QString relation = index.data(PaperLibrarySectionedModel::RelationHintRole).toString();
        if (!relation.isEmpty()) {
            return {relation, true};
        }
        const QString intent = index.data(PaperLibrarySectionedModel::ShelfIntentRole).toString();
        if (!intent.isEmpty()) {
            return {intent, true};
        }
        const QString priority = index.data(PaperLibrarySectionedModel::PriorityHintRole).toString();
        if (!priority.isEmpty()) {
            return {priority, true};
        }
        return {QStringList(index.data(PaperLibrarySectionedModel::TopicTagsRole).toStringList().mid(0, 2)).join(QStringLiteral(" · ")), true};
    }
    // Only a tile actually showing a generated card gives up its title
    // caption — the card displays the title as the artwork already
    const bool generatedCardShowing = index.data(GeneratedCoverRole).toBool() && !index.data(CoverRole).value<QPixmap>().isNull();
    if (!generatedCardShowing) {
        return {index.data(Qt::DisplayRole).toString(), false};
    }
    const QString description = index.data(DescriptionRole).toString();
    if (!description.isEmpty()) {
        return {description, true};
    }
    return {QStringList(index.data(TagsRole).toStringList().mid(0, 2)).join(QStringLiteral(" · ")), true};
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
        prebuildCorpusShelves();
        showShelfGuide();
        requestCorpusCovers();
        scheduleCorpusPrewarm();
    });

    // The non-modal notice slot under the header ("Loading catalog…",
    // "PDF not local — …"); most of the time it is hidden
    m_paperNotice = new QLabel(this);
    m_paperNotice->setMargin(8);
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
    if (m_shelfRenderTimer) {
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
        syncViewModeButton(); // the arrangement is a per-shelf choice
        if (m_paperNotice) {
            m_paperNotice->hide();
        }
    }
    selectFirstTile();
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

void LibraryView::showShelfGuide()
{
    const Shelf shelf = activeShelf();
    if (!usesCorpusList(shelf) || !m_paperNotice) {
        if (m_paperNotice) {
            m_paperNotice->hide();
        }
        return;
    }
    const QString guide = corpusShelfGuide(shelf, paperSectionMode(shelf));
    if (guide.isEmpty()) {
        m_paperNotice->hide();
        return;
    }
    showPaperNotice(guide, false);
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

    // Keep each pass bounded. Generated corpus cards are immediately useful,
    // while real local PDF covers are warmed opportunistically after the
    // highest-ranked first screens.
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
        const QString pdfPath = index.data(PaperLibrarySectionedModel::PdfPathRole).toString();
        if (pdfPath.isEmpty()) {
            continue;
        }
        const CoverGenerator::CoverSpec spec = corpusCoverSpecForIndex(index);
        const QString cached = m_coverLoader->cachedCoverPath(pdfPath, spec);
        if (!cached.isEmpty()) {
            sections->setCoverForPath(pdfPath, QVariant::fromValue(QPixmap(cached)), CoverLoader::isGeneratedCoverPath(cached));
        } else {
            m_coverLoader->requestCover(pdfPath, spec);
        }
        ++requested;
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
    chip.setColor(QPalette::Window, blendColors(palette().color(QPalette::Base), palette().color(QPalette::Highlight), 0.10));
    QColor noticeColor = palette().color(QPalette::Text);
    noticeColor.setAlphaF(0.75);
    chip.setColor(QPalette::WindowText, noticeColor);
    m_paperNotice->setPalette(chip);
    m_paperNotice->setAutoFillBackground(true);
    m_paperNotice->setText(text);
    m_paperNotice->show();
    if (autoHide) {
        m_paperNoticeTimer->start();
    } else {
        m_paperNoticeTimer->stop();
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
        QStringList tags = index.data(PaperLibrarySectionedModel::TopicTagsRole).toStringList();
        tags.removeAll(QString());
        if (!title.isEmpty()) {
            m_store->setTitle(url, title);
        }
        if (!tags.isEmpty()) {
            m_store->setTags(url, tags);
        }
        const QString description = joinCompact({detail, priority, intent, relation});
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
        QAction *relatedAction = menu.addAction(relatedQuery.isEmpty() ? i18nc("@action:inmenu on a corpus tile", "Find Related")
                                                                       : i18nc("@action:inmenu on a corpus tile", "Find Related: %1", relatedLabel));
        relatedAction->setEnabled(!relatedQuery.isEmpty());
        QAction *downrankAction = menu.addAction(downranked ? i18nc("@action:inmenu on a corpus tile", "Undo Thumbs Down") : i18nc("@action:inmenu on a corpus tile", "Thumbs Down"));
        QAction *clearSearchAction = menu.addAction(i18nc("@action:inmenu on a corpus tile", "Clear Search"));
        clearSearchAction->setEnabled(!searchQuery().isEmpty());
        menu.addSeparator();
#if defined(Q_OS_MACOS)
        QAction *revealAction = menu.addAction(i18nc("@action:inmenu on a corpus tile", "Show in Finder"));
#else
        QAction *revealAction = menu.addAction(i18nc("@action:inmenu on a corpus tile", "Open Containing Folder"));
#endif
        revealAction->setEnabled(!pdfPath.isEmpty());

        const QAction *chosen = menu.exec(m_grid->viewport()->mapToGlobal(pos));
        if (chosen == relatedAction) {
            setSearchQuery(relatedQuery);
        } else if (chosen == downrankAction) {
            if (sections) {
                sections->setDownranked(index, !downranked);
                QTimer::singleShot(0, this, &LibraryView::requestCorpusCovers);
            }
        } else if (chosen == clearSearchAction) {
            setSearchQuery(QString());
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

    QMenu menu(this);
    QAction *pinAction = menu.addAction(pinned ? i18nc("@action:inmenu on a library tile", "Unpin") : i18nc("@action:inmenu on a library tile", "Pin"));
    QAction *downrankAction = menu.addAction(downranked ? i18nc("@action:inmenu on a library tile", "Undo Thumbs Down") : i18nc("@action:inmenu on a library tile", "Thumbs Down"));
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
