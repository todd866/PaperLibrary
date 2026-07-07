/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QListView>
#include <QLineEdit>
#include <QPainter>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyleOptionViewItem>
#include <QTabBar>
#include <QTemporaryDir>
#include <QToolButton>

#include <KConfigGroup>
#include <KSharedConfig>

#include <memory>

#include "../shell/libraryview.h"
#include "../shell/librarystore.h"
#include "../shell/paperlibrarymodel.h"

namespace
{
QByteArray corpusRecordLine()
{
    QJsonObject object;
    object.insert(QStringLiteral("slug"), QStringLiteral("10-9999-synthetic-mnd-tiles"));
    object.insert(QStringLiteral("doi"), QStringLiteral("10.9999/synthetic.mnd.tiles"));
    object.insert(QStringLiteral("md5"), QString());
    object.insert(QStringLiteral("pmid"), QString());
    object.insert(QStringLiteral("cite_key"), QStringLiteral("sample2026mndtiles"));
    object.insert(QStringLiteral("title"), QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));
    object.insert(QStringLiteral("authors"), QStringLiteral("Casey Clinician"));
    object.insert(QStringLiteral("year"), QStringLiteral("2026"));
    object.insert(QStringLiteral("journal"), QStringLiteral("Journal of Synthetic Neurology"));
    object.insert(QStringLiteral("bytes"), 98765);
    object.insert(QStringLiteral("source"), QStringLiteral("md-project-review-set"));
    object.insert(QStringLiteral("added_ts"), QStringLiteral("2026-04-01T00:00:00+00:00"));
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray workCorpusRecordLine()
{
    QJsonObject object;
    object.insert(QStringLiteral("slug"), QStringLiteral("10-9999-synthetic-beyond-bayes-work"));
    object.insert(QStringLiteral("doi"), QStringLiteral("10.9999/synthetic.beyond.bayes.work"));
    object.insert(QStringLiteral("md5"), QString());
    object.insert(QStringLiteral("pmid"), QString());
    object.insert(QStringLiteral("cite_key"), QStringLiteral("sample2026beyondbayes"));
    object.insert(QStringLiteral("title"), QStringLiteral("Beyond Bayes Revision Notes for High-Dimensional Inference"));
    object.insert(QStringLiteral("authors"), QStringLiteral("Robin Reviewer"));
    object.insert(QStringLiteral("year"), QStringLiteral("2026"));
    object.insert(QStringLiteral("journal"), QStringLiteral("Synthetic Methods"));
    object.insert(QStringLiteral("bytes"), 87654);
    object.insert(QStringLiteral("source"), QStringLiteral("peer review major revisions"));
    object.insert(QStringLiteral("added_ts"), QStringLiteral("2026-05-01T00:00:00+00:00"));
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray bayesianFalsePositiveRecordLine()
{
    QJsonObject object;
    object.insert(QStringLiteral("slug"), QStringLiteral("10-9999-synthetic-bayesian-clinical-false-positive"));
    object.insert(QStringLiteral("doi"), QStringLiteral("10.9999/synthetic.bayesian.clinical"));
    object.insert(QStringLiteral("md5"), QString());
    object.insert(QStringLiteral("pmid"), QString());
    object.insert(QStringLiteral("cite_key"), QStringLiteral("sample2026bayesianclinical"));
    object.insert(QStringLiteral("title"), QStringLiteral("Bayesian prediction model for hospital readmission"));
    object.insert(QStringLiteral("authors"), QStringLiteral("Pat Statistician"));
    object.insert(QStringLiteral("year"), QStringLiteral("2026"));
    object.insert(QStringLiteral("journal"), QStringLiteral("Clinical Epidemiology"));
    object.insert(QStringLiteral("bytes"), 76543);
    object.insert(QStringLiteral("source"), QStringLiteral("unpaywall"));
    object.insert(QStringLiteral("added_ts"), QStringLiteral("2026-05-02T00:00:00+00:00"));
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray biosystemsFalsePositiveRecordLine()
{
    QJsonObject object;
    object.insert(QStringLiteral("slug"), QStringLiteral("10-9999-synthetic-biosystems-false-positive"));
    object.insert(QStringLiteral("doi"), QStringLiteral("10.9999/synthetic.biosystems"));
    object.insert(QStringLiteral("md5"), QString());
    object.insert(QStringLiteral("pmid"), QString());
    object.insert(QStringLiteral("cite_key"), QStringLiteral("sample2026biosystems"));
    object.insert(QStringLiteral("title"), QStringLiteral("Generic BioSystems article about cell division"));
    object.insert(QStringLiteral("authors"), QStringLiteral("Sam Systems"));
    object.insert(QStringLiteral("year"), QStringLiteral("2026"));
    object.insert(QStringLiteral("journal"), QStringLiteral("BioSystems"));
    object.insert(QStringLiteral("bytes"), 65432);
    object.insert(QStringLiteral("source"), QStringLiteral("aa_fast_download"));
    object.insert(QStringLiteral("added_ts"), QStringLiteral("2026-05-03T00:00:00+00:00"));
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

void writeMndFocusManifest(const QString &corpusDir)
{
    QDir(corpusDir).mkpath(QStringLiteral("focus/MND"));
    QJsonObject object;
    object.insert(QStringLiteral("id"), QStringLiteral("10-9999-synthetic-mnd-tiles"));
    object.insert(QStringLiteral("title"), QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));
    object.insert(QStringLiteral("kind"), QStringLiteral("pdf"));
    object.insert(QStringLiteral("section"), QStringLiteral("00-current"));
    object.insert(QStringLiteral("reason"), QStringLiteral("Core project paper; biomarker framing"));
    QJsonArray array;
    array.append(object);
    QFile manifest(QDir(corpusDir).filePath(QStringLiteral("focus/MND/manifest.json")));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
    manifest.close();
}

void writeEmptyFocusManifest(const QString &corpusDir, const QString &shelf)
{
    QDir(corpusDir).mkpath(QStringLiteral("focus/") + shelf);
    QFile manifest(QDir(corpusDir).filePath(QStringLiteral("focus/") + shelf + QStringLiteral("/manifest.json")));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(QJsonArray()).toJson(QJsonDocument::Compact));
    manifest.close();
}

QString writeFocusThumbnail(const QString &corpusDir, const QString &shelf, const QString &fileName, const QColor &color)
{
    const QString relativePath = QStringLiteral("thumbnails/") + fileName;
    const QString path = QDir(corpusDir).filePath(QStringLiteral("focus/") + shelf + QLatin1Char('/') + relativePath);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage image(180, 240, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 150));
    painter.drawRoundedRect(QRectF(26, 38, 128, 72), 12, 12);
    painter.setBrush(QColor(60, 30, 20, 180));
    painter.drawEllipse(QRectF(44, 64, 32, 32));
    painter.drawRect(QRectF(86, 68, 64, 8));
    painter.drawRect(QRectF(86, 84, 44, 8));
    painter.end();
    if (!image.save(path)) {
        return QString();
    }
    return relativePath;
}

void writeStarterPackCatalog(const QString &starterDir)
{
    QDir root(starterDir);
    QVERIFY(root.mkpath(QStringLiteral("books")));
    QFile epub(root.filePath(QStringLiteral("books/sample.epub")));
    QVERIFY(epub.open(QIODevice::WriteOnly));
    epub.write("not a real epub; enough for a local fixture path\n");
    epub.close();

    QJsonObject record;
    record.insert(QStringLiteral("slug"), QStringLiteral("starter-sample"));
    record.insert(QStringLiteral("title"), QStringLiteral("Starter Sample Book"));
    record.insert(QStringLiteral("authors"), QStringLiteral("Public Domain Author"));
    record.insert(QStringLiteral("year"), QStringLiteral("1901"));
    record.insert(QStringLiteral("source"), QStringLiteral("Project Gutenberg"));
    record.insert(QStringLiteral("source_id"), QStringLiteral("PG-123"));
    record.insert(QStringLiteral("source_url"), QStringLiteral("https://www.gutenberg.org/ebooks/123"));
    record.insert(QStringLiteral("rights"), QStringLiteral("Public domain in the United States"));
    record.insert(QStringLiteral("format"), QStringLiteral("epub"));
    record.insert(QStringLiteral("epub_path"), QStringLiteral("books/sample.epub"));
    record.insert(QStringLiteral("added_ts"), QStringLiteral("2026-07-05T00:00:00+00:00"));
    record.insert(QStringLiteral("tags"), QJsonArray({QStringLiteral("Classic"), QStringLiteral("Public Domain")}));

    QFile catalog(root.filePath(QStringLiteral("catalog.jsonl")));
    QVERIFY(catalog.open(QIODevice::WriteOnly));
    catalog.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
    catalog.write("\n");
    catalog.close();
}

bool sectionedModelsContainTitle(LibraryView *view, const QString &title)
{
    const QList<PaperLibrarySectionedModel *> models = view->findChildren<PaperLibrarySectionedModel *>();
    for (PaperLibrarySectionedModel *model : models) {
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->data(model->index(row), Qt::DisplayRole).toString() == title) {
                return true;
            }
        }
    }
    return false;
}

