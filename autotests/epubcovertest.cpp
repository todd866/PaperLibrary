/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <KZip>
#include <QBuffer>
#include <QImage>
#include <QTemporaryDir>

#include <memory>

#include "../shell/epubcover.h"

namespace
{
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

const char *containerXml = R"(<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>)";
}

class EpubCoverTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();

    void testEpub3CoverImageProperty();
    void testEpub2MetaCover();
    void testFallbackLargestImage();
    void testNoImagesReturnsNull();
    void testMissingFileReturnsNull();
    void testNotAZipReturnsNull();
    void testContentsEpubFixture();

    void testMetadataFromOpf();
    void testMetadataAbsentFieldsAreEmpty();
    void testMetadataOfMissingFileIsEmpty();
    void testDirectoryBundleCoverAndMetadata();
    void testDirectoryBundleEncryptedCoverFallsBack();
    void testDirectoryBundleFallbackLargestImage();
    void testDirectoryBundleHrefCannotEscape();

private:
    QString writeEpub(const QList<QPair<QString, QByteArray>> &files);
    QString writeEpubDir(const QList<QPair<QString, QByteArray>> &files);

    std::unique_ptr<QTemporaryDir> m_dir;
    int m_serial = 0;
};

void EpubCoverTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
}

QString EpubCoverTest::writeEpub(const QList<QPair<QString, QByteArray>> &files)
{
    const QString path = m_dir->filePath(QStringLiteral("book-%1.epub").arg(++m_serial));
    KZip zip(path);
    if (!zip.open(QIODevice::WriteOnly)) {
        return QString();
    }
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    for (const auto &file : files) {
        zip.writeFile(file.first, file.second);
    }
    zip.close();
    return path;
}

void EpubCoverTest::testEpub3CoverImageProperty()
{
    // EPUB 3 declares the cover as a manifest item property; the href is
    // relative to the OPF, here pointing into a subdirectory
    const char *opf = R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata/>
  <manifest>
    <item id="content" href="content.xhtml" media-type="application/xhtml+xml"/>
    <item id="cover" href="images/cover.png" media-type="image/png" properties="cover-image"/>
  </manifest>
</package>)";
    const QByteArray cover = pngBytes(QColor(255, 128, 0), QSize(24, 30));
    const QString path = writeEpub({
        {QStringLiteral("META-INF/container.xml"), containerXml},
        {QStringLiteral("OEBPS/content.opf"), opf},
        {QStringLiteral("OEBPS/content.xhtml"), QByteArrayLiteral("<html/>")},
        {QStringLiteral("OEBPS/images/cover.png"), cover},
    });
    QVERIFY(!path.isEmpty());

    const QImage extracted = EpubCover::extract(path);
    QVERIFY(!extracted.isNull());
    QCOMPARE(extracted.size(), QSize(24, 30));
    QCOMPARE(extracted.pixelColor(0, 0), QColor(255, 128, 0));
}

void EpubCoverTest::testEpub2MetaCover()
{
    // EPUB 2 points at the cover with <meta name="cover" content="id"/>
    const char *opf = R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0">
  <metadata>
    <meta name="cover" content="cover-img"/>
  </metadata>
  <manifest>
    <item id="content" href="content.xhtml" media-type="application/xhtml+xml"/>
    <item id="cover-img" href="cover.png" media-type="image/png"/>
  </manifest>
</package>)";
    const QByteArray cover = pngBytes(QColor(0, 96, 255), QSize(20, 26));
    const QString path = writeEpub({
        {QStringLiteral("META-INF/container.xml"), containerXml},
        {QStringLiteral("OEBPS/content.opf"), opf},
        {QStringLiteral("OEBPS/content.xhtml"), QByteArrayLiteral("<html/>")},
        {QStringLiteral("OEBPS/cover.png"), cover},
    });
    QVERIFY(!path.isEmpty());

    const QImage extracted = EpubCover::extract(path);
    QVERIFY(!extracted.isNull());
    QCOMPARE(extracted.size(), QSize(20, 26));
    QCOMPARE(extracted.pixelColor(0, 0), QColor(0, 96, 255));
}

void EpubCoverTest::testFallbackLargestImage()
{
    // No declared cover anywhere: the largest image in the archive wins
    const char *opf = R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0">
  <metadata/>
  <manifest>
    <item id="content" href="content.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
</package>)";
    const QString path = writeEpub({
        {QStringLiteral("META-INF/container.xml"), containerXml},
        {QStringLiteral("OEBPS/content.opf"), opf},
        {QStringLiteral("OEBPS/content.xhtml"), QByteArrayLiteral("<html/>")},
        {QStringLiteral("OEBPS/img/small.png"), pngBytes(QColor(200, 0, 0), QSize(8, 8))},
        {QStringLiteral("OEBPS/img/big.png"), pngBytes(QColor(0, 160, 0), QSize(48, 48))},
    });
    QVERIFY(!path.isEmpty());

    const QImage extracted = EpubCover::extract(path);
    QVERIFY(!extracted.isNull());
    QCOMPARE(extracted.size(), QSize(48, 48));
    QCOMPARE(extracted.pixelColor(0, 0), QColor(0, 160, 0));
}

