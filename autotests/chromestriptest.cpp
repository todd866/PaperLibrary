/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QtTest>

#include <QApplication>
#include <QRect>
#include <QToolButton>

#include <KConfig>
#include <KConfigGroup>

#include "../shell/chrometabbar.h"

/**
 * Headless coverage for the titlebar-tabs strip logic that does not need a
 * live NSWindow: the leading-inset rule (windowed vs fullscreen), the strip
 * height propagation, the drag-region hit-test classification, and the
 * TitlebarTabs escape-hatch config fallback. The NSWindow adoption itself is
 * verified separately by launching the app (see todd-dev-notes.md).
 */
class ChromeStripTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testComputeEffectiveInset();
    void testEffectiveInsetState();
    void testInsetShiftsBar();
    void testStripHeightPropagates();
    void testHitTestClassification();
    void testConfigFallback();
};

void ChromeStripTest::testComputeEffectiveInset()
{
    // Windowed keeps the clearance; fullscreen collapses it; negatives clamp.
    QCOMPARE(ChromeTabStrip::computeEffectiveInset(80, false), 80);
    QCOMPARE(ChromeTabStrip::computeEffectiveInset(80, true), 0);
    QCOMPARE(ChromeTabStrip::computeEffectiveInset(0, false), 0);
    QCOMPARE(ChromeTabStrip::computeEffectiveInset(-5, false), 0);
}

void ChromeStripTest::testEffectiveInsetState()
{
    ChromeTabStrip strip;
    QCOMPARE(strip.effectiveInset(), 0); // no inset until set

    strip.setLeadingInset(78);
    QCOMPARE(strip.effectiveInset(), 78);

    strip.setFullscreen(true);
    QCOMPARE(strip.effectiveInset(), 0); // collapses in fullscreen

    strip.setFullscreen(false);
    QCOMPARE(strip.effectiveInset(), 78); // restored on the way out
}

void ChromeStripTest::testInsetShiftsBar()
{
    ChromeTabStrip strip;
    strip.tabBar()->addTab(QStringLiteral("One"));
    strip.setLeadingInset(80);
    strip.resize(600, 40);
    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));

    // The bar sits at x = inset inside the strip; fullscreen collapses it.
    QCOMPARE(strip.tabBar()->pos().x(), 80);

    strip.setFullscreen(true);
    QTest::qWait(1);
    QCOMPARE(strip.tabBar()->pos().x(), 0);
}

void ChromeStripTest::testStripHeightPropagates()
{
    ChromeTabStrip strip;
    strip.tabBar()->addTab(QStringLiteral("One"));
    strip.setStripHeight(44);
    QCOMPARE(strip.minimumHeight(), 44);
    QCOMPARE(strip.maximumHeight(), 44);

    strip.resize(400, 44);
    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));
    // The taller row flows into the tabs themselves.
    QCOMPARE(strip.tabBar()->tabRect(0).height(), 44);
}

void ChromeStripTest::testHitTestClassification()
{
    ChromeTabStrip strip;
    ChromeTabBar *const bar = strip.tabBar();
    bar->addTab(QStringLiteral("One"));
    bar->addTab(QStringLiteral("Two"));
    strip.resize(800, 35); // wide, so the tabs cap out and leave bare strip
    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));

    // A point in the middle of the first tab classifies as a tab.
    QCOMPARE(bar->hitTest(bar->tabRect(0).center()), ChromeTabBar::StripHit::Tab);

    // The "+" button after the last tab classifies as the new-tab button.
    const QRect plus = bar->newTabButtonRect();
    QVERIFY(plus.isValid());
    QCOMPARE(bar->hitTest(plus.center()), ChromeTabBar::StripHit::NewTabButton);

    // Bare strip past everything drags the window.
    QCOMPARE(bar->hitTest(QPoint(780, 17)), ChromeTabBar::StripHit::Empty);
}

void ChromeStripTest::testConfigFallback()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    KConfig config(dir.filePath(QStringLiteral("titlebartabstest")), KConfig::SimpleConfig);
    KConfigGroup general = config.group(QStringLiteral("General"));

    // Default on: titlebar tabs unless explicitly disabled.
    QVERIFY(chromeTitlebarTabsEnabled(general));

    general.writeEntry("TitlebarTabs", false);
    QVERIFY(!chromeTitlebarTabsEnabled(general));

    general.writeEntry("TitlebarTabs", true);
    QVERIFY(chromeTitlebarTabsEnabled(general));
}

int main(int argc, char *argv[])
{
    // Offscreen: deterministic geometry/visibility without popping windows.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ChromeStripTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "chromestriptest.moc"