int tabIndexForText(QTabBar *tabs, const QString &text)
{
    for (int index = 0; index < tabs->count(); ++index) {
        if (tabs->tabText(index) == text) {
            return index;
        }
    }
    return -1;
}

QImage paintedTileImage(LibraryView *view, QListView *grid, const QModelIndex &index)
{
    QStyleOptionViewItem option;
    option.initFrom(grid);
    option.state |= QStyle::State_Enabled | QStyle::State_Active;
    option.widget = grid;
    QSize tileSize = grid->gridSize();
    if (!tileSize.isValid() || tileSize.isEmpty()) {
        tileSize = grid->itemDelegate()->sizeHint(option, index);
    }
    option.rect = QRect(QPoint(0, 0), tileSize);
    option.font = grid->font();
    option.fontMetrics = QFontMetrics(grid->font());

    QPixmap tile(tileSize);
    tile.fill(view->palette().color(QPalette::Base));
    QPainter painter(&tile);
    grid->itemDelegate()->paint(&painter, option, index);
    painter.end();
    return tile.toImage();
}

int changedPixelsBetween(const QImage &left, const QImage &right)
{
    const int width = qMin(left.width(), right.width());
    const int height = qMin(left.height(), right.height());
    int changed = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (left.pixelColor(x, y) != right.pixelColor(x, y)) {
                ++changed;
            }
        }
    }
    return changed;
}

int darkMaskDifference(const QImage &left, const QImage &right, const QRect &region)
{
    const QRect bounded = region.intersected(left.rect()).intersected(right.rect());
    int changed = 0;
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const bool leftDark = left.pixelColor(x, y).lightness() < 112;
            const bool rightDark = right.pixelColor(x, y).lightness() < 112;
            if (leftDark != rightDark) {
                ++changed;
            }
        }
    }
    return changed;
}
}

class LibraryViewTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void testInitialStartupUsesTileBeforeRefresh();
    void testCorpusShelvesUseTileGrid();
    void testCorpusShelfModelsPersistAcrossSwitches();
    void testDomainShelvesRequireFocusManifest();
    void testWorkShelfGeneratedCardsAreVisible();
    void testWorkShelfGeneratedCardsVaryByDocumentTitle();
    void testLocalCorpusPdfPrefersFileRenderOverManifestThumbnail();
    void testExtractedCorpusThumbnailOverridesLocalPdfRender();
    void testGeneratedCorpusCoverKeepsSemanticThumbnail();
    void testBooksShelfStaysWithLocalEbooks();
    void testBooksShelfFetchesMoreRowsOnScroll();
    void testFinishedReadingShelfKeepsCompletedBooksOutOfActiveFeeds();
    void testLocalBookClassificationIgnoresStaleTags();
    void testBookTileDisplayMetadataAvoidsGenericBookOnly();
    void testLocalPdfTileUsesCorpusMetadata();
    void testTilesSelectOnClickAndOpenOnDoubleClick();
    void testDraggingTileDownDownranksLocalBook();
    void testCorpusActivationStoresCuratedMetadata();
    void testStarterPackEmptySetupTile();
    void testStarterPackInstalledMetadataTooltip();
    void testCorpusShelfPrebuildsBeforeFirstSwitch();
    void testCorpusSelectionDoesNotShowPersistentContextStrip();
    void testEmptyCorpusShelfUsesTile();
    void testRapidCorpusShelfSwitchingDoesNotReload();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
};

void LibraryViewTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void LibraryViewTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());

    const QString configPath = m_dir->filePath(QStringLiteral("paperlibraryrc"));
    qputenv("PAPERLIBRARY_CONFIG_PATH", QFile::encodeName(configPath));

    QFile catalog(m_dir->filePath(QStringLiteral("catalog.jsonl")));
    QVERIFY(catalog.open(QIODevice::WriteOnly));
    catalog.write(corpusRecordLine());
    catalog.write(workCorpusRecordLine());
    catalog.write(bayesianFalsePositiveRecordLine());
    catalog.write(biosystemsFalsePositiveRecordLine());
    catalog.close();

    QDir(m_dir->path()).mkpath(QStringLiteral("pdfs"));
    QFile pdf(m_dir->filePath(QStringLiteral("pdfs/10-9999-synthetic-mnd-tiles.pdf")));
    QVERIFY(pdf.open(QIODevice::WriteOnly));
    pdf.write("%PDF-1.4\n");
    pdf.close();

    KConfigGroup general = KSharedConfig::openConfig(configPath, KConfig::SimpleConfig)->group(QStringLiteral("General"));
    general.writeEntry("PaperLibraryPath", m_dir->path());
    general.writeEntry("ScanAppleBooksOnStartup", false);
    general.sync();
}

void LibraryViewTest::testInitialStartupUsesTileBeforeRefresh()
{
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, false);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QVERIFY(grid->model());
    QCOMPARE(grid->model()->rowCount(), 1);
    const QModelIndex index = grid->model()->index(0, 0);
    QCOMPARE(index.data(Qt::DisplayRole).toString(), QStringLiteral("Opening PaperLibrary"));
    QVERIFY(!index.data(LibraryView::UrlRole).toUrl().isValid());
    QCOMPARE(grid->viewMode(), QListView::IconMode);
    QVERIFY(grid->isWrapping());
}

void LibraryViewTest::testCorpusShelvesUseTileGrid()
{
    writeMndFocusManifest(m_dir->path());
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, true);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);

    const int mndTab = tabIndexForText(shelves, QStringLiteral("MND"));
    QVERIFY(mndTab >= 0);
    shelves->setCurrentIndex(mndTab);
    QTRY_VERIFY(grid->model());
    QTRY_COMPARE(grid->model()->rowCount(), 1);

    QCOMPARE(grid->viewMode(), QListView::IconMode);
    QCOMPARE(grid->flow(), QListView::LeftToRight);
    QVERIFY(grid->isWrapping());
    QVERIFY(grid->uniformItemSizes());
    QVERIFY(grid->gridSize().width() >= 160);
    QVERIFY(grid->gridSize().height() >= 220);
    QVERIFY(!grid->model()->index(0, 0).data(Qt::DisplayRole).toString().isEmpty());
}

void LibraryViewTest::testCorpusShelfModelsPersistAcrossSwitches()
{
    writeMndFocusManifest(m_dir->path());
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, true);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);

    const int mndTab = tabIndexForText(shelves, QStringLiteral("MND"));
    const int workTab = tabIndexForText(shelves, QStringLiteral("Work"));
    QVERIFY(mndTab >= 0);
    QVERIFY(workTab >= 0);

    shelves->setCurrentIndex(mndTab);
    QTRY_COMPARE(grid->model()->rowCount(), 1);
    QAbstractItemModel *mndModel = grid->model();

    shelves->setCurrentIndex(workTab);
    QTRY_VERIFY(grid->model() != mndModel);
    QTRY_COMPARE(grid->model()->rowCount(), 1);
    QAbstractItemModel *workModel = grid->model();
    QVERIFY(workModel != mndModel);

    shelves->setCurrentIndex(mndTab);
    QTRY_COMPARE(grid->model(), mndModel);
    shelves->setCurrentIndex(workTab);
    QTRY_COMPARE(grid->model(), workModel);
}

void LibraryViewTest::testDomainShelvesRequireFocusManifest()
{
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, true);

    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);
    QVERIFY(tabIndexForText(shelves, QStringLiteral("Recent")) >= 0);
    QVERIFY(tabIndexForText(shelves, QStringLiteral("Books")) >= 0);
    QVERIFY(tabIndexForText(shelves, QStringLiteral("Finished")) >= 0);
    QVERIFY(tabIndexForText(shelves, QStringLiteral("Fiction")) >= 0);
    QVERIFY(tabIndexForText(shelves, QStringLiteral("Non-fiction")) >= 0);
    QVERIFY(tabIndexForText(shelves, QStringLiteral("Work")) >= 0);
    QVERIFY(tabIndexForText(shelves, QStringLiteral("Starter Pack")) >= 0);
    QVERIFY(tabIndexForText(shelves, QStringLiteral("Papers")) >= 0);
    QCOMPARE(tabIndexForText(shelves, QStringLiteral("Textbooks")), -1);
    QCOMPARE(tabIndexForText(shelves, QStringLiteral("Medicine")), -1);
    QCOMPARE(tabIndexForText(shelves, QStringLiteral("MND")), -1);
}

