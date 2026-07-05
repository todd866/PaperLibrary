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
#include <QPainter>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyleOptionViewItem>
#include <QTabBar>
#include <QTemporaryDir>

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
    void testGeneratedCorpusCoverKeepsSemanticThumbnail();
    void testBooksShelfStaysWithLocalEbooks();
    void testLocalBookClassificationIgnoresStaleTags();
    void testTilesSelectOnClickAndOpenOnDoubleClick();
    void testDraggingTileDownDownranksLocalBook();
    void testCorpusActivationStoresCuratedMetadata();
    void testStarterPackEmptySetupTile();
    void testStarterPackInstalledMetadataTooltip();
    void testCorpusShelfPrebuildsBeforeFirstSwitch();
    void testCorpusSelectionUsesInlineContextNotNativeTooltip();
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
    QVERIFY(tileSize.width() >= 160);
    QVERIFY(tileSize.height() >= 220);

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

    addBook(QStringLiteral("books/1941 The America That Went To War.epub"), QStringLiteral("1941"), {QStringLiteral("Psychiatry"), QStringLiteral("Book")});
    addBook(QStringLiteral("books/Master of the Senate.epub"), QStringLiteral("Master of the Senate"), {QStringLiteral("Fiction"), QStringLiteral("Book")});
    addBook(QStringLiteral("books/A Game Of Thrones.epub"), QStringLiteral("A Game Of Thrones"), {QStringLiteral("Non-fiction"), QStringLiteral("Book")});

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

    const QStringList caroTags = tagsForTitle(QStringLiteral("Master of the Senate"));
    QVERIFY(caroTags.contains(QStringLiteral("Politics")));
    QVERIFY(caroTags.contains(QStringLiteral("Book")));
    QVERIFY(!caroTags.contains(QStringLiteral("Fiction")));

    const QStringList thronesTags = tagsForTitle(QStringLiteral("A Game of Thrones"));
    QVERIFY(thronesTags.contains(QStringLiteral("Fiction")));
    QVERIFY(thronesTags.contains(QStringLiteral("Book")));
    QVERIFY(!thronesTags.contains(QStringLiteral("Non-fiction")));
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

void LibraryViewTest::testCorpusSelectionUsesInlineContextNotNativeTooltip()
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

    QTRY_VERIFY([&view]() {
        const QList<QLabel *> labels = view.findChildren<QLabel *>();
        return std::any_of(labels.cbegin(), labels.cend(), [](const QLabel *label) {
            return !label->isHidden() && label->text().contains(QStringLiteral("Neurofilament Biomarkers")) && label->text().contains(QStringLiteral("Why:"));
        });
    }());
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
