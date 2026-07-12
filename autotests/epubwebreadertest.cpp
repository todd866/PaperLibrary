/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../shell/epubwebreader.h"

#include <KZip>

#include <QTemporaryDir>
#include <QTest>

#include <utility>

using namespace EpubWebReaderCore;

namespace
{
const char *containerXml = R"(<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>)";

QString writeEpub(QTemporaryDir *dir, const QList<std::pair<QString, QByteArray>> &files)
{
    const QString path = dir->filePath(QStringLiteral("book.epub"));
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
}

class EpubWebReaderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testEpub3NavDocument();
    void testWeakNcxUsesInBookContentsLinks();
    void testFallbackNavigationFromSpineHeadings();
    void testOversizedMarkupEntryIsNotRead();
    void testReportedReaderPositionTitle();
};

void EpubWebReaderTest::testEpub3NavDocument()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray opf = R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata><dc:title xmlns:dc="http://purl.org/dc/elements/1.1/">Structured Book</dc:title></metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="chap1" href="chap1.xhtml" media-type="application/xhtml+xml"/>
    <item id="chap2" href="chap2.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="chap1"/>
    <itemref idref="chap2"/>
  </spine>
</package>)";
    const QByteArray nav = R"(<?xml version="1.0"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <body>
    <nav epub:type="toc">
      <ol>
        <li><a href="chap1.xhtml#start">Opening</a>
          <ol><li><a href="chap2.xhtml">Second Chapter</a></li></ol>
        </li>
      </ol>
    </nav>
  </body>
</html>)";

    const QString path = writeEpub(&dir, {
                                             {QStringLiteral("META-INF/container.xml"), containerXml},
                                             {QStringLiteral("OEBPS/content.opf"), opf},
                                             {QStringLiteral("OEBPS/nav.xhtml"), nav},
                                             {QStringLiteral("OEBPS/chap1.xhtml"), QByteArrayLiteral("<html><body><h1>Ignored Fallback</h1></body></html>")},
                                             {QStringLiteral("OEBPS/chap2.xhtml"), QByteArrayLiteral("<html><body><h1>Chapter Two</h1></body></html>")},
                                         });
    QVERIFY(!path.isEmpty());

    const EpubInspection inspection = inspectEpub(path);
    QVERIFY(inspection.supported());
    QCOMPARE(inspection.navigation.size(), 2);
    QCOMPARE(inspection.navigation.at(0).title, QStringLiteral("Opening"));
    QCOMPARE(inspection.navigation.at(0).spineIndex, 0);
    QCOMPARE(inspection.navigation.at(0).fragment, QStringLiteral("start"));
    QCOMPARE(inspection.navigation.at(0).level, 1);
    QCOMPARE(inspection.navigation.at(1).title, QStringLiteral("Second Chapter"));
    QCOMPARE(inspection.navigation.at(1).spineIndex, 1);
    QCOMPARE(inspection.navigation.at(1).level, 2);
}

void EpubWebReaderTest::testWeakNcxUsesInBookContentsLinks()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray opf = R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0">
  <metadata><dc:title xmlns:dc="http://purl.org/dc/elements/1.1/">Weak TOC Book</dc:title></metadata>
  <manifest>
    <item id="titlepage" href="titlepage.xhtml" media-type="application/xhtml+xml"/>
    <item id="contents" href="contents.xhtml" media-type="application/xhtml+xml"/>
    <item id="chap1" href="chap1.xhtml" media-type="application/xhtml+xml"/>
    <item id="chap2" href="chap2.xhtml" media-type="application/xhtml+xml"/>
    <item id="chap3" href="chap3.xhtml" media-type="application/xhtml+xml"/>
    <item id="chap4" href="chap4.xhtml" media-type="application/xhtml+xml"/>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
  </manifest>
  <spine toc="ncx">
    <itemref idref="titlepage"/>
    <itemref idref="contents"/>
    <itemref idref="chap1"/>
    <itemref idref="chap2"/>
    <itemref idref="chap3"/>
    <itemref idref="chap4"/>
  </spine>
</package>)";
    const QByteArray ncx = R"(<?xml version="1.0"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/">
  <navMap>
    <navPoint id="start" playOrder="1">
      <navLabel><text>Start</text></navLabel>
      <content src="titlepage.xhtml"/>
    </navPoint>
  </navMap>
</ncx>)";
    const QByteArray contents = R"(<?xml version="1.0"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <body>
    <div><a href="chap1.xhtml#c1">Chapter One Bran</a></div>
    <div><a href="chap2.xhtml#c2">Chapter Two Catelyn</a></div>
    <div><a href="chap3.xhtml#c3">Chapter Three Daenerys</a></div>
    <div><a href="chap4.xhtml#c4">Chapter Four Eddard</a></div>
  </body>
