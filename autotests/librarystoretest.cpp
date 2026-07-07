/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <QDateTime>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include <memory>

#include "../shell/applebooksprogress.h"
#include "../shell/librarystore.h"

class LibraryStoreTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    void testRecordOpenBumpsCountAndRecency();
    void testPinning();
    void testDownranking();
    void testFinishedReading();
    void testRemove();
    void testRankingOrder();
    void testSuffixFilter();
    void testSkipsMissingFiles();
    void testImportUrlsSeedsFromFileMtime();
    void testImportDoesNotClobberExistingEntries();
    void testMetadataRoundTrip();
    void testMetadataDefaultsEmpty();
    void testEntriesCarryMetadata();

    void testAppleBooksProgressFixture();
    void testAppleBooksProgressValidButEmpty();
    void testAppleBooksProgressMissingDatabase();
    void testAppleBooksProgressGarbageFile();

private:
    QString storePath() const;
    QUrl touchFile(const QString &name);

    std::unique_ptr<QTemporaryDir> m_dir;
};

void LibraryStoreTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void LibraryStoreTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
}

QString LibraryStoreTest::storePath() const
{
    return m_dir->filePath(QStringLiteral("librarystorerc"));
}

QUrl LibraryStoreTest::touchFile(const QString &name)
{
    const QString path = m_dir->filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return QUrl();
    }
    file.write("x");
    file.close();
    return QUrl::fromLocalFile(path);
}

void LibraryStoreTest::testRecordOpenBumpsCountAndRecency()
{
    const QUrl url = touchFile(QStringLiteral("a.pdf"));
    LibraryStore store(storePath());
    QVERIFY(store.isEmpty());

    const QDateTime first(QDate(2026, 1, 1), QTime(10, 0));
    const QDateTime second(QDate(2026, 2, 1), QTime(10, 0));

    store.recordOpen(url, first);
    QVERIFY(!store.isEmpty());
    QList<LibraryStore::Entry> entries = store.entries();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.at(0).url, url);
    QCOMPARE(entries.at(0).openCount, 1);
    QCOMPARE(entries.at(0).lastOpened, first);
    QCOMPARE(entries.at(0).pinned, false);

    store.recordOpen(url, second);
    entries = store.entries();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.at(0).openCount, 2);
    QCOMPARE(entries.at(0).lastOpened, second);

    // The convenience overload stamps "now"
    store.recordOpen(url);
    entries = store.entries();
    QCOMPARE(entries.at(0).openCount, 3);
    QVERIFY(qAbs(entries.at(0).lastOpened.msecsTo(QDateTime::currentDateTime())) < 60000);
}