void EpubCoverTest::testNoImagesReturnsNull()
{
    const char *opf = R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata/>
  <manifest>
    <item id="content" href="content.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
</package>)";
    const QString path = writeEpub({
        {QStringLiteral("META-INF/container.xml"), containerXml},
        {QStringLiteral("OEBPS/content.opf"), opf},
        {QStringLiteral("OEBPS/content.xhtml"), QByteArrayLiteral("<html/>")},
    });
    QVERIFY(!path.isEmpty());

    QVERIFY(EpubCover::extract(path).isNull());
}

void EpubCoverTest::testMissingFileReturnsNull()
{
    QVERIFY(EpubCover::extract(m_dir->filePath(QStringLiteral("does-not-exist.epub"))).isNull());
}

void EpubCoverTest::testNotAZipReturnsNull()
{
    const QString path = m_dir->filePath(QStringLiteral("garbage.epub"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("this is not a zip archive");
    file.close();

    QVERIFY(EpubCover::extract(path).isNull());
}

void EpubCoverTest::testContentsEpubFixture()
{
    // The repository's real EPUB fixture has a valid container and OPF but
    // no image anywhere in the archive — checked once, asserted forever
    const QImage extracted = EpubCover::extract(QStringLiteral(KDESRCDIR "data/contents.epub"));
    QVERIFY(extracted.isNull());
}

// Apple Books stores many EPUBs as *directories*, not zips: the same
// container/OPF layout laid out on the filesystem
QString EpubCoverTest::writeEpubDir(const QList<QPair<QString, QByteArray>> &files)
{
    const QString root = m_dir->filePath(QStringLiteral("bundle-%1.epub").arg(++m_serial));
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

namespace
{
const char *opfWithDublinCore = R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>An Uncommonly Good Book</dc:title>
    <dc:creator>Jane Doe</dc:creator>
    <dc:creator>John Q. Smith</dc:creator>
    <dc:date>1987-03-01T00:00:00Z</dc:date>
    <dc:description>&lt;p&gt;An &lt;i&gt;uncommonly&lt;/i&gt; good book.&lt;/p&gt;</dc:description>
  </metadata>
  <manifest>
    <item id="content" href="content.xhtml" media-type="application/xhtml+xml"/>
    <item id="cover" href="images/My%20Cover.png" media-type="image/png" properties="cover-image"/>
  </manifest>
</package>)";
}

void EpubCoverTest::testMetadataFromOpf()
{
    const QString path = writeEpub({
        {QStringLiteral("META-INF/container.xml"), containerXml},
        {QStringLiteral("OEBPS/content.opf"), opfWithDublinCore},
        {QStringLiteral("OEBPS/content.xhtml"), QByteArrayLiteral("<html/>")},
    });
    QVERIFY(!path.isEmpty());

    const EpubCover::Metadata metadata = EpubCover::metadata(path);
    QCOMPARE(metadata.creators, QStringLiteral("Jane Doe, John Q. Smith"));
    QCOMPARE(metadata.year, QStringLiteral("1987"));
    QCOMPARE(metadata.description, QStringLiteral("An uncommonly good book."));
}

void EpubCoverTest::testMetadataAbsentFieldsAreEmpty()
{
    const char *opf = R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata/>
  <manifest>
    <item id="content" href="content.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
</package>)";
    const QString path = writeEpub({
        {QStringLiteral("META-INF/container.xml"), containerXml},
        {QStringLiteral("OEBPS/content.opf"), opf},
        {QStringLiteral("OEBPS/content.xhtml"), QByteArrayLiteral("<html/>")},
    });
    QVERIFY(!path.isEmpty());

    const EpubCover::Metadata metadata = EpubCover::metadata(path);
    QVERIFY(metadata.creators.isEmpty());
    QVERIFY(metadata.year.isEmpty());
    QVERIFY(metadata.description.isEmpty());
}

void EpubCoverTest::testMetadataOfMissingFileIsEmpty()
{
    const EpubCover::Metadata metadata = EpubCover::metadata(m_dir->filePath(QStringLiteral("does-not-exist.epub")));
    QVERIFY(metadata.creators.isEmpty());
    QVERIFY(metadata.year.isEmpty());
    QVERIFY(metadata.description.isEmpty());
}

void EpubCoverTest::testDirectoryBundleCoverAndMetadata()
{
    // The OPF href is percent-encoded ("My%20Cover.png"); the file on disk
    // carries the decoded name, and the href is OPF-relative
    const QByteArray cover = pngBytes(QColor(30, 200, 90), QSize(22, 28));
    const QString root = writeEpubDir({
        {QStringLiteral("META-INF/container.xml"), containerXml},
        {QStringLiteral("OEBPS/content.opf"), opfWithDublinCore},
        {QStringLiteral("OEBPS/content.xhtml"), QByteArrayLiteral("<html/>")},
        {QStringLiteral("OEBPS/images/My Cover.png"), cover},
    });
    QVERIFY(!root.isEmpty());

    const QImage extracted = EpubCover::extract(root);
    QVERIFY(!extracted.isNull());
    QCOMPARE(extracted.size(), QSize(22, 28));
    QCOMPARE(extracted.pixelColor(0, 0), QColor(30, 200, 90));

    const EpubCover::Metadata metadata = EpubCover::metadata(root);
    QCOMPARE(metadata.creators, QStringLiteral("Jane Doe, John Q. Smith"));
    QCOMPARE(metadata.year, QStringLiteral("1987"));
    QCOMPARE(metadata.description, QStringLiteral("An uncommonly good book."));
}

void EpubCoverTest::testDirectoryBundleEncryptedCoverFallsBack()
{
    // DRM'd Apple Books bundles keep the manifest readable but the images
    // undecodable: the cover must fall back to null (the caller generates
    // a card) while the OPF metadata still comes through
    const QString root = writeEpubDir({
        {QStringLiteral("META-INF/container.xml"), containerXml},
        {QStringLiteral("OEBPS/content.opf"), opfWithDublinCore},
        {QStringLiteral("OEBPS/content.xhtml"), QByteArrayLiteral("<html/>")},
        {QStringLiteral("OEBPS/images/My Cover.png"), QByteArrayLiteral("\x01\x02\x03 encrypted garbage, not a PNG")},
    });
    QVERIFY(!root.isEmpty());

    QVERIFY(EpubCover::extract(root).isNull());

    const EpubCover::Metadata metadata = EpubCover::metadata(root);
    QCOMPARE(metadata.creators, QStringLiteral("Jane Doe, John Q. Smith"));
    QCOMPARE(metadata.year, QStringLiteral("1987"));
}

void EpubCoverTest::testDirectoryBundleFallbackLargestImage()
{
    const char *opf = R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0">
  <metadata/>
  <manifest>
    <item id="content" href="content.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
</package>)";
    const QString root = writeEpubDir({
        {QStringLiteral("META-INF/container.xml"), containerXml},
        {QStringLiteral("OEBPS/content.opf"), opf},
        {QStringLiteral("OEBPS/content.xhtml"), QByteArrayLiteral("<html/>")},
        {QStringLiteral("OEBPS/img/small.png"), pngBytes(QColor(200, 0, 0), QSize(8, 8))},
        {QStringLiteral("OEBPS/img/big.png"), pngBytes(QColor(0, 160, 0), QSize(48, 48))},
    });
    QVERIFY(!root.isEmpty());

    const QImage extracted = EpubCover::extract(root);
    QVERIFY(!extracted.isNull());
    QCOMPARE(extracted.size(), QSize(48, 48));
    QCOMPARE(extracted.pixelColor(0, 0), QColor(0, 160, 0));
}

void EpubCoverTest::testDirectoryBundleHrefCannotEscape()
{
    // A malicious (or corrupt) OPF href must never read outside the bundle
    const QByteArray outside = pngBytes(QColor(255, 0, 255), QSize(10, 10));
    {
        QFile file(m_dir->filePath(QStringLiteral("outside.png")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(outside);
    }
    const char *opf = R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata/>
  <manifest>
    <item id="content" href="content.xhtml" media-type="application/xhtml+xml"/>
    <item id="cover" href="../../outside.png" media-type="image/png" properties="cover-image"/>
  </manifest>
</package>)";
    const QString root = writeEpubDir({
        {QStringLiteral("META-INF/container.xml"), containerXml},
        {QStringLiteral("OEBPS/content.opf"), opf},
        {QStringLiteral("OEBPS/content.xhtml"), QByteArrayLiteral("<html/>")},
    });
    QVERIFY(!root.isEmpty());

    QVERIFY(EpubCover::extract(root).isNull());
}

QTEST_MAIN(EpubCoverTest)
#include "epubcovertest.moc"
