/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <KZip>
#include <KZipFileEntry>

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <memory>

#include "../shell/epubimporter.h"

namespace
{
const char *containerXml = R"(<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>)";

QByteArray pngBytes(const QColor &color, const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}
}

class EpubImporterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    void testImportsDirectoryBundleToValidZip();
    void testMimetypeIsStoredFirstRawBytes();
    void testDrmBundleRefused();
    void testNotDownloadedEmptyBundle();
    void testNotDownloadedICloudPlaceholder();
    void testNonBundleIsNotADirectoryBundle();
    void testIdempotentReuseDoesNotRezip();
    void testSourceChangeRebuilds();
    void testReadingProgressCopiedFromBooksDb();

private:
    /** Write a synthetic directory-bundle EPUB with the given relative files. */
    QString writeBundle(const QList<QPair<QString, QByteArray>> &files, const QString &name = QString());
    /** A complete, openable synthetic bundle (mimetype + container + OPF + chapter + cover). */
    QString writeCompleteBundle(const QString &name = QString());
    /** A minimal Apple Books sqlite DB mapping @p path to @p progress. */
    QString writeBooksDb(const QString &path, double progress);

    std::unique_ptr<QTemporaryDir> m_dir;
    int m_serial = 0;
};

namespace
{
// Defined after the class: this raw string carries "http://" whose "//"
// derails moc's preprocessor if it precedes the Q_OBJECT class (moc then
// swallows the class as a comment). The importer never parses the OPF —
// it repackages bytes verbatim — so the content is only round-tripped.
const char *contentOpf = R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>An Uncommonly Good Book</dc:title>
  </metadata>
  <manifest>
    <item id="content" href="content.xhtml" media-type="application/xhtml+xml"/>
    <item id="cover" href="images/cover.png" media-type="image/png" properties="cover-image"/>
  </manifest>
  <spine>
    <itemref idref="content"/>
  </spine>
</package>)";
}

void EpubImporterTest::initTestCase()
{
    // Redirect AppDataLocation to a throwaway sandbox so imports never touch
    // the real app data directory.
    QCoreApplication::setApplicationName(QStringLiteral("epubimportertest"));
    QStandardPaths::setTestModeEnabled(true);
}

void EpubImporterTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    qputenv("PAPERLIBRARY_IMPORT_DIR", QFile::encodeName(m_dir->filePath(QStringLiteral("imported-books"))));
    // A clean import directory each test keeps idempotency assertions honest.
    QDir(EpubImporter::importDir()).removeRecursively();
}

QString EpubImporterTest::writeBundle(const QList<QPair<QString, QByteArray>> &files, const QString &name)
{
    const QString root = m_dir->filePath(name.isEmpty() ? QStringLiteral("bundle-%1.epub").arg(++m_serial) : name);
    for (const auto &entry : files) {
        const QString path = root + QLatin1Char('/') + entry.first;
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            return QString();
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(entry.second) != entry.second.size()) {
            return QString();
        }
    }
    return root;
}

QString EpubImporterTest::writeCompleteBundle(const QString &name)
{
    return writeBundle(
        {
            {QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")},
            {QStringLiteral("META-INF/container.xml"), containerXml},
            {QStringLiteral("OEBPS/content.opf"), contentOpf},
            {QStringLiteral("OEBPS/content.xhtml"), QByteArrayLiteral("<html><body><p>Chapter one.</p></body></html>")},
            {QStringLiteral("OEBPS/images/cover.png"), pngBytes(QColor(30, 200, 90), QSize(40, 52))},
        },
        name);
}

QString EpubImporterTest::writeBooksDb(const QString &path, double progress)
{
    const QString dbPath = m_dir->filePath(QStringLiteral("BKLibrary-1.sqlite"));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("epubimportertest_books"));
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            return QString();
        }
        QSqlQuery query(db);
        query.exec(QStringLiteral("CREATE TABLE ZBKLIBRARYASSET (ZTITLE TEXT, ZPATH TEXT, ZREADINGPROGRESS REAL, ZLASTOPENDATE REAL)"));
        QSqlQuery insert(db);
        insert.prepare(QStringLiteral("INSERT INTO ZBKLIBRARYASSET VALUES (?, ?, ?, ?)"));
        insert.addBindValue(QStringLiteral("An Uncommonly Good Book"));
        insert.addBindValue(path);
        insert.addBindValue(progress);
        insert.addBindValue(700000000.0);
        insert.exec();
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("epubimportertest_books"));
    return dbPath;
}