void LibraryViewTest::testWorkShelfGeneratedCardsAreVisible()
{
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, true);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);

    const int workTab = tabIndexForText(shelves, QStringLiteral("Work"));
    QVERIFY(workTab >= 0);
    shelves->setCurrentIndex(workTab);
    QTRY_COMPARE(grid->model()->rowCount(), 1);
    const QModelIndex index = grid->model()->index(0, 0);
    QVERIFY(index.data(PaperLibrarySectionedModel::SourceRowRole).isValid());
    QVERIFY(index.data(PaperLibrarySectionedModel::CoverPixmapRole).value<QPixmap>().isNull());
    const LibraryView::TileCaption caption = LibraryView::tileCaption(index);
    QVERIFY(caption.secondary);
    QVERIFY2(caption.text.contains(QStringLiteral("Robin Reviewer")), qPrintable(caption.text));
    QVERIFY2(caption.text.contains(QStringLiteral("2026")), qPrintable(caption.text));
    QVERIFY2(caption.text.contains(QStringLiteral("Synthetic Methods")), qPrintable(caption.text));
    const QString relatedQuery = index.data(PaperLibrarySectionedModel::RelatedQueryRole).toString();
    QVERIFY(!relatedQuery.isEmpty());

    QStyleOptionViewItem option;
    option.initFrom(grid);
    option.state |= QStyle::State_Enabled | QStyle::State_Active;
    option.widget = grid;
    QSize tileSize = grid->gridSize();
    if (!tileSize.isValid() || tileSize.isEmpty()) {
        tileSize = grid->itemDelegate()->sizeHint(option, index);
    }
    if (!tileSize.isValid() || tileSize.isEmpty()) {
        tileSize = QSize(172, 232);
    }
    option.rect = QRect(QPoint(0, 0), tileSize);
    option.font = grid->font();
    option.fontMetrics = QFontMetrics(grid->font());
    QVERIFY(tileSize.width() >= 220);
    QVERIFY(tileSize.height() >= 280);

    QPixmap tile(tileSize);
    const QColor base = view.palette().color(QPalette::Base);
    tile.fill(base);
    QPainter painter(&tile);
    grid->itemDelegate()->paint(&painter, option, index);
    painter.end();

    const QImage image = tile.toImage();
    int changedCoverPixels = 0;
    const int bottom = qMin(image.height(), 156);
    const int right = qMin(image.width(), 140);
    for (int y = 12; y < bottom; ++y) {
        for (int x = 20; x < right; ++x) {
            if (image.pixelColor(x, y) != base) {
                ++changedCoverPixels;
            }
        }
    }
    QVERIFY2(changedCoverPixels > 4000, qPrintable(QString::number(changedCoverPixels)));

    QLineEdit *searchField = view.findChild<QLineEdit *>();
    QVERIFY(searchField);
    grid->setCurrentIndex(index);
    QVERIFY(view.showAdjacentDocumentsForCurrentTile());
    QTRY_COMPARE(searchField->text(), relatedQuery);
    QToolButton *resultMode = view.findChild<QToolButton *>(QStringLiteral("corpusResultModeButton"));
    QVERIFY(resultMode);
    QTRY_VERIFY(!resultMode->isHidden());
    QVERIFY2(resultMode->text().contains(QStringLiteral("Related")), qPrintable(resultMode->text()));
    const QList<QLabel *> labels = view.findChildren<QLabel *>();
    QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](const QLabel *label) {
        return !label->isHidden() && label->text().contains(QStringLiteral("Adjacent documents for"));
    }));
    resultMode->click();
    QTRY_VERIFY(resultMode->isHidden());
    QTRY_VERIFY(searchField->text().isEmpty());
}

void LibraryViewTest::testWorkShelfGeneratedCardsVaryByDocumentTitle()
{
    QFile catalog(m_dir->filePath(QStringLiteral("catalog.jsonl")));
    QVERIFY(catalog.open(QIODevice::Append));
    QJsonObject response;
    response.insert(QStringLiteral("slug"), QStringLiteral("10-9999-synthetic-reviewer-response"));
    response.insert(QStringLiteral("doi"), QStringLiteral("10.9999/synthetic.reviewer.response"));
    response.insert(QStringLiteral("title"), QStringLiteral("Reviewer Response for High-Dimensional Coherence"));
    response.insert(QStringLiteral("authors"), QStringLiteral("Robin Reviewer"));
    response.insert(QStringLiteral("year"), QStringLiteral("2026"));
    response.insert(QStringLiteral("journal"), QStringLiteral("Synthetic Methods"));
    response.insert(QStringLiteral("bytes"), 45678);
    response.insert(QStringLiteral("source"), QStringLiteral("peer review major revisions"));
    response.insert(QStringLiteral("added_ts"), QStringLiteral("2026-05-04T00:00:00+00:00"));
    catalog.write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    catalog.write("\n");
    catalog.close();

    QDir(m_dir->path()).mkpath(QStringLiteral("focus/Work"));
    QJsonArray manifest;
    auto appendWorkEntry = [&manifest](const QString &id, const QString &title, const QString &reason) {
        QJsonObject entry;
        entry.insert(QStringLiteral("id"), id);
        entry.insert(QStringLiteral("title"), title);
        entry.insert(QStringLiteral("kind"), QStringLiteral("pdf"));
        entry.insert(QStringLiteral("authors"), QStringLiteral("Robin Reviewer"));
        entry.insert(QStringLiteral("year"), QStringLiteral("2026"));
        entry.insert(QStringLiteral("journal"), QStringLiteral("Synthetic Methods"));
        entry.insert(QStringLiteral("source"), QStringLiteral("peer review major revisions"));
        entry.insert(QStringLiteral("reason"), reason);
        entry.insert(QStringLiteral("shelf"), QStringLiteral("Work"));
        entry.insert(QStringLiteral("section"), QStringLiteral("00-beyond-bayes-revision"));
        manifest.append(entry);
    };
    appendWorkEntry(QStringLiteral("10-9999-synthetic-beyond-bayes-work"),
                    QStringLiteral("Beyond Bayes Revision Notes for High-Dimensional Inference"),
                    QStringLiteral("manuscript draft; Bayesian/FEP literature"));
    appendWorkEntry(QStringLiteral("10-9999-synthetic-reviewer-response"),
                    QStringLiteral("Reviewer Response for High-Dimensional Coherence"),
                    QStringLiteral("response to reviewers; major revision"));

    QFile manifestFile(QDir(m_dir->path()).filePath(QStringLiteral("focus/Work/manifest.json")));
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    manifestFile.close();

    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, true);
    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);

    const int workTab = tabIndexForText(shelves, QStringLiteral("Work"));
    QVERIFY(workTab >= 0);
    shelves->setCurrentIndex(workTab);
    QTRY_COMPARE(grid->model()->rowCount(), 2);

    auto findTitleIndex = [grid](const QString &title) {
        for (int row = 0; row < grid->model()->rowCount(); ++row) {
            const QModelIndex index = grid->model()->index(row, 0);
            if (index.data(Qt::DisplayRole).toString() == title) {
                return index;
            }
        }
        return QModelIndex();
    };

    const QModelIndex manuscriptIndex = findTitleIndex(QStringLiteral("Beyond Bayes Revision Notes for High-Dimensional Inference"));
    const QModelIndex responseIndex = findTitleIndex(QStringLiteral("Reviewer Response for High-Dimensional Coherence"));
    QVERIFY(manuscriptIndex.isValid());
    QVERIFY(responseIndex.isValid());
    QCOMPARE(manuscriptIndex.data(PaperLibraryModel::AuthorsRole).toString(), responseIndex.data(PaperLibraryModel::AuthorsRole).toString());
    QCOMPARE(manuscriptIndex.data(PaperLibraryModel::YearRole).toString(), responseIndex.data(PaperLibraryModel::YearRole).toString());
    QCOMPARE(manuscriptIndex.data(PaperLibraryModel::JournalRole).toString(), responseIndex.data(PaperLibraryModel::JournalRole).toString());

    const QImage manuscriptTile = paintedTileImage(&view, grid, manuscriptIndex);
    const QImage responseTile = paintedTileImage(&view, grid, responseIndex);
    QVERIFY2(changedPixelsBetween(manuscriptTile, responseTile) > 6000, "different work items need visually distinct generated cards");
    QVERIFY2(darkMaskDifference(manuscriptTile, responseTile, QRect(28, 126, 172, 54)) > 180,
             "title region must not collapse distinct Work items to the same author/year citation label");
}