</html>)";

    const QString path = writeEpub(&dir, {
                                             {QStringLiteral("META-INF/container.xml"), containerXml},
                                             {QStringLiteral("OEBPS/content.opf"), opf},
                                             {QStringLiteral("OEBPS/toc.ncx"), ncx},
                                             {QStringLiteral("OEBPS/titlepage.xhtml"), QByteArrayLiteral("<html><body><h1>Cover</h1></body></html>")},
                                             {QStringLiteral("OEBPS/contents.xhtml"), contents},
                                             {QStringLiteral("OEBPS/chap1.xhtml"), QByteArrayLiteral("<html><body><h1 id=\"c1\">Bran</h1></body></html>")},
                                             {QStringLiteral("OEBPS/chap2.xhtml"), QByteArrayLiteral("<html><body><h1 id=\"c2\">Catelyn</h1></body></html>")},
                                             {QStringLiteral("OEBPS/chap3.xhtml"), QByteArrayLiteral("<html><body><h1 id=\"c3\">Daenerys</h1></body></html>")},
                                             {QStringLiteral("OEBPS/chap4.xhtml"), QByteArrayLiteral("<html><body><h1 id=\"c4\">Eddard</h1></body></html>")},
                                         });
    QVERIFY(!path.isEmpty());

    const EpubInspection inspection = inspectEpub(path);
    QVERIFY(inspection.supported());
    QCOMPARE(inspection.navigation.size(), 4);
    QCOMPARE(inspection.navigation.at(0).title, QStringLiteral("Chapter One Bran"));
    QCOMPARE(inspection.navigation.at(0).spineIndex, 2);
    QCOMPARE(inspection.navigation.at(0).fragment, QStringLiteral("c1"));
    QCOMPARE(inspection.navigation.at(2).title, QStringLiteral("Chapter Three Daenerys"));
    QCOMPARE(inspection.navigation.at(2).spineIndex, 4);
    QCOMPARE(inspection.navigation.at(2).fragment, QStringLiteral("c3"));
}

void EpubWebReaderTest::testFallbackNavigationFromSpineHeadings()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray opf = R"(<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata><dc:title xmlns:dc="http://purl.org/dc/elements/1.1/">Fallback Book</dc:title></metadata>
  <manifest>
    <item id="chap1" href="chap1.xhtml" media-type="application/xhtml+xml"/>
    <item id="chap2" href="chap2.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="chap1"/>
    <itemref idref="chap2"/>
  </spine>
</package>)";

    const QString path = writeEpub(&dir, {
                                             {QStringLiteral("META-INF/container.xml"), containerXml},
                                             {QStringLiteral("OEBPS/content.opf"), opf},
                                             {QStringLiteral("OEBPS/chap1.xhtml"), QByteArrayLiteral("<html><head><title>Title One</title></head><body><h1>Chapter One</h1></body></html>")},
                                             {QStringLiteral("OEBPS/chap2.xhtml"), QByteArrayLiteral("<html><head><title>Title Two</title></head><body></body></html>")},
                                         });
    QVERIFY(!path.isEmpty());

    const EpubInspection inspection = inspectEpub(path);
    QVERIFY(inspection.supported());
    QCOMPARE(inspection.navigation.size(), 2);
    QCOMPARE(inspection.navigation.at(0).title, QStringLiteral("Chapter One"));
    QCOMPARE(inspection.navigation.at(0).spineIndex, 0);
    QCOMPARE(inspection.navigation.at(1).title, QStringLiteral("Title Two"));
    QCOMPARE(inspection.navigation.at(1).spineIndex, 1);
}

void EpubWebReaderTest::testOversizedMarkupEntryIsNotRead()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeEpub(&dir, {
                                             {QStringLiteral("large.xhtml"), QByteArray(8 * 1024 * 1024 + 1, 'x')},
                                         });
    KZip zip(path);
    QVERIFY(zip.open(QIODevice::ReadOnly));
    QVERIFY(readArchiveFile(zip, QStringLiteral("large.xhtml")).isEmpty());
    QCOMPARE(readArchiveFile(zip, QStringLiteral("large.xhtml"), 9 * 1024 * 1024).size(), 8 * 1024 * 1024 + 1);
}

void EpubWebReaderTest::testReportedReaderPositionTitle()
{
    const QString title = reportedReaderPositionTitle(1234.6, 7);
    const ReportedReaderPosition parsed = parseReportedReaderPositionTitle(title);
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.scrollOffset, 1235.0);

    const ReportedReaderPosition negative = parseReportedReaderPositionTitle(reportedReaderPositionTitle(-200.0, 8));
    QVERIFY(negative.valid);
    QCOMPARE(negative.scrollOffset, 0.0);

    QVERIFY(!parseReportedReaderPositionTitle(QStringLiteral("A Game of Thrones")).valid);
    QVERIFY(!parseReportedReaderPositionTitle(QStringLiteral("__paperlibrary_epub_position__|abc|123")).valid);
    QVERIFY(!parseReportedReaderPositionTitle(QStringLiteral("__paperlibrary_epub_position__|9|not-a-number")).valid);
    QVERIFY(!parseReportedReaderPositionTitle(QStringLiteral("__paperlibrary_epub_position__|9|1000000001")).valid);
}

QTEST_GUILESS_MAIN(EpubWebReaderTest)

#include "epubwebreadertest.moc"
