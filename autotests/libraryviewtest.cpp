/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
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
    void testCorpusShelvesUseTileGrid();
    void testCorpusShelfModelsPersistAcrossSwitches();
    void testDomainShelvesRequireFocusManifest();
    void testWorkShelfGeneratedCardsAreVisible();
    void testBooksShelfStaysWithLocalEbooks();
    void testTilesSelectOnClickAndOpenOnDoubleClick();
    void testCorpusActivationStoresCuratedMetadata();

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

void LibraryViewTest::testCorpusActivationStoresCuratedMetadata()
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
    QTRY_COMPARE(grid->model()->rowCount(), 1);

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

QTEST_MAIN(LibraryViewTest)
#include "libraryviewtest.moc"
