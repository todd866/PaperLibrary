/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>

#include <KConfigGroup>
#include <KSharedConfig>

#include <memory>

#include "../shell/libraryview.h"
#include "../shell/librarystore.h"

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
}

class LibraryViewTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void testCorpusShelvesUseTileGrid();
    void testBooksShelfStaysWithLocalEbooks();
    void testTilesSelectOnClickAndOpenOnDoubleClick();

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
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, true);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);

    shelves->setCurrentIndex(LibraryView::MndShelf);
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

void LibraryViewTest::testBooksShelfStaysWithLocalEbooks()
{
    LibraryStore store(m_dir->filePath(QStringLiteral("store-paperlibraryrc")));
    LibraryView view(&store, nullptr, true);

    QListView *grid = view.findChild<QListView *>();
    QVERIFY(grid);
    QTabBar *shelves = view.findChild<QTabBar *>();
    QVERIFY(shelves);

    shelves->setCurrentIndex(LibraryView::BooksShelf);
    QVERIFY(grid->model());
    QCOMPARE(grid->model()->rowCount(), 0);

    shelves->setCurrentIndex(LibraryView::MndShelf);
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

QTEST_MAIN(LibraryViewTest)
#include "libraryviewtest.moc"
