/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QFileInfo>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPointer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWidget>

#include "../shell/pdfview.h"

namespace
{
bool writeFixturePdf(const QString &path, int pages = 2)
{
    QPdfWriter writer(path);
    writer.setResolution(72);
    writer.setPageSize(QPageSize(QPageSize::A4));

    QPainter painter(&writer);
    if (!painter.isActive()) {
        return false;
    }
    for (int page = 1; page <= pages; ++page) {
        if (page > 1 && !writer.newPage()) {
            painter.end();
            return false;
        }
        painter.drawText(QPoint(72, 96),
                         QStringLiteral("PaperLibrary PDF lifecycle fixture — page %1").arg(page));
    }
    painter.end();
    return QFileInfo(path).size() > 0;
}
}

class PdfViewLifecycleTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        PdfView::setReaderMotionEnabled(false);
    }

    void testChapterProgressLocatesThePageWithinItsChapter()
    {
        // Three chapters in a 30-page book: Ch1 p1, Ch2 p11, Ch3 p21.
        const QList<PdfView::AiNavigationEntry> outline = {
            {QStringLiteral("One"), 1, 1},
            {QStringLiteral("Two"), 11, 1},
            {QStringLiteral("Three"), 21, 1},
        };

        // Middle of chapter two: page 15 of the 11..20 span.
        auto p = PdfView::computeChapterProgress(outline, 15, 30);
        QVERIFY(p.valid);
        QCOMPARE(p.title, QStringLiteral("Two"));
        QCOMPARE(p.chapterIndex, 2);
        QCOMPARE(p.chapterCount, 3);
        QCOMPARE(p.pagesLeftInChapter, 6); // pages 15->21 (next chapter starts at 21)
        QVERIFY(qAbs(p.fraction - (4.0 / 10.0)) < 1e-6); // 4 pages into a 10-page chapter

        // Last chapter runs to the end of the book (page 30).
        auto last = PdfView::computeChapterProgress(outline, 25, 30);
        QCOMPARE(last.title, QStringLiteral("Three"));
        QCOMPARE(last.chapterIndex, 3);
        QCOMPARE(last.pagesLeftInChapter, 6); // 25->31 (one past the last page)

        // On the very last page of a chapter, one page remains before the next.
        QCOMPARE(PdfView::computeChapterProgress(outline, 10, 30).pagesLeftInChapter, 1);
        QCOMPARE(PdfView::computeChapterProgress(outline, 10, 30).title, QStringLiteral("One"));
    }

    void testChapterProgressHandlesFrontMatterAndNoOutline()
    {
        const QList<PdfView::AiNavigationEntry> outline = {{QStringLiteral("Chapter 1"), 5, 1}};
        // Before the first chapter -> front matter, not chapter 1.
        auto front = PdfView::computeChapterProgress(outline, 2, 20);
        QVERIFY(front.valid);
        QCOMPARE(front.chapterIndex, 0);
        QVERIFY(!front.title.isEmpty()); // labelled, e.g. "Front matter"

        // No outline at all -> nothing to show.
        QVERIFY(!PdfView::computeChapterProgress({}, 3, 20).valid);
    }

    void testChapterProgressUsesTopLevelEntriesAsChapters()
    {
        // Subsections (level 2) must not count as chapter ends: end-of-chapter is the next level-1.
        const QList<PdfView::AiNavigationEntry> outline = {
            {QStringLiteral("Chapter 1"), 1, 1},
            {QStringLiteral("1.1"), 3, 2},
            {QStringLiteral("1.2"), 6, 2},
            {QStringLiteral("Chapter 2"), 10, 1},
        };
        auto p = PdfView::computeChapterProgress(outline, 4, 20);
        QCOMPARE(p.title, QStringLiteral("Chapter 1")); // still in chapter 1, not "1.1"
        QCOMPARE(p.chapterCount, 2);
        QCOMPARE(p.pagesLeftInChapter, 6); // 4 -> 10, the next chapter
    }

    void testOpenAndDestroyWithParent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pdfPath = dir.filePath(QStringLiteral("two-pages.pdf"));
        QVERIFY(writeFixturePdf(pdfPath));

        // Parent destruction matches Shell -> tab widget -> PdfView teardown. Keeping the widget
        // visible and navigating once ensures QPdfView has live scroll bars/page-navigator state
        // before QPdfDocument closes.
        auto *host = new QWidget;
        host->resize(900, 700);
        auto *layout = new QVBoxLayout(host);
        auto *view = new PdfView(host);
        layout->addWidget(view);
        host->show();

        QVERIFY(view->open(QUrl::fromLocalFile(pdfPath)));
        QTRY_VERIFY_WITH_TIMEOUT(view->pageCount() >= 2, 10000);
        view->goToPageOneBased(2);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);

        QPointer<PdfView> guard(view);
        delete host;
        QVERIFY(guard.isNull());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
    }

    void testDestroyWhileScrolledDoesNotReenterTheDyingView()
    {
        // Closing a tab whose PDF is scrolled aborted the app:
        //   ~PdfView -> ~QWidget -> deleteChildren() -> ~QPdfDocument -> QPdfDocument::close()
        //     -> QAbstractSlider::setRange(0,0) -> setValue(0) (the value was non-zero)
        //     -> ShellPdfView::scrollContentsBy -> QPdfPageNavigator::jump()
        //     -> a signal into a PdfView slot -> assertObjectType<PdfView> -> qFatal.
        // The precondition the old test missed is a NON-ZERO vertical scroll value, so assert it.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pdfPath = dir.filePath(QStringLiteral("many-pages.pdf"));
        QVERIFY(writeFixturePdf(pdfPath, 12));

        auto *host = new QWidget;
        host->resize(700, 500);
        auto *layout = new QVBoxLayout(host);
        auto *view = new PdfView(host);
        layout->addWidget(view);
        host->show();

        QVERIFY(view->open(QUrl::fromLocalFile(pdfPath)));
        QTRY_VERIFY_WITH_TIMEOUT(view->pageCount() >= 12, 10000);
        view->goToPageOneBased(9);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 500);

        // goToPageOneBased() alone leaves the scroll bar at 0 headlessly. The crash needs a
        // non-zero value so QPdfDocument::close()'s setRange(0,0) clamps it and re-enters
        // scrollContentsBy on the dying view.
        QScrollBar *scrolled = nullptr;
        const QList<QScrollBar *> bars = view->findChildren<QScrollBar *>();
        for (QScrollBar *bar : bars) {
            if (bar->orientation() == Qt::Vertical && bar->maximum() > 0) {
                bar->setValue(bar->maximum() / 2);
                scrolled = bar;
                break;
            }
        }
        QVERIFY2(scrolled != nullptr, "no vertical scroll range; fixture too short?");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        QVERIFY2(scrolled->value() > 0,
                 "the reproduction needs a non-zero vertical scroll value before teardown");

        QPointer<PdfView> guard(view);
        delete host;                          // aborts before the destructor tears QtPdf down
        QVERIFY(guard.isNull());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
    }
};

QTEST_MAIN(PdfViewLifecycleTest)

#include "pdfviewlifecycletest.moc"