void LibraryStoreTest::testPinning()
{
    const QUrl url = touchFile(QStringLiteral("a.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(url, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));

    QCOMPARE(store.isPinned(url), false);
    store.setPinned(url, true);
    QCOMPARE(store.isPinned(url), true);
    QCOMPARE(store.entries().at(0).pinned, true);

    store.setPinned(url, false);
    QCOMPARE(store.isPinned(url), false);
    QCOMPARE(store.entries().at(0).pinned, false);
}

void LibraryStoreTest::testDownranking()
{
    const QUrl downranked = touchFile(QStringLiteral("downranked.pdf"));
    const QUrl normal = touchFile(QStringLiteral("normal.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(downranked, QDateTime(QDate(2026, 1, 3), QTime(10, 0)));
    store.recordOpen(downranked, QDateTime(QDate(2026, 1, 4), QTime(10, 0)));
    store.recordOpen(normal, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));

    QCOMPARE(store.isDownranked(downranked), false);
    store.setPinned(downranked, true);
    store.setDownranked(downranked, true);
    QCOMPARE(store.isDownranked(downranked), true);
    QCOMPARE(store.isPinned(downranked), false); // thumbs-down wins over pinning

    QList<LibraryStore::Entry> entries = store.entries();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).url, normal);
    QCOMPARE(entries.at(1).url, downranked);
    QCOMPARE(entries.at(1).downranked, true);

    store.setDownranked(downranked, false);
    entries = store.entries();
    QCOMPARE(entries.at(0).url, downranked); // high open count returns once no longer downranked
    QCOMPARE(entries.at(0).downranked, false);
}

void LibraryStoreTest::testFinishedReading()
{
    const QUrl finished = touchFile(QStringLiteral("finished.epub"));
    const QUrl active = touchFile(QStringLiteral("active.epub"));
    LibraryStore store(storePath());
    store.recordOpen(finished, QDateTime(QDate(2026, 1, 3), QTime(10, 0)));
    store.recordOpen(finished, QDateTime(QDate(2026, 1, 4), QTime(10, 0)));
    store.recordOpen(active, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));

    store.setPinned(finished, true);
    store.setDownranked(finished, true);
    QCOMPARE(store.isDownranked(finished), true);

    store.setFinishedReading(finished, true);
    QCOMPARE(store.isFinishedReading(finished), true);
    QCOMPARE(store.isPinned(finished), false);
    QCOMPARE(store.isDownranked(finished), false);

    QList<LibraryStore::Entry> entries = store.entries({QStringLiteral("epub")});
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).url, active);
    QCOMPARE(entries.at(0).finishedReading, false);
    QCOMPARE(entries.at(1).url, finished);
    QCOMPARE(entries.at(1).finishedReading, true);

    LibraryStore reopened(storePath());
    QCOMPARE(reopened.isFinishedReading(finished), true);
    reopened.setFinishedReading(finished, false);
    QCOMPARE(reopened.isFinishedReading(finished), false);
}

void LibraryStoreTest::testRemove()
{
    const QUrl urlA = touchFile(QStringLiteral("a.pdf"));
    const QUrl urlB = touchFile(QStringLiteral("b.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(urlA, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));
    store.recordOpen(urlB, QDateTime(QDate(2026, 1, 2), QTime(10, 0)));
    QCOMPARE(store.entries().size(), 2);

    store.remove(urlA);
    const QList<LibraryStore::Entry> entries = store.entries();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.at(0).url, urlB);
    store.importUrls({urlA});
    QCOMPARE(store.entries().size(), 1); // removed files are not resurrected by recent-file import

    store.recordOpen(urlA, QDateTime(QDate(2026, 1, 3), QTime(10, 0)));
    QCOMPARE(store.entries().size(), 2); // explicitly opening the file adds it back

    store.remove(urlA);
    store.remove(urlB);
    QVERIFY(store.entries().isEmpty());
}

void LibraryStoreTest::testRankingOrder()
{
    const QUrl pinned = touchFile(QStringLiteral("pinned.pdf"));
    const QUrl older = touchFile(QStringLiteral("older.pdf"));
    const QUrl newer = touchFile(QStringLiteral("newer.pdf"));
    const QUrl top = touchFile(QStringLiteral("top.pdf"));

    LibraryStore store(storePath());

    // pinned.pdf: opened once, but pinned
    store.recordOpen(pinned, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));
    store.setPinned(pinned, true);

    // older.pdf and newer.pdf: same count, recency decides
    for (int i = 0; i < 5; ++i) {
        store.recordOpen(older, QDateTime(QDate(2026, 3, 1), QTime(10, 0)));
        store.recordOpen(newer, QDateTime(QDate(2026, 4, 1), QTime(10, 0)));
    }

    // top.pdf: highest count, but not pinned
    for (int i = 0; i < 9; ++i) {
        store.recordOpen(top, QDateTime(QDate(2026, 2, 1), QTime(10, 0)));
    }

    const QList<LibraryStore::Entry> entries = store.entries();
    QCOMPARE(entries.size(), 4);
    QCOMPARE(entries.at(0).url, pinned); // pinned first, regardless of count
    QCOMPARE(entries.at(1).url, top);    // then count desc
    QCOMPARE(entries.at(2).url, newer);  // count tie: recency desc
    QCOMPARE(entries.at(3).url, older);
}