void LibraryViewTest::testLocalCorpusPdfPrefersFileRenderOverManifestThumbnail()
{
    QDir(m_dir->path()).mkpath(QStringLiteral("focus/MND"));
    const QString thumbnail = writeFocusThumbnail(m_dir->path(), QStringLiteral("MND"), QStringLiteral("mnd-neurofilament.png"), QColor(220, 78, 42));
    QVERIFY(!thumbnail.isEmpty());

    QJsonObject object;
    object.insert(QStringLiteral("id"), QStringLiteral("10-9999-synthetic-mnd-tiles"));
    object.insert(QStringLiteral("title"), QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));
    object.insert(QStringLiteral("kind"), QStringLiteral("pdf"));
    object.insert(QStringLiteral("section"), QStringLiteral("00-current"));
    object.insert(QStringLiteral("reason"), QStringLiteral("Core project paper; biomarker framing"));
    object.insert(QStringLiteral("thumbnail"), thumbnail);
    QJsonArray array;
    array.append(object);
    QFile manifest(QDir(m_dir->path()).filePath(QStringLiteral("focus/MND/manifest.json")));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
    manifest.close();

    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, true);
    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);
    PaperLibraryModel *paperModel = view.findChild<PaperLibraryModel *>();
    QVERIFY(paperModel);
    QTRY_VERIFY(paperModel->isLoaded());

    const int mndTab = tabIndexForText(shelves, QStringLiteral("MND"));
    QVERIFY(mndTab >= 0);
    shelves->setCurrentIndex(mndTab);
    QTRY_COMPARE(grid->model()->rowCount(), 1);
    PaperLibrarySectionedModel *sections = qobject_cast<PaperLibrarySectionedModel *>(grid->model());
    QVERIFY(sections);
    const QModelIndex index = sections->index(0, 0);
    QCOMPARE(index.data(PaperLibrarySectionedModel::ThumbnailPathRole).toString(),
             QDir(m_dir->path()).filePath(QStringLiteral("focus/MND/") + thumbnail));
    // The manifest still exposes a thumbnail path, but because the PDF is
    // local the app must not install that decorative asset as the tile cover
    // ahead of the file-derived render.
    const QPixmap cover = index.data(PaperLibrarySectionedModel::CoverPixmapRole).value<QPixmap>();
    if (!cover.isNull()) {
        const QImage image = cover.toImage();
        int assetPixels = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.red() > 170 && pixel.green() > 40 && pixel.green() < 120 && pixel.blue() < 80) {
                    ++assetPixels;
                }
            }
        }
        QVERIFY2(assetPixels < image.width() * image.height() / 4, qPrintable(QString::number(assetPixels)));
    }
}

void LibraryViewTest::testExtractedCorpusThumbnailOverridesLocalPdfRender()
{
    QDir(m_dir->path()).mkpath(QStringLiteral("focus/MND"));
    const QString thumbnail = writeFocusThumbnail(m_dir->path(), QStringLiteral("MND"), QStringLiteral("mnd-extracted.png"), QColor(220, 78, 42));
    QVERIFY(!thumbnail.isEmpty());

    QJsonObject object;
    object.insert(QStringLiteral("id"), QStringLiteral("10-9999-synthetic-mnd-tiles"));
    object.insert(QStringLiteral("title"), QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));
    object.insert(QStringLiteral("kind"), QStringLiteral("pdf"));
    object.insert(QStringLiteral("section"), QStringLiteral("00-current"));
    object.insert(QStringLiteral("reason"), QStringLiteral("Core project paper; biomarker framing"));
    object.insert(QStringLiteral("thumbnail"), thumbnail);
    object.insert(QStringLiteral("thumbnail_source"), QStringLiteral("paperlibrary-file-extracted"));
    QJsonArray array;
    array.append(object);
    QFile manifest(QDir(m_dir->path()).filePath(QStringLiteral("focus/MND/manifest.json")));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
    manifest.close();

    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, true);
    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);
    PaperLibraryModel *paperModel = view.findChild<PaperLibraryModel *>();
    QVERIFY(paperModel);
    QTRY_VERIFY(paperModel->isLoaded());

    const int mndTab = tabIndexForText(shelves, QStringLiteral("MND"));
    QVERIFY(mndTab >= 0);
    shelves->setCurrentIndex(mndTab);
    QTRY_COMPARE(grid->model()->rowCount(), 1);
    PaperLibrarySectionedModel *sections = qobject_cast<PaperLibrarySectionedModel *>(grid->model());
    QVERIFY(sections);
    const QModelIndex index = sections->index(0, 0);
    QCOMPARE(index.data(PaperLibrarySectionedModel::ThumbnailSourceRole).toString(), QStringLiteral("paperlibrary-file-extracted"));
    QTRY_VERIFY(!index.data(PaperLibrarySectionedModel::CoverPixmapRole).value<QPixmap>().isNull());

    const QImage image = index.data(PaperLibrarySectionedModel::CoverPixmapRole).value<QPixmap>().toImage();
    int assetPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > 170 && pixel.green() > 40 && pixel.green() < 120 && pixel.blue() < 80) {
                ++assetPixels;
            }
        }
    }
    QVERIFY2(assetPixels > image.width() * image.height() / 3, qPrintable(QString::number(assetPixels)));
}

void LibraryViewTest::testGeneratedCorpusCoverKeepsSemanticThumbnail()
{
    writeMndFocusManifest(m_dir->path());
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, false);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);
    PaperLibraryModel *paperModel = view.findChild<PaperLibraryModel *>();
    QVERIFY(paperModel);
    QTRY_VERIFY(paperModel->isLoaded());

    const int mndTab = tabIndexForText(shelves, QStringLiteral("MND"));
    QVERIFY(mndTab >= 0);
    shelves->setCurrentIndex(mndTab);
    QTRY_COMPARE(grid->model()->rowCount(), 1);
    QTRY_COMPARE(grid->model()->index(0, 0).data(Qt::DisplayRole).toString(), QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));

    PaperLibrarySectionedModel *sections = qobject_cast<PaperLibrarySectionedModel *>(grid->model());
    QVERIFY(sections);
    const QModelIndex index = sections->index(0, 0);
    const QString path = sections->resolvePath(index);
    QVERIFY(!path.isEmpty());

    QPixmap generated(QSize(96, 96));
    generated.fill(QColor(255, 0, 255));
    sections->setCoverForPath(path, QVariant::fromValue(generated), true);
    QVERIFY(index.data(PaperLibrarySectionedModel::GeneratedCoverRole).toBool());

    QStyleOptionViewItem option;
    option.initFrom(grid);
    option.state |= QStyle::State_Enabled | QStyle::State_Active;
    option.widget = grid;
    QSize tileSize = grid->gridSize();
    if (!tileSize.isValid() || tileSize.isEmpty()) {
        tileSize = grid->itemDelegate()->sizeHint(option, index);
    }
    option.rect = QRect(QPoint(0, 0), tileSize);
    option.font = grid->font();
    option.fontMetrics = QFontMetrics(grid->font());

    QPixmap tile(tileSize);
    tile.fill(view.palette().color(QPalette::Base));
    QPainter painter(&tile);
    grid->itemDelegate()->paint(&painter, option, index);
    painter.end();

    const QImage image = tile.toImage();
    int magentaPixels = 0;
    int nonBasePixels = 0;
    const QColor base = view.palette().color(QPalette::Base);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel != base) {
                ++nonBasePixels;
            }
            if (pixel.red() > 240 && pixel.green() < 25 && pixel.blue() > 240) {
                ++magentaPixels;
            }
        }
    }
    QVERIFY2(nonBasePixels > 4000, qPrintable(QString::number(nonBasePixels)));
    QVERIFY2(magentaPixels < 200, qPrintable(QString::number(magentaPixels)));
}

void LibraryViewTest::testBooksShelfStaysWithLocalEbooks()
{
    writeMndFocusManifest(m_dir->path());
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, true);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);

    const int booksTab = tabIndexForText(shelves, QStringLiteral("Books"));
    const int mndTab = tabIndexForText(shelves, QStringLiteral("MND"));
    QVERIFY(booksTab >= 0);
    QVERIFY(mndTab >= 0);

    shelves->setCurrentIndex(booksTab);
    QVERIFY(grid->model());
    QCOMPARE(grid->model()->rowCount(), 0);

    shelves->setCurrentIndex(mndTab);
    QTRY_COMPARE(grid->model()->rowCount(), 1);
}