void EpubImporterTest::testImportsDirectoryBundleToValidZip()
{
    const QString bundle = writeCompleteBundle();
    QVERIFY(!bundle.isEmpty());
    QVERIFY(EpubImporter::isDirectoryBundle(bundle));

    const EpubImporter::Result result = EpubImporter::import(bundle);
    QCOMPARE(result.status, EpubImporter::Status::Imported);
    QVERIFY(!result.importedPath.isEmpty());
    QVERIFY(QFileInfo(result.importedPath).isFile());
    QVERIFY(result.importedPath.endsWith(QLatin1String(".epub")));

    // The produced file is a readable zip whose contents round-trip.
    KZip zip(result.importedPath);
    QVERIFY(zip.open(QIODevice::ReadOnly));
    const KArchiveDirectory *root = zip.directory();

    // mimetype: present, first (headerStart 0) and STORED (encoding 0).
    const KArchiveEntry *mimeEntry = root->entry(QStringLiteral("mimetype"));
    QVERIFY(mimeEntry && mimeEntry->isFile());
    const auto *mimeZipEntry = static_cast<const KZipFileEntry *>(mimeEntry);
    QCOMPARE(mimeZipEntry->encoding(), 0);      // 0 = stored/uncompressed
    QCOMPARE(mimeZipEntry->headerStart(), 0LL); // first local header in the archive
    QCOMPARE(mimeZipEntry->data(), QByteArrayLiteral("application/epub+zip"));

    // Everything else came across, tree preserved.
    const KArchiveEntry *opf = root->entry(QStringLiteral("OEBPS/content.opf"));
    QVERIFY(opf && opf->isFile());
    QCOMPARE(static_cast<const KArchiveFile *>(opf)->data(), QByteArray(contentOpf));

    const KArchiveEntry *chapter = root->entry(QStringLiteral("OEBPS/content.xhtml"));
    QVERIFY(chapter && chapter->isFile());

    const KArchiveEntry *cover = root->entry(QStringLiteral("OEBPS/images/cover.png"));
    QVERIFY(cover && cover->isFile());
    QVERIFY(!QImage::fromData(static_cast<const KArchiveFile *>(cover)->data()).isNull());

    // A compressible entry actually got deflated (not stored), proving the
    // importer only exempts the mimetype from compression.
    QVERIFY(static_cast<const KZipFileEntry *>(chapter)->encoding() == 8 || static_cast<const KZipFileEntry *>(opf)->encoding() == 8);

    zip.close();
}