void LibraryStoreTest::testSuffixFilter()
{
    const QUrl pdf = touchFile(QStringLiteral("a.pdf"));
    const QUrl epub = touchFile(QStringLiteral("b.epub"));
    const QUrl upperPdf = touchFile(QStringLiteral("c.PDF"));

    LibraryStore store(storePath());
    store.recordOpen(pdf, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));
    store.recordOpen(epub, QDateTime(QDate(2026, 1, 2), QTime(10, 0)));
    store.recordOpen(upperPdf, QDateTime(QDate(2026, 1, 3), QTime(10, 0)));

    QCOMPARE(store.entries().size(), 3);

    const QList<LibraryStore::Entry> pdfs = store.entries({QStringLiteral("pdf")});
    QCOMPARE(pdfs.size(), 2); // suffix match is case-insensitive
    for (const LibraryStore::Entry &entry : pdfs) {
        QVERIFY(entry.url == pdf || entry.url == upperPdf);
    }

    const QList<LibraryStore::Entry> epubs = store.entries({QStringLiteral("epub")});
    QCOMPARE(epubs.size(), 1);
    QCOMPARE(epubs.at(0).url, epub);

    QCOMPARE(store.entries({QStringLiteral("pdf"), QStringLiteral("epub")}).size(), 3);
}

void LibraryStoreTest::testSkipsMissingFiles()
{
    const QUrl urlA = touchFile(QStringLiteral("a.pdf"));
    const QUrl urlB = touchFile(QStringLiteral("b.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(urlA, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));
    store.recordOpen(urlB, QDateTime(QDate(2026, 1, 2), QTime(10, 0)));
    store.recordOpen(urlB, QDateTime(QDate(2026, 1, 3), QTime(10, 0)));

    QVERIFY(QFile::remove(urlB.toLocalFile()));

    // Missing files are skipped in listings ...
    QList<LibraryStore::Entry> entries = store.entries();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.at(0).url, urlA);

    // ... but the record survives: the file coming back restores the entry
    QCOMPARE(touchFile(QStringLiteral("b.pdf")), urlB);
    entries = store.entries();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).url, urlB); // still ranked by its preserved count
    QCOMPARE(entries.at(0).openCount, 2);
}

void LibraryStoreTest::testImportUrlsSeedsFromFileMtime()
{
    const QUrl urlA = touchFile(QStringLiteral("a.pdf"));
    const QUrl urlB = touchFile(QStringLiteral("b.pdf"));

    const QDateTime mtimeA(QDate(2025, 6, 1), QTime(8, 30));
    {
        QFile file(urlA.toLocalFile());
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(mtimeA, QFileDevice::FileModificationTime));
    }

    LibraryStore store(storePath());
    QVERIFY(store.isEmpty());
    store.importUrls({urlA, urlB});

    const QList<LibraryStore::Entry> entries = store.entries();
    QCOMPARE(entries.size(), 2);
    for (const LibraryStore::Entry &entry : entries) {
        QCOMPARE(entry.openCount, 1);
        QCOMPARE(entry.pinned, false);
    }
    const int indexA = entries.at(0).url == urlA ? 0 : 1;
    QCOMPARE(entries.at(indexA).url, urlA);
    QCOMPARE(entries.at(indexA).lastOpened, mtimeA);
}

void LibraryStoreTest::testImportDoesNotClobberExistingEntries()
{
    const QUrl urlA = touchFile(QStringLiteral("a.pdf"));
    const QUrl urlB = touchFile(QStringLiteral("b.pdf"));

    LibraryStore store(storePath());
    const QDateTime opened(QDate(2026, 5, 1), QTime(12, 0));
    store.recordOpen(urlA, opened);
    store.recordOpen(urlA, opened);
    store.recordOpen(urlA, opened);

    store.importUrls({urlA, urlB});

    const QList<LibraryStore::Entry> entries = store.entries();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).url, urlA); // count 3 outranks count 1
    QCOMPARE(entries.at(0).openCount, 3);
    QCOMPARE(entries.at(0).lastOpened, opened);
    QCOMPARE(entries.at(1).url, urlB);
    QCOMPARE(entries.at(1).openCount, 1);
}