void LibraryViewTest::testBooksShelfFetchesMoreRowsOnScroll()
{
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    static constexpr int BookCount = 130;
    for (int i = 0; i < BookCount; ++i) {
        const QString path = m_dir->filePath(QStringLiteral("books/book-%1.epub").arg(i, 3, 10, QLatin1Char('0')));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a real epub; enough for a local fixture path\n");
        file.close();
        const QUrl url = QUrl::fromLocalFile(path);
        store.setTitle(url, QStringLiteral("Book %1").arg(i, 3, 10, QLatin1Char('0')));
        store.setTags(url, {QStringLiteral("Book")});
    }

    LibraryView view(&store, nullptr, true);
    view.refresh();

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);
    const int booksTab = tabIndexForText(shelves, QStringLiteral("Books"));
    QVERIFY(booksTab >= 0);
    shelves->setCurrentIndex(booksTab);
    QTRY_VERIFY(grid->model());

    const int initialRows = grid->model()->rowCount();
    QVERIFY2(initialRows > 0, "Books shelf should paint an initial tile batch");
    QVERIFY2(initialRows < BookCount, qPrintable(QStringLiteral("initial model loaded every book: %1").arg(initialRows)));
    QCOMPARE(grid->model()->property("paperlibraryTotalRows").toInt(), BookCount);
    QVERIFY(grid->model()->property("paperlibraryHasMoreRows").toBool());

    QScrollBar *bar = grid->verticalScrollBar();
    QVERIFY(bar);
    bar->setRange(0, 1);
    bar->setValue(1);

    QTRY_VERIFY(grid->model()->rowCount() > initialRows);
    QVERIFY(grid->model()->rowCount() <= BookCount);
}

void LibraryViewTest::testFinishedReadingShelfKeepsCompletedBooksOutOfActiveFeeds()
{
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    auto addBook = [this, &store](const QString &fileName, const QString &title, const QDateTime &opened) {
        const QString path = m_dir->filePath(fileName);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return QUrl();
        }
        file.write("not a real epub; enough for a local fixture path\n");
        file.close();
        const QUrl url = QUrl::fromLocalFile(path);
        store.setTitle(url, title);
        store.setTags(url, {QStringLiteral("Book")});
        store.recordOpen(url, opened);
        return url;
    };

    const QUrl active = addBook(QStringLiteral("books/A Game Of Thrones.epub"), QStringLiteral("A Game of Thrones"), QDateTime(QDate(2026, 7, 5), QTime(21, 0)));
    const QUrl finished = addBook(QStringLiteral("books/Master of the Senate.epub"), QStringLiteral("Master of the Senate"), QDateTime(QDate(2026, 7, 6), QTime(8, 40)));
    QVERIFY(active.isValid());
    QVERIFY(finished.isValid());
    store.setFinishedReading(finished, true);

    LibraryView view(&store, nullptr, true);
    view.refresh();

    const QList<QUrl> books = view.shelfUrls(LibraryView::BooksShelf);
    QVERIFY(books.contains(active));
    QVERIFY(!books.contains(finished));
    const QList<QUrl> recent = view.shelfUrls(LibraryView::PdfShelf);
    QVERIFY(recent.contains(active));
    QVERIFY(!recent.contains(finished));
    const QList<QUrl> finishedBooks = view.shelfUrls(LibraryView::FinishedShelf);
    QVERIFY(finishedBooks.contains(finished));
    QVERIFY(!finishedBooks.contains(active));

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);
    const int finishedTab = tabIndexForText(shelves, QStringLiteral("Finished"));
    QVERIFY(finishedTab >= 0);
    shelves->setCurrentIndex(finishedTab);
    QTRY_VERIFY(grid->model());
    QTRY_COMPARE(grid->model()->index(0, 0).data(Qt::DisplayRole).toString(), QStringLiteral("Master of the Senate"));
    QCOMPARE(grid->model()->rowCount(), 1);
    const QModelIndex index = grid->model()->index(0, 0);
    QCOMPARE(index.data(Qt::DisplayRole).toString(), QStringLiteral("Master of the Senate"));
    QCOMPARE(index.data(LibraryView::FinishedReadingRole).toBool(), true);
    QCOMPARE(index.data(LibraryView::FormatRole).toString(), QStringLiteral("EPUB"));
}

void LibraryViewTest::testLocalBookClassificationIgnoresStaleTags()
{
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    auto addBook = [this, &store](const QString &fileName, const QString &title, const QStringList &tags, const QString &description = QString()) {
        const QString path = m_dir->filePath(fileName);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a real epub; enough for a local fixture path\n");
        file.close();
        const QUrl url = QUrl::fromLocalFile(path);
        store.setTitle(url, title);
        store.setTags(url, tags);
        if (!description.isEmpty()) {
            store.setDescription(url, description);
        }
    };
    auto addEpubBundle = [this, &store](const QString &fileName, const QString &title, const QStringList &tags) {
        const QString root = m_dir->filePath(fileName);
        QVERIFY(QDir().mkpath(root + QStringLiteral("/META-INF")));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/OEBPS")));

        QFile container(root + QStringLiteral("/META-INF/container.xml"));
        QVERIFY(container.open(QIODevice::WriteOnly));
        container.write(R"(<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>)");
        container.close();

        QFile opf(root + QStringLiteral("/OEBPS/content.opf"));
        QVERIFY(opf.open(QIODevice::WriteOnly));
        opf.write(R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>1941</dc:title>
    <dc:creator>William M. Christie</dc:creator>
    <dc:date>2016</dc:date>
    <dc:description>&lt;p&gt;&lt;i&gt;1941: The America That Went to War&lt;/i&gt; presents a detailed history of the United States on the eve of World War II, after the Depression.&lt;/p&gt;</dc:description>
  </metadata>
  <manifest>
    <item id="content" href="content.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
</package>)");
        opf.close();

        QFile content(root + QStringLiteral("/OEBPS/content.xhtml"));
        QVERIFY(content.open(QIODevice::WriteOnly));
        content.write("<html/>\n");
        content.close();

        const QUrl url = QUrl::fromLocalFile(root);
        store.setTitle(url, title);
        store.setTags(url, tags);
    };
    auto bookUrl = [this](const QString &fileName) {
        return QUrl::fromLocalFile(m_dir->filePath(fileName));
    };

    const QString warFile = QStringLiteral("books/1941.epub");
    const QString caroFile = QStringLiteral("books/Master of the Senate.epub");
    const QString thronesFile = QStringLiteral("books/A Game Of Thrones.epub");
    addEpubBundle(warFile, QStringLiteral("1941"), {QStringLiteral("Psychiatry"), QStringLiteral("Book")});
    addBook(caroFile, QStringLiteral("Master of the Senate"), {QStringLiteral("Fiction"), QStringLiteral("Book")});
    addBook(thronesFile, QStringLiteral("A Game Of Thrones"), {QStringLiteral("Non-fiction"), QStringLiteral("Book")});
    const QUrl warUrl = bookUrl(warFile);
    const QUrl caroUrl = bookUrl(caroFile);
    const QUrl thronesUrl = bookUrl(thronesFile);

    LibraryView view(&store, nullptr, true);
    view.refresh();

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);
    const int booksTab = tabIndexForText(shelves, QStringLiteral("Books"));
    QVERIFY(booksTab >= 0);
    shelves->setCurrentIndex(booksTab);
    QTRY_VERIFY(grid->model());
    QTRY_VERIFY(grid->model()->rowCount() >= 3);

    auto tagsForTitle = [grid](const QString &title) {
        for (int row = 0; row < grid->model()->rowCount(); ++row) {
            const QModelIndex index = grid->model()->index(row, 0);
            if (index.data(Qt::DisplayRole).toString() == title) {
                return index.data(LibraryView::TagsRole).toStringList();
            }
        }
        return QStringList();
    };

    const QStringList warTags = tagsForTitle(QStringLiteral("1941: The America That Went to War"));
    QVERIFY(warTags.contains(QStringLiteral("Non-fiction")));
    QVERIFY(warTags.contains(QStringLiteral("Book")));
    QVERIFY(!warTags.contains(QStringLiteral("Psychiatry")));
    QCOMPARE(store.metadata(warUrl).tags, warTags);

    const QStringList caroTags = tagsForTitle(QStringLiteral("Master of the Senate"));
    QVERIFY(caroTags.contains(QStringLiteral("Politics")));
    QVERIFY(caroTags.contains(QStringLiteral("Book")));
    QVERIFY(!caroTags.contains(QStringLiteral("Fiction")));
    QCOMPARE(store.metadata(caroUrl).tags, caroTags);

    const QStringList thronesTags = tagsForTitle(QStringLiteral("A Game of Thrones"));
    QVERIFY(thronesTags.contains(QStringLiteral("Fiction")));
    QVERIFY(thronesTags.contains(QStringLiteral("Book")));
    QVERIFY(!thronesTags.contains(QStringLiteral("Non-fiction")));
    QCOMPARE(store.metadata(thronesUrl).tags, thronesTags);
}