void EpubImporterTest::testMimetypeIsStoredFirstRawBytes()
{
    // The OCF spec's exact requirement, verified straight off the raw zip
    // bytes: first local file header at offset 0, compression method 0
    // (stored), filename "mimetype", content immediately following.
    const QString bundle = writeCompleteBundle();
    const EpubImporter::Result result = EpubImporter::import(bundle);
    QCOMPARE(result.status, EpubImporter::Status::Imported);

    QFile file(result.importedPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray head = file.read(64);
    file.close();

    QVERIFY(head.size() >= 38 + 20);
    QCOMPARE(head.left(4), QByteArrayLiteral("PK\x03\x04"));   // local file header signature
    QCOMPARE(quint8(head.at(8)), quint8(0));                   // compression method low byte = 0 (stored)
    QCOMPARE(quint8(head.at(9)), quint8(0));                   // compression method high byte = 0
    QCOMPARE(quint16(quint8(head.at(26)) | quint8(head.at(27)) << 8), quint16(8)); // filename length "mimetype"
    QCOMPARE(quint16(quint8(head.at(28)) | quint8(head.at(29)) << 8), quint16(0)); // extra field length 0
    QCOMPARE(head.mid(30, 8), QByteArrayLiteral("mimetype"));
    QCOMPARE(head.mid(38, 20), QByteArrayLiteral("application/epub+zip"));
}

void EpubImporterTest::testDrmBundleRefused()
{
    // A bundle carrying META-INF/encryption.xml is DRM-protected: refuse it
    // cleanly and write nothing.
    const QString bundle = writeBundle({
        {QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")},
        {QStringLiteral("META-INF/container.xml"), containerXml},
        {QStringLiteral("META-INF/encryption.xml"), QByteArrayLiteral("<encryption/>")},
        {QStringLiteral("OEBPS/content.opf"), contentOpf},
    });
    QVERIFY(!bundle.isEmpty());

    const EpubImporter::Result result = EpubImporter::import(bundle);
    QCOMPARE(result.status, EpubImporter::Status::DrmProtected);
    QVERIFY(result.importedPath.isEmpty());
    QVERIFY(!QFileInfo::exists(EpubImporter::importedPathFor(bundle)));
}

void EpubImporterTest::testNotDownloadedEmptyBundle()
{
    // An empty bundle directory (nothing materialised) reads as not-downloaded.
    const QString root = m_dir->filePath(QStringLiteral("empty-%1.epub").arg(++m_serial));
    QVERIFY(QDir().mkpath(root));

    const EpubImporter::Result result = EpubImporter::import(root);
    QCOMPARE(result.status, EpubImporter::Status::NotDownloaded);
    QVERIFY(result.importedPath.isEmpty());
}

void EpubImporterTest::testNotDownloadedICloudPlaceholder()
{
    // iCloud leaves a hidden ".name.icloud" sentinel in place of an
    // undownloaded file; the real container.xml is absent.
    const QString bundle = writeBundle({
        {QStringLiteral("META-INF/.container.xml.icloud"), QByteArrayLiteral("placeholder")},
    });
    QVERIFY(!bundle.isEmpty());

    const EpubImporter::Result result = EpubImporter::import(bundle);
    QCOMPARE(result.status, EpubImporter::Status::NotDownloaded);
    QVERIFY(result.importedPath.isEmpty());
}

void EpubImporterTest::testNonBundleIsNotADirectoryBundle()
{
    // A regular (zipped) .epub file is not a directory bundle: the caller
    // should open it directly, so import declines.
    const QString zipped = m_dir->filePath(QStringLiteral("real.epub"));
    {
        KZip zip(zipped);
        QVERIFY(zip.open(QIODevice::WriteOnly));
        zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
        zip.close();
    }
    QVERIFY(!EpubImporter::isDirectoryBundle(zipped));
    QCOMPARE(EpubImporter::import(zipped).status, EpubImporter::Status::NotADirectoryBundle);

    // A plain directory that does not end in .epub is likewise declined.
    const QString plainDir = m_dir->filePath(QStringLiteral("not-a-book"));
    QVERIFY(QDir().mkpath(plainDir));
    QVERIFY(!EpubImporter::isDirectoryBundle(plainDir));
    QCOMPARE(EpubImporter::import(plainDir).status, EpubImporter::Status::NotADirectoryBundle);
}

void EpubImporterTest::testIdempotentReuseDoesNotRezip()
{
    const QString bundle = writeCompleteBundle();
    const EpubImporter::Result first = EpubImporter::import(bundle);
    QCOMPARE(first.status, EpubImporter::Status::Imported);

    // Stamp the imported file with a distinctive past time; a reuse must
    // leave it untouched, a rebuild would move it to "now".
    const QDateTime marker = QDateTime::currentDateTime().addSecs(-3600);
    {
        QFile file(first.importedPath);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(marker, QFileDevice::FileModificationTime));
        file.close();
    }

    const EpubImporter::Result second = EpubImporter::import(bundle);
    QCOMPARE(second.status, EpubImporter::Status::Imported);
    QCOMPARE(second.importedPath, first.importedPath);
    // Reuse leaves the file in place (mtime within a second of the marker).
    QVERIFY(qAbs(QFileInfo(second.importedPath).lastModified().secsTo(marker)) <= 1);
    // And reuse never re-copies the reading position.
    QCOMPARE(second.progress, -1.0);
}

void EpubImporterTest::testSourceChangeRebuilds()
{
    const QString bundle = writeCompleteBundle();
    const EpubImporter::Result first = EpubImporter::import(bundle);
    QCOMPARE(first.status, EpubImporter::Status::Imported);
    {
        KZip zip(first.importedPath);
        QVERIFY(zip.open(QIODevice::ReadOnly));
        QVERIFY(zip.directory()->entry(QStringLiteral("OEBPS/chapter2.xhtml")) == nullptr);
        zip.close();
    }

    // Add a chapter to the source: the signature changes, forcing a rebuild.
    const QString added = bundle + QStringLiteral("/OEBPS/chapter2.xhtml");
    QFile file(added);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("<html><body><p>Chapter two.</p></body></html>");
    file.close();

    const EpubImporter::Result second = EpubImporter::import(bundle);
    QCOMPARE(second.status, EpubImporter::Status::Imported);
    QCOMPARE(second.importedPath, first.importedPath); // stable name
    KZip zip(second.importedPath);
    QVERIFY(zip.open(QIODevice::ReadOnly));
    QVERIFY(zip.directory()->entry(QStringLiteral("OEBPS/chapter2.xhtml")) != nullptr);
    zip.close();
}

void EpubImporterTest::testReadingProgressCopiedFromBooksDb()
{
    const QString bundle = writeCompleteBundle();
    const QString dbPath = writeBooksDb(QFileInfo(bundle).canonicalFilePath(), 0.42);
    QVERIFY(!dbPath.isEmpty());

    const EpubImporter::Result result = EpubImporter::import(bundle, dbPath);
    QCOMPARE(result.status, EpubImporter::Status::Imported);
    QVERIFY(qAbs(result.progress - 0.42) < 1e-9); // one-way copy on a fresh import

    // A second import reuses and does NOT re-copy the position.
    const EpubImporter::Result reuse = EpubImporter::import(bundle, dbPath);
    QCOMPARE(reuse.status, EpubImporter::Status::Imported);
    QCOMPARE(reuse.progress, -1.0);
}

QTEST_GUILESS_MAIN(EpubImporterTest)
#include "epubimportertest.moc"