void LibraryStoreTest::testMetadataRoundTrip()
{
    const QUrl url = touchFile(QStringLiteral("a.pdf"));
    {
        LibraryStore store(storePath());
        store.recordOpen(url, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));
        store.setTitle(url, QStringLiteral("Neural Manuscript"));
        store.setTags(url, {QStringLiteral("Manuscript"), QStringLiteral("Neuroscience")});
        store.setDescription(url, QStringLiteral("A manuscript about brains"));
        store.setKeywords(url, {QStringLiteral("cortex"), QStringLiteral("plasticity")});
    }

    // A fresh store instance sees the persisted metadata
    LibraryStore store(storePath());
    const LibraryStore::Entry entry = store.metadata(url);
    QCOMPARE(entry.url, url);
    QCOMPARE(entry.openCount, 1);
    QCOMPARE(entry.title, QStringLiteral("Neural Manuscript"));
    QCOMPARE(entry.tags, QStringList({QStringLiteral("Manuscript"), QStringLiteral("Neuroscience")}));
    QCOMPARE(entry.description, QStringLiteral("A manuscript about brains"));
    QCOMPARE(entry.keywords, QStringList({QStringLiteral("cortex"), QStringLiteral("plasticity")}));

    // Overwriting works, and clearing a field removes it
    store.setTitle(url, QStringLiteral("Renamed"));
    QCOMPARE(store.metadata(url).title, QStringLiteral("Renamed"));
    store.setTags(url, {});
    QCOMPARE(store.metadata(url).tags, QStringList());
}