void LibraryViewTest::testBookTileDisplayMetadataAvoidsGenericBookOnly()
{
    auto addEpubBundle = [this](const QString &fileName, const QString &title, const QString &creator, const QString &year, const QString &description) {
        const QString root = m_dir->filePath(fileName);
        QVERIFY(QDir().mkpath(root + QStringLiteral("/META-INF")));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/OEBPS")));

        QFile container(root + QStringLiteral("/META-INF/container.xml"));
        QVERIFY(container.open(QIODevice::WriteOnly));
        container.write(R"(<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>)");
        container.close();

        QFile opf(root + QStringLiteral("/OEBPS/content.opf"));
        QVERIFY(opf.open(QIODevice::WriteOnly));
        const QByteArray opfXml = QStringLiteral(R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>%1</dc:title>
    <dc:creator>%2</dc:creator>
    <dc:date>%3</dc:date>
    <dc:description>%4</dc:description>
  </metadata>
  <manifest>
    <item id="content" href="content.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
</package>)")
                                      .arg(title.toHtmlEscaped(), creator.toHtmlEscaped(), year.toHtmlEscaped(), description.toHtmlEscaped())
                                      .toUtf8();
        opf.write(opfXml);
        opf.close();

        QFile content(root + QStringLiteral("/OEBPS/content.xhtml"));
        QVERIFY(content.open(QIODevice::WriteOnly));
        content.write("<html/>\n");
        content.close();
    };

    const QString yurchakPath = QStringLiteral("books/everything-was-forever.epub");
    addEpubBundle(yurchakPath,
                  QStringLiteral("Everything Was Forever Until It Was No More"),
                  QStringLiteral("Alexei Yurchak"),
                  QStringLiteral("2006"),
                  QStringLiteral("Late socialism, Soviet public culture, and post-Soviet anthropology."));
    const QString submarinePath = QStringLiteral("books/german-submarine-warfare.epub");
    addEpubBundle(submarinePath,
                  QStringLiteral("German Submarine Warfare in World War I"),
                  QStringLiteral("Lawrence Sondhaus"),
                  QStringLiteral("2017"),
                  QStringLiteral("A naval history of submarine warfare and war at sea in World War I."));
    const QString warPath = QStringLiteral("books/1941.epub");
    addEpubBundle(warPath,
                  QStringLiteral("1941: The America That Went to War"),
                  QStringLiteral("William M. Christie"),
                  QStringLiteral("2016"),
                  QStringLiteral("United States history on the eve of World War II."));
    const QString senatePath = QStringLiteral("books/master-of-the-senate.epub");
    addEpubBundle(senatePath,
                  QStringLiteral("Master of the Senate"),
                  QStringLiteral("Robert A. Caro"),
                  QStringLiteral("2002"),
                  QStringLiteral("A political biography of Lyndon Johnson and the United States Senate."));
    const QString mountaineeringPath = QStringLiteral("books/medicine-for-mountaineering.epub");
    addEpubBundle(mountaineeringPath,
                  QStringLiteral("Medicine for Mountaineering & Other Wilderness Activities"),
                  QStringLiteral("James A. Wilkerson M.D."),
                  QStringLiteral("2010"),
                  QStringLiteral("A wilderness medicine reference for mountaineering and remote travel."));
    const QUrl yurchakUrl = QUrl::fromLocalFile(m_dir->filePath(yurchakPath));
    const QUrl submarineUrl = QUrl::fromLocalFile(m_dir->filePath(submarinePath));
    const QUrl warUrl = QUrl::fromLocalFile(m_dir->filePath(warPath));
    const QUrl senateUrl = QUrl::fromLocalFile(m_dir->filePath(senatePath));
    const QUrl mountaineeringUrl = QUrl::fromLocalFile(m_dir->filePath(mountaineeringPath));

    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    store.setTitle(yurchakUrl, QStringLiteral("Everything Was Forever Until It Was No More"));
    store.setTags(yurchakUrl, {QStringLiteral("Book")});
    store.setTitle(submarineUrl, QStringLiteral("German Submarine Warfare in World War I"));
    store.setTags(submarineUrl, {QStringLiteral("Non-fiction"), QStringLiteral("Book")});
    store.setTitle(warUrl, QStringLiteral("1941"));
    store.setTags(warUrl, {QStringLiteral("MND Project"), QStringLiteral("Psychiatry"), QStringLiteral("Book")});
    store.setTitle(senateUrl, QStringLiteral("Master of the Senate"));
    store.setTags(senateUrl, {QStringLiteral("MND Project"), QStringLiteral("Book")});
    store.setTitle(mountaineeringUrl, QStringLiteral("Medicine for Mountaineeri..., 6th Edition"));
    store.setTags(mountaineeringUrl, {QStringLiteral("Book")});

    LibraryView view(&store, nullptr, true);
    view.refresh();

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);
    const int booksTab = tabIndexForText(shelves, QStringLiteral("Books"));
    QVERIFY(booksTab >= 0);
    shelves->setCurrentIndex(booksTab);
    QTRY_VERIFY(grid->model());
    QTRY_VERIFY(grid->model()->rowCount() >= 2);

    auto indexForTitle = [grid](const QString &title) {
        for (int row = 0; row < grid->model()->rowCount(); ++row) {
            const QModelIndex index = grid->model()->index(row, 0);
            if (index.data(Qt::DisplayRole).toString() == title) {
                return index;
            }
        }
        return QModelIndex();
    };
    auto verifyTileMetadataBudget = [](const QStringList &displayTags) {
        QVERIFY2(!displayTags.isEmpty(), "tile metadata must not be empty");
        QVERIFY2(displayTags.size() <= 2, qPrintable(QStringLiteral("too many tile metadata lines: %1").arg(displayTags.join(QStringLiteral(" | ")))));
        for (const QString &tag : displayTags) {
            QVERIFY2(!tag.contains(QStringLiteral("...")), qPrintable(QStringLiteral("ASCII ellipsis leaked into tile metadata: %1").arg(tag)));
            QVERIFY2(!tag.contains(QChar(0x2026)), qPrintable(QStringLiteral("ellipsized tile metadata: %1").arg(tag)));
            QVERIFY2(tag.size() <= 24, qPrintable(QStringLiteral("tile metadata over budget: %1").arg(tag)));
        }
    };

    const QModelIndex yurchakIndex = indexForTitle(QStringLiteral("Everything Was Forever Until It Was No More"));
    QVERIFY(yurchakIndex.isValid());
    const QStringList yurchakDisplayTags = yurchakIndex.data(LibraryView::DisplayTagsRole).toStringList();
    verifyTileMetadataBudget(yurchakDisplayTags);
    QVERIFY(yurchakDisplayTags.contains(QStringLiteral("Alexei Yurchak")));
    QVERIFY(yurchakDisplayTags.contains(QStringLiteral("Soviet anthropology")));
    QVERIFY(!yurchakDisplayTags.contains(QStringLiteral("Book")));
    const QString yurchakTooltip = yurchakIndex.data(Qt::ToolTipRole).toString();
    QVERIFY(yurchakTooltip.contains(QStringLiteral("Alexei Yurchak")));
    QVERIFY(yurchakTooltip.contains(QStringLiteral("2006")));
    QVERIFY(!yurchakTooltip.contains(QStringLiteral("EPUB · Book")));

    const QModelIndex submarineIndex = indexForTitle(QStringLiteral("German Submarine Warfare in World War I"));
    QVERIFY(submarineIndex.isValid());
    const QStringList submarineDisplayTags = submarineIndex.data(LibraryView::DisplayTagsRole).toStringList();
    verifyTileMetadataBudget(submarineDisplayTags);
    QVERIFY(submarineDisplayTags.contains(QStringLiteral("Lawrence Sondhaus")));
    QVERIFY(submarineDisplayTags.contains(QStringLiteral("Naval history")));
    QVERIFY(!submarineDisplayTags.contains(QStringLiteral("Book")));
    QVERIFY(!submarineDisplayTags.contains(QStringLiteral("MND / ALS")));

    const QModelIndex warIndex = indexForTitle(QStringLiteral("1941: The America That Went to War"));
    QVERIFY(warIndex.isValid());
    const QStringList warDisplayTags = warIndex.data(LibraryView::DisplayTagsRole).toStringList();
    verifyTileMetadataBudget(warDisplayTags);
    QVERIFY(warDisplayTags.contains(QStringLiteral("William M. Christie")));
    QVERIFY(warDisplayTags.contains(QStringLiteral("WWII history")));
    QVERIFY(!warDisplayTags.contains(QStringLiteral("MND / ALS")));
    QVERIFY(!warDisplayTags.contains(QStringLiteral("Psychiatry")));

    const QModelIndex senateIndex = indexForTitle(QStringLiteral("Master of the Senate"));
    QVERIFY(senateIndex.isValid());
    const QStringList senateDisplayTags = senateIndex.data(LibraryView::DisplayTagsRole).toStringList();
    verifyTileMetadataBudget(senateDisplayTags);
    QVERIFY(senateDisplayTags.contains(QStringLiteral("Robert A. Caro")));
    QVERIFY(senateDisplayTags.contains(QStringLiteral("Political biography")));
    QVERIFY(!senateDisplayTags.contains(QStringLiteral("MND / ALS")));

    const QModelIndex mountaineeringIndex = indexForTitle(QStringLiteral("Medicine for Mountaineering & Other Wilderness Activities"));
    QVERIFY(mountaineeringIndex.isValid());
    const QStringList mountaineeringDisplayTags = mountaineeringIndex.data(LibraryView::DisplayTagsRole).toStringList();
    verifyTileMetadataBudget(mountaineeringDisplayTags);
    QVERIFY(mountaineeringDisplayTags.contains(QStringLiteral("James A. Wilkerson")));
    QVERIFY(mountaineeringDisplayTags.contains(QStringLiteral("Wilderness medicine")));
    QVERIFY(!mountaineeringDisplayTags.contains(QStringLiteral("Book")));
}

