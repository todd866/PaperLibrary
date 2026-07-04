/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <QImage>
#include <QPainter>

#include "../shell/coverheuristic.h"

// Synthetic stand-ins for the page-one renders the cover pipeline produces
namespace
{
/** A near-white page with sparse black text lines — a typical paper. */
QImage whiteTextPage()
{
    QImage page(256, 320, QImage::Format_ARGB32);
    page.fill(QColor(252, 252, 250));
    QPainter painter(&page);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(20, 20, 20));
    for (int y = 40; y < 300; y += 16) {
        painter.drawRect(24, y, 208, 2);
    }
    return page;
}

/** A fully colorful page, e.g. a photograph or illustrated cover. */
QImage colorfulGradient()
{
    QImage page(256, 320, QImage::Format_ARGB32);
    QPainter painter(&page);
    for (int x = 0; x < 256; ++x) {
        painter.setPen(QColor::fromHsvF(x / 256.0, 0.8, 0.9));
        painter.drawLine(x, 0, x, 319);
    }
    return page;
}

/** A designed book cover: saturated red field with a white title block. */
QImage redBookCover()
{
    QImage page(256, 320, QImage::Format_ARGB32);
    page.fill(QColor(168, 32, 42));
    QPainter painter(&page);
    painter.fillRect(QRect(24, 40, 208, 70), QColor(250, 248, 244));
    return page;
}

/** A grayscale photograph page: colorless but almost no white paper. */
QImage grayscalePhotoPage()
{
    QImage page(256, 320, QImage::Format_ARGB32);
    QPainter painter(&page);
    for (int y = 0; y < 320; ++y) {
        const int v = 30 + (200 * y) / 320;
        painter.setPen(QColor(v, v, v));
        painter.drawLine(0, y, 255, y);
    }
    return page;
}
}

class CoverHeuristicTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testWhiteTextPageGenerates();
    void testColorfulGradientKeepsRender();
    void testRedCoverWithTitleBlockKeepsRender();
    void testGrayscalePhotoPageKeepsRender();
    void testBlankPageGenerates();
    void testNullImageGenerates();
    void testScoresExposeTheEvidence();
};

void CoverHeuristicTest::testWhiteTextPageGenerates()
{
    QCOMPARE(CoverHeuristic::analyze(whiteTextPage()), CoverHeuristic::GenerateTypographic);
}

void CoverHeuristicTest::testColorfulGradientKeepsRender()
{
    QCOMPARE(CoverHeuristic::analyze(colorfulGradient()), CoverHeuristic::KeepRender);
}

void CoverHeuristicTest::testRedCoverWithTitleBlockKeepsRender()
{
    QCOMPARE(CoverHeuristic::analyze(redBookCover()), CoverHeuristic::KeepRender);
}

void CoverHeuristicTest::testGrayscalePhotoPageKeepsRender()
{
    QCOMPARE(CoverHeuristic::analyze(grayscalePhotoPage()), CoverHeuristic::KeepRender);
}

void CoverHeuristicTest::testBlankPageGenerates()
{
    QImage blank(256, 320, QImage::Format_ARGB32);
    blank.fill(QColor(253, 253, 251));
    QCOMPARE(CoverHeuristic::analyze(blank), CoverHeuristic::GenerateTypographic);
}

void CoverHeuristicTest::testNullImageGenerates()
{
    QCOMPARE(CoverHeuristic::analyze(QImage()), CoverHeuristic::GenerateTypographic);
}

void CoverHeuristicTest::testScoresExposeTheEvidence()
{
    CoverHeuristic::Score score;

    // The text page is colorless and mostly paper
    CoverHeuristic::analyze(whiteTextPage(), &score);
    QVERIFY(score.colorfulness < 0.05);
    QVERIFY(score.inkCoverage > 0.01);
    QVERIFY(score.inkCoverage < 0.25);
    QVERIFY(score.variance > 0.0);

    // The red cover is saturated and almost entirely ink
    CoverHeuristic::analyze(redBookCover(), &score);
    QVERIFY(score.colorfulness > 0.5);
    QVERIFY(score.inkCoverage > 0.5);

    // The gradient earns its keep on color alone
    CoverHeuristic::analyze(colorfulGradient(), &score);
    QVERIFY(score.colorfulness > 0.5);

    // The grayscale photo earns its keep on ink coverage alone
    CoverHeuristic::analyze(grayscalePhotoPage(), &score);
    QVERIFY(score.colorfulness < 0.05);
    QVERIFY(score.inkCoverage > 0.5);

    // A null image scores zero across the board
    CoverHeuristic::analyze(QImage(), &score);
    QCOMPARE(score.colorfulness, 0.0);
    QCOMPARE(score.inkCoverage, 0.0);
    QCOMPARE(score.variance, 0.0);
}

QTEST_MAIN(CoverHeuristicTest)
#include "coverheuristictest.moc"
