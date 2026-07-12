/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QtTest>

#include <QStandardPaths>
#include <QUrl>

#include "../shell/readingprogress.h"

class ReadingProgressTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true); // an isolated config, never the user's
    }

    void testFractionFromCounts()
    {
        QCOMPARE(ReadingProgress::fractionFromCounts(0, 0), -1.0);  // unknown length
        QCOMPARE(ReadingProgress::fractionFromCounts(5, 0), -1.0);
        QCOMPARE(ReadingProgress::fractionFromCounts(0, 10), 0.0);
        QCOMPARE(ReadingProgress::fractionFromCounts(5, 10), 0.5);
        QCOMPARE(ReadingProgress::fractionFromCounts(10, 10), 1.0);
        QCOMPARE(ReadingProgress::fractionFromCounts(-3, 10), 0.0); // clamps below
        QCOMPARE(ReadingProgress::fractionFromCounts(99, 10), 1.0); // clamps above
    }

    void testRecordAndReadBackByPath()
    {
        const QString path = QStringLiteral("/tmp/some/book.epub");
        ReadingProgress::record(QUrl::fromLocalFile(path), 0.42);
        QVERIFY(qAbs(ReadingProgress::fractionForPath(path) - 0.42) < 1e-9);
    }

    void testAnUnrecordedDocumentReadsNegative()
    {
        QVERIFY(ReadingProgress::fractionForPath(QStringLiteral("/tmp/never/opened.pdf")) < 0.0);
        QVERIFY(ReadingProgress::fractionForPath(QString()) < 0.0);
    }

    void testAppleBooksSyncMatchesByNormalisedTitle()
    {
        QHash<QString,double> byTitle;
        byTitle.insert(ReadingProgress::titleKey(QStringLiteral("The Power Broker")), 0.62);
        ReadingProgress::syncFromAppleBooks(byTitle);
        // Case/punctuation-insensitive match, so "the power broker!" finds it.
        QVERIFY(qAbs(ReadingProgress::fractionForTitle(QStringLiteral("the POWER broker!")) - 0.62) < 1e-9);
        QVERIFY(ReadingProgress::fractionForTitle(QStringLiteral("Some Other Book")) < 0.0);
    }

    void testTitleKeyedRecordBridgesCopiesAndOutranksAppleBooks()
    {
        // Apple Books knows this book at 0.10; the reader has read it HERE to 0.55. A tile showing a
        // different path copy resolves the real PaperLibrary position by title, not the Apple stub.
        QHash<QString,double> apple;
        apple.insert(ReadingProgress::titleKey(QStringLiteral("A Bridged Book")), 0.10);
        ReadingProgress::syncFromAppleBooks(apple);
        ReadingProgress::record(QUrl::fromLocalFile(QStringLiteral("/some/copy/a.epub")),
                                QStringLiteral("A Bridged Book"), 0.55);
        // By title (any copy), native reading wins over Apple Books.
        QVERIFY(qAbs(ReadingProgress::fractionForTitle(QStringLiteral("a bridged book")) - 0.55) < 1e-9);
        // And the exact path still resolves directly.
        QVERIFY(qAbs(ReadingProgress::fractionForPath(QStringLiteral("/some/copy/a.epub")) - 0.55) < 1e-9);
        // A different path copy of the same title has no path entry, but the title bridge covers it.
        QVERIFY(ReadingProgress::fractionForPath(QStringLiteral("/other/copy/a.epub")) < 0.0);
        QVERIFY(qAbs(ReadingProgress::fractionForTitle(QStringLiteral("A Bridged Book")) - 0.55) < 1e-9);
    }

    void testARecordedFractionIsClampedAndUnknownIsNotWritten()
    {
        const QString path = QStringLiteral("/tmp/clamp/me.pdf");
        ReadingProgress::record(QUrl::fromLocalFile(path), 1.7);
        QCOMPARE(ReadingProgress::fractionForPath(path), 1.0);
        // An "unknown" (-1) must never overwrite a real recorded value.
        ReadingProgress::record(QUrl::fromLocalFile(path), -1.0);
        QCOMPARE(ReadingProgress::fractionForPath(path), 1.0);
    }
};

QTEST_MAIN(ReadingProgressTest)
#include "readingprogresstest.moc"