void LibraryViewTest::testLocalPdfTileUsesCorpusMetadata()
{
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    const QUrl url = QUrl::fromLocalFile(m_dir->filePath(QStringLiteral("pdfs/10-9999-synthetic-mnd-tiles.pdf")));
    store.recordOpen(url, QDateTime(QDate(2026, 7, 6), QTime(7, 46)));

    LibraryView view(&store, nullptr, true);
    view.refresh();

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    PaperLibraryModel *paperModel = view.findChild<PaperLibraryModel *>();
    QVERIFY(paperModel);
    QTRY_VERIFY(paperModel->isLoaded());
    QTRY_VERIFY(grid->model());
    QTRY_COMPARE(grid->model()->rowCount(), 1);

    const QModelIndex index = grid->model()->index(0, 0);
    QTRY_COMPARE(index.data(Qt::DisplayRole).toString(), QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));
    QCOMPARE(index.data(LibraryView::DisplayTitleRole).toString(), QStringLiteral("Clinician 2026"));
    const LibraryView::TileCaption caption = LibraryView::tileCaption(index);
    QCOMPARE(caption.text, QStringLiteral("Clinician 2026"));
    QVERIFY(!caption.secondary);
    const QStringList tags = index.data(LibraryView::TagsRole).toStringList();
    QVERIFY(tags.contains(QStringLiteral("MND Project")));
    QVERIFY(tags.contains(QStringLiteral("Paper")));
    QCOMPARE(index.data(LibraryView::DescriptionRole).toString(), QStringLiteral("Casey Clinician · 2026 · Journal of Synthetic Neurology"));
    const QStringList displayTags = index.data(LibraryView::DisplayTagsRole).toStringList();
    QCOMPARE(displayTags.size(), 1);
    QVERIFY(displayTags.constFirst().contains(QStringLiteral("Neurofilament evidence")));
    QVERIFY(!index.data(Qt::DisplayRole).toString().contains(QStringLiteral("10-9999")));
}

void LibraryViewTest::testTilesSelectOnClickAndOpenOnDoubleClick()
{
    const QString pdfPath = m_dir->filePath(QStringLiteral("recent.pdf"));
    QFile pdf(pdfPath);
    QVERIFY(pdf.open(QIODevice::WriteOnly));
    pdf.write("%PDF-1.4\n");
    pdf.close();

    const QUrl url = QUrl::fromLocalFile(pdfPath);
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    store.recordOpen(url, QDateTime(QDate(2026, 7, 5), QTime(10, 24)));

    LibraryView view(&store, nullptr, false);
    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTRY_VERIFY(grid->model());
    QTRY_COMPARE(grid->model()->rowCount(), 1);
    QTRY_COMPARE(grid->model()->index(0, 0).data(Qt::DisplayRole).toString(), QStringLiteral("recent"));

    QSignalSpy activatedSpy(&view, &LibraryView::itemActivated);
    const QModelIndex index = grid->model()->index(0, 0);
    QVERIFY(index.isValid());

    QVERIFY(QMetaObject::invokeMethod(grid, "clicked", Qt::DirectConnection, Q_ARG(QModelIndex, index)));
    QCOMPARE(activatedSpy.count(), 0);

    grid->setCurrentIndex(index);
    QVERIFY(QMetaObject::invokeMethod(grid, "doubleClicked", Qt::DirectConnection, Q_ARG(QModelIndex, index)));
    QCOMPARE(activatedSpy.count(), 1);
    QCOMPARE(activatedSpy.takeFirst().at(0).toUrl(), url);
}

void LibraryViewTest::testDraggingTileDownDownranksLocalBook()
{
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    auto addBook = [this, &store](const QString &fileName, const QString &title, int opens) {
        const QString path = m_dir->filePath(fileName);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return QUrl();
        }
        file.write("not a real epub; enough for a local fixture path\n");
        file.close();
        const QUrl url = QUrl::fromLocalFile(path);
        store.setTitle(url, title);
        store.setTags(url, {QStringLiteral("Book")});
        for (int i = 0; i < opens; ++i) {
            store.recordOpen(url, QDateTime(QDate(2026, 7, 5), QTime(10, 0).addSecs(i)));
        }
        return url;
    };

    const QUrl topBook = addBook(QStringLiteral("books/Master of the Senate.epub"), QStringLiteral("Master of the Senate"), 4);
    const QUrl otherBook = addBook(QStringLiteral("books/A Game Of Thrones.epub"), QStringLiteral("A Game Of Thrones"), 1);
    QVERIFY(topBook.isValid());
    QVERIFY(otherBook.isValid());

    LibraryView view(&store, nullptr, true);
    view.resize(700, 520);
    view.refresh();
    view.layout()->activate();

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    grid->resize(680, 440);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);
    const int booksTab = tabIndexForText(shelves, QStringLiteral("Books"));
    QVERIFY(booksTab >= 0);
    shelves->setCurrentIndex(booksTab);
    QTRY_VERIFY(grid->model());
    QTRY_VERIFY(grid->model()->rowCount() >= 2);

    auto findTitleIndex = [grid](const QString &title) {
        for (int row = 0; row < grid->model()->rowCount(); ++row) {
            const QModelIndex index = grid->model()->index(row, 0);
            if (index.data(Qt::DisplayRole).toString() == title) {
                return index;
            }
        }
        return QModelIndex();
    };

    const QModelIndex index = findTitleIndex(QStringLiteral("Master of the Senate"));
    QVERIFY(index.isValid());
    grid->scrollTo(index);
    grid->doItemsLayout();
    QRect rect = grid->visualRect(index);
    QVERIFY(!rect.isEmpty());
    const QPoint start = rect.center();
    const QPoint finish = start + QPoint(0, 90);

    QTest::mousePress(grid->viewport(), Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(grid->viewport(), finish);
    QTest::mouseRelease(grid->viewport(), Qt::LeftButton, Qt::NoModifier, finish);

    QTRY_VERIFY(store.isDownranked(topBook));
}