void LibraryStoreTest::testMetadataDefaultsEmpty()
{
    const QUrl url = touchFile(QStringLiteral("a.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(url, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));

    // An entry that never had metadata set reads back empty fields
    const LibraryStore::Entry entry = store.metadata(url);
    QCOMPARE(entry.title, QString());
    QCOMPARE(entry.tags, QStringList());
    QCOMPARE(entry.description, QString());
    QCOMPARE(entry.keywords, QStringList());
    QCOMPARE(entry.finishedReading, false);

    // ... as does a url the store has never seen at all
    const LibraryStore::Entry unknown = store.metadata(QUrl::fromLocalFile(QStringLiteral("/nowhere/unknown.pdf")));
    QCOMPARE(unknown.openCount, 0);
    QCOMPARE(unknown.title, QString());
    QCOMPARE(unknown.tags, QStringList());
}

void LibraryStoreTest::testEntriesCarryMetadata()
{
    const QUrl tagged = touchFile(QStringLiteral("tagged.pdf"));
    const QUrl plain = touchFile(QStringLiteral("plain.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(tagged, QDateTime(QDate(2026, 1, 2), QTime(10, 0)));
    store.recordOpen(tagged, QDateTime(QDate(2026, 1, 3), QTime(10, 0)));
    store.recordOpen(plain, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));
    store.setTitle(tagged, QStringLiteral("Type Ratings"));
    store.setTags(tagged, {QStringLiteral("Aviation"), QStringLiteral("Reference")});
    store.setDescription(tagged, QStringLiteral("Aircraft type rating notes"));
    store.setKeywords(tagged, {QStringLiteral("ATPL")});

    const QList<LibraryStore::Entry> entries = store.entries();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).url, tagged); // count 2 outranks count 1
    QCOMPARE(entries.at(0).title, QStringLiteral("Type Ratings"));
    QCOMPARE(entries.at(0).tags, QStringList({QStringLiteral("Aviation"), QStringLiteral("Reference")}));
    QCOMPARE(entries.at(0).description, QStringLiteral("Aircraft type rating notes"));
    QCOMPARE(entries.at(0).keywords, QStringList({QStringLiteral("ATPL")}));
    QCOMPARE(entries.at(0).finishedReading, false);
    QCOMPARE(entries.at(1).url, plain);
    QCOMPARE(entries.at(1).title, QString());
    QCOMPARE(entries.at(1).tags, QStringList());
}

void LibraryStoreTest::testAppleBooksProgressFixture()
{
    QVERIFY(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")));

    const QString dbPath = m_dir->filePath(QStringLiteral("BKLibrary-1-091020131601.sqlite"));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("librarystoretest_fixture"));
        db.setDatabaseName(dbPath);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE ZBKLIBRARYASSET (ZTITLE TEXT, ZPATH TEXT, ZREADINGPROGRESS REAL, ZLASTOPENDATE REAL)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO ZBKLIBRARYASSET VALUES ('Reading Book', '/books/reading.epub', 0.129, 700000000)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO ZBKLIBRARYASSET VALUES ('Unread Book', '/books/unread.epub', 0, 700000001)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO ZBKLIBRARYASSET VALUES ('No Path Book', NULL, 0.5, 700000002)")));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("librarystoretest_fixture"));

    bool ok = false;
    const QList<AppleBooksProgress::BookEntry> books = AppleBooksProgress::read(dbPath, &ok);
    QVERIFY(ok);               // opened and queried successfully
    QCOMPARE(books.size(), 1); // progress=0 and NULL-path rows are excluded
    QCOMPARE(books.at(0).title, QStringLiteral("Reading Book"));
    QCOMPARE(books.at(0).path, QStringLiteral("/books/reading.epub"));
    QCOMPARE(books.at(0).progress, 0.129);
}

// A readable database with the right schema but no in-progress rows is a
// genuine empty result — reported as success, not as a failure.
void LibraryStoreTest::testAppleBooksProgressValidButEmpty()
{
    QVERIFY(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")));

    const QString dbPath = m_dir->filePath(QStringLiteral("BKLibrary-empty.sqlite"));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("librarystoretest_empty"));
        db.setDatabaseName(dbPath);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE ZBKLIBRARYASSET (ZTITLE TEXT, ZPATH TEXT, ZREADINGPROGRESS REAL, ZLASTOPENDATE REAL)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO ZBKLIBRARYASSET VALUES ('Unread Book', '/books/unread.epub', 0, 700000000)")));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("librarystoretest_empty"));

    bool ok = false;
    const QList<AppleBooksProgress::BookEntry> books = AppleBooksProgress::read(dbPath, &ok);
    QVERIFY(ok); // the query ran; no in-progress rows is a real (empty) answer
    QVERIFY(books.isEmpty());
}

void LibraryStoreTest::testAppleBooksProgressMissingDatabase()
{
    // An explicitly requested database that does not exist is a failure, not
    // a silent empty — the caller must be able to tell the two apart.
    bool ok = true;
    const QList<AppleBooksProgress::BookEntry> books = AppleBooksProgress::read(m_dir->filePath(QStringLiteral("does-not-exist.sqlite")), &ok);
    QVERIFY(!ok);
    QVERIFY(books.isEmpty());
}

void LibraryStoreTest::testAppleBooksProgressGarbageFile()
{
    // A file that exists but is not an Apple Books database (corrupt / schema
    // mismatch): a failure, reported through ok rather than an empty result.
    const QUrl garbage = touchFile(QStringLiteral("garbage.sqlite"));
    bool ok = true;
    const QList<AppleBooksProgress::BookEntry> books = AppleBooksProgress::read(garbage.toLocalFile(), &ok);
    QVERIFY(!ok);
    QVERIFY(books.isEmpty());
}

QTEST_GUILESS_MAIN(LibraryStoreTest)
#include "librarystoretest.moc"