void LibraryViewTest::testCorpusActivationStoresCuratedMetadata()
{
    writeMndFocusManifest(m_dir->path());
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, false);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);
    PaperLibraryModel *paperModel = view.findChild<PaperLibraryModel *>();
    QVERIFY(paperModel);
    QTRY_VERIFY(paperModel->isLoaded());

    const int mndTab = tabIndexForText(shelves, QStringLiteral("MND"));
    QVERIFY(mndTab >= 0);
    shelves->setCurrentIndex(mndTab);
    QTRY_COMPARE(grid->model()->rowCount(), 1);
    QTRY_COMPARE(grid->model()->index(0, 0).data(Qt::DisplayRole).toString(), QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));

    QSignalSpy activatedSpy(&view, &LibraryView::itemActivated);
    const QModelIndex index = grid->model()->index(0, 0);
    QVERIFY(index.isValid());
    grid->setCurrentIndex(index);
    QVERIFY(QMetaObject::invokeMethod(grid, "doubleClicked", Qt::DirectConnection, Q_ARG(QModelIndex, index)));
    QCOMPARE(activatedSpy.count(), 1);

    const QUrl url = activatedSpy.takeFirst().at(0).toUrl();
    const LibraryStore::Entry metadata = store.metadata(url);
    QCOMPARE(metadata.title, QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));
    QVERIFY(metadata.tags.contains(QStringLiteral("Current")));
    QVERIFY(metadata.description.contains(QStringLiteral("Casey Clinician")));
    QVERIFY(metadata.description.contains(QStringLiteral("Core project paper")));
    QVERIFY(metadata.description.contains(QStringLiteral("Adjacent:")));
}

void LibraryViewTest::testStarterPackEmptySetupTile()
{
    const QString starterDir = m_dir->filePath(QStringLiteral("starter-empty"));
    KConfigGroup general = KSharedConfig::openConfig(m_dir->filePath(QStringLiteral("paperlibraryrc")), KConfig::SimpleConfig)->group(QStringLiteral("General"));
    general.writeEntry("StarterPackPath", starterDir);
    general.sync();

    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, false);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);

    const int starterTab = tabIndexForText(shelves, QStringLiteral("Starter Pack"));
    QVERIFY(starterTab >= 0);
    shelves->setCurrentIndex(starterTab);
    QTRY_VERIFY(grid->model());
    QTRY_COMPARE(grid->model()->rowCount(), 1);

    QTRY_COMPARE(grid->model()->index(0, 0).data(Qt::DisplayRole).toString(), QStringLiteral("Install Starter Pack"));
    const QModelIndex index = grid->model()->index(0, 0);
    QVERIFY(!index.data(LibraryView::UrlRole).toUrl().isValid());
    QVERIFY(index.data(Qt::ToolTipRole).toString().contains(QStringLiteral("fetch-public-domain-starter.sh")));
    QCOMPARE(grid->viewMode(), QListView::IconMode);
    QVERIFY(grid->isWrapping());
    QVERIFY(grid->uniformItemSizes());
}

void LibraryViewTest::testStarterPackInstalledMetadataTooltip()
{
    const QString starterDir = m_dir->filePath(QStringLiteral("starter-installed"));
    writeStarterPackCatalog(starterDir);
    KConfigGroup general = KSharedConfig::openConfig(m_dir->filePath(QStringLiteral("paperlibraryrc")), KConfig::SimpleConfig)->group(QStringLiteral("General"));
    general.writeEntry("StarterPackPath", starterDir);
    general.sync();

    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, false);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);

    const int starterTab = tabIndexForText(shelves, QStringLiteral("Starter Pack"));
    QVERIFY(starterTab >= 0);
    shelves->setCurrentIndex(starterTab);
    QTRY_COMPARE(grid->model()->rowCount(), 1);

    QTRY_COMPARE(grid->model()->index(0, 0).data(Qt::DisplayRole).toString(), QStringLiteral("Starter Sample Book"));
    const QModelIndex index = grid->model()->index(0, 0);
    QVERIFY(index.data(LibraryView::DescriptionRole).toString().contains(QStringLiteral("Public Domain Author")));
    QVERIFY(index.data(LibraryView::DescriptionRole).toString().contains(QStringLiteral("1901")));
    QVERIFY(index.data(LibraryView::TagsRole).toStringList().contains(QStringLiteral("Classic")));
    QVERIFY(index.data(LibraryView::UrlRole).toUrl().isValid());

    const QString tooltip = index.data(Qt::ToolTipRole).toString();
    QVERIFY(tooltip.contains(QStringLiteral("Project Gutenberg")));
    QVERIFY(tooltip.contains(QStringLiteral("PG-123")));
    QVERIFY(tooltip.contains(QStringLiteral("Public domain in the United States")));
    QVERIFY(tooltip.contains(QStringLiteral("https://www.gutenberg.org/ebooks/123")));
}

void LibraryViewTest::testCorpusShelfPrebuildsBeforeFirstSwitch()
{
    writeMndFocusManifest(m_dir->path());
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, false);

    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);
    const int mndTab = tabIndexForText(shelves, QStringLiteral("MND"));
    QVERIFY(mndTab >= 0);
    QVERIFY(shelves->currentIndex() != mndTab);

    PaperLibraryModel *paperModel = view.findChild<PaperLibraryModel *>();
    QVERIFY(paperModel);
    QTRY_VERIFY(paperModel->isLoaded());
    QTRY_VERIFY(sectionedModelsContainTitle(&view, QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis")));
}

void LibraryViewTest::testCorpusSelectionDoesNotShowPersistentContextStrip()
{
    writeMndFocusManifest(m_dir->path());
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, false);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);

    const int mndTab = tabIndexForText(shelves, QStringLiteral("MND"));
    QVERIFY(mndTab >= 0);
    shelves->setCurrentIndex(mndTab);
    QTRY_COMPARE(grid->model()->rowCount(), 1);
    QTRY_COMPARE(grid->model()->index(0, 0).data(Qt::DisplayRole).toString(), QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));

    const QModelIndex index = grid->model()->index(0, 0);
    QCOMPARE(index.data(Qt::ToolTipRole).toString(), QString());
    grid->setCurrentIndex(index);
    QTest::qWait(50);

    const QList<QLabel *> labels = view.findChildren<QLabel *>();
    QVERIFY(std::none_of(labels.cbegin(), labels.cend(), [](const QLabel *label) {
        return !label->isHidden() && label->text().contains(QStringLiteral("Why:"));
    }));
}

void LibraryViewTest::testEmptyCorpusShelfUsesTile()
{
    writeEmptyFocusManifest(m_dir->path(), QStringLiteral("Medicine"));
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, false);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);

    const int medicineTab = tabIndexForText(shelves, QStringLiteral("Medicine"));
    QVERIFY(medicineTab >= 0);
    shelves->setCurrentIndex(medicineTab);
    QTRY_COMPARE(grid->model()->rowCount(), 1);

    QTRY_COMPARE(grid->model()->index(0, 0).data(Qt::DisplayRole).toString(), QStringLiteral("No local documents yet"));
    const QModelIndex index = grid->model()->index(0, 0);
    QVERIFY(index.data(PaperLibrarySectionedModel::SourceRowRole).isValid());
    QVERIFY(!index.data(PaperLibrarySectionedModel::SectionHeaderRole).toBool());
    QVERIFY(index.data(PaperLibraryModel::MissingRole).toBool());
    QVERIFY(index.data(Qt::ToolTipRole).toString().contains(QStringLiteral("No local documents yet")));
    QCOMPARE(grid->viewMode(), QListView::IconMode);
}

void LibraryViewTest::testRapidCorpusShelfSwitchingDoesNotReload()
{
    writeMndFocusManifest(m_dir->path());
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, false);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);

    PaperLibraryModel *paperModel = view.findChild<PaperLibraryModel *>();
    QVERIFY(paperModel);
    QTRY_VERIFY(paperModel->isLoaded());
    QSignalSpy loadedSpy(paperModel, &PaperLibraryModel::loaded);

    const int recentTab = tabIndexForText(shelves, QStringLiteral("Recent"));
    const int mndTab = tabIndexForText(shelves, QStringLiteral("MND"));
    const int workTab = tabIndexForText(shelves, QStringLiteral("Work"));
    const int papersTab = tabIndexForText(shelves, QStringLiteral("Papers"));
    QVERIFY(recentTab >= 0);
    QVERIFY(mndTab >= 0);
    QVERIFY(workTab >= 0);
    QVERIFY(papersTab >= 0);

    for (int i = 0; i < 10; ++i) {
        shelves->setCurrentIndex(mndTab);
        shelves->setCurrentIndex(workTab);
        shelves->setCurrentIndex(papersTab);
        shelves->setCurrentIndex(recentTab);
    }
    QTest::qWait(50);

    QCOMPARE(loadedSpy.count(), 0);
    QVERIFY(grid->model());
    QCOMPARE(grid->viewMode(), QListView::IconMode);
}

QTEST_MAIN(LibraryViewTest)
#include "libraryviewtest.moc"
