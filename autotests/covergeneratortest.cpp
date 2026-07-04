/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <QBuffer>
#include <QDir>
#include <QImage>
#include <QPalette>

#include <algorithm>

#include "../shell/covergenerator.h"

namespace
{
QPalette lightPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, Qt::white);
    return palette;
}

QPalette darkPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(43, 43, 43));
    return palette;
}

CoverGenerator::CoverSpec paperSpec()
{
    return {QStringLiteral("Bayesian Descriptions Are Not Mechanisms"), QStringLiteral("Ian Todd"), QStringLiteral("2026 · Manuscript"), QStringLiteral("Neuroscience")};
}

QByteArray pngBytes(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

const QSize CardSize(272, 340); // 2x the library tile's cover box

// Sample points along the card border, all outside the content margins:
// on the spine, in the field's gradient band, above and below the text
QList<QPoint> borderSamplePoints(const QSize &size)
{
    const int w = size.width();
    const int h = size.height();
    return {QPoint(2, 2), QPoint(w - 3, 2), QPoint(2, h - 3), QPoint(w - 3, h - 3), QPoint(w / 2, 2), QPoint(w / 2, h - 3), QPoint(2, h / 2), QPoint(w - 3, h / 2)};
}
}

class CoverGeneratorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSizeMatchesRequest();
    void testDeterminism();
    void testTagHueGoldenValues();
    void testTagHueIsCaseInsensitive();
    void testEmptyTagFallsBackToTitle();
    void testDarkModeDeepensNotInverts();
    void testSpineStripDarkensLeftEdge();
    void testFieldGradientRunsLighterTopToDarkerBottom();
    void testKeylineRegisters();
    void testLongTitleStaysInsideCard();
    void testTitleWrapsAtWordBoundaries();
    void testOversizedSingleWordElides();
    void testLastLineNotAnOrphan();
    void testEmptyAuthorsAndJournalRenderCleanly();
    void testLightAndDarkDiffer();
    void testUntitledSpecStillRenders();
    void renderSampleGallery();
};

void CoverGeneratorTest::testSizeMatchesRequest()
{
    const QImage card = CoverGenerator::generate(paperSpec(), CardSize, lightPalette());
    QCOMPARE(card.size(), CardSize);
    QVERIFY(!card.isNull());
}

void CoverGeneratorTest::testDeterminism()
{
    const QImage first = CoverGenerator::generate(paperSpec(), CardSize, lightPalette());
    const QImage second = CoverGenerator::generate(paperSpec(), CardSize, lightPalette());
    QCOMPARE(first, second);
    QCOMPARE(pngBytes(first), pngBytes(second)); // identical image bytes, not just pixels
}

void CoverGeneratorTest::testTagHueGoldenValues()
{
    // Pinned so a hash or palette change is a conscious decision: these are
    // the user's real shelf genres, each in its own hue family
    QCOMPARE(CoverGenerator::accentColor(QStringLiteral("Neuroscience"), false), QColor(0x2E, 0x59, 0x43)); // pine
    QCOMPARE(CoverGenerator::accentColor(QStringLiteral("Aviation"), false), QColor(0x9A, 0x7B, 0x2E));     // ochre
    QCOMPARE(CoverGenerator::accentColor(QStringLiteral("Peer Review"), false), QColor(0x9C, 0x5B, 0x41));  // clay
    QCOMPARE(CoverGenerator::accentColor(QStringLiteral("Manuscript"), false), QColor(0x4A, 0x3B, 0x4F));   // charcoal-plum
}

void CoverGeneratorTest::testTagHueIsCaseInsensitive()
{
    QCOMPARE(CoverGenerator::accentColor(QStringLiteral("NEUROSCIENCE"), false), CoverGenerator::accentColor(QStringLiteral("neuroscience"), false));
    QCOMPARE(CoverGenerator::accentColor(QStringLiteral("  Aviation "), false), CoverGenerator::accentColor(QStringLiteral("aviation"), false));
}

void CoverGeneratorTest::testEmptyTagFallsBackToTitle()
{
    // An untagged card still gets a deterministic accent, seeded by title
    CoverGenerator::CoverSpec untagged = paperSpec();
    untagged.tag.clear();
    const QImage first = CoverGenerator::generate(untagged, CardSize, lightPalette());
    const QImage second = CoverGenerator::generate(untagged, CardSize, lightPalette());
    QCOMPARE(first, second);

    // Its border — spine, gradient, keyline — matches a card whose tag IS
    // the title string: same seed, same accent (the chip never reaches
    // the border, so only the accent decides these pixels)
    CoverGenerator::CoverSpec titleTagged = untagged;
    titleTagged.tag = untagged.title;
    const QImage reference = CoverGenerator::generate(titleTagged, CardSize, lightPalette());
    const QList<QPoint> samples = borderSamplePoints(CardSize);
    for (const QPoint &point : samples) {
        QCOMPARE(first.pixelColor(point.x(), point.y()), reference.pixelColor(point.x(), point.y()));
    }
}

void CoverGeneratorTest::testDarkModeDeepensNotInverts()
{
    const QColor light = CoverGenerator::accentColor(QStringLiteral("Neuroscience"), false);
    const QColor dark = CoverGenerator::accentColor(QStringLiteral("Neuroscience"), true);
    QVERIFY(dark.lightnessF() < light.lightnessF()); // deepened…
    QCOMPARE(dark.hslHueF(), light.hslHueF());       // …same hue, never inverted
}

void CoverGeneratorTest::testSpineStripDarkensLeftEdge()
{
    // A darker spine strip runs the full left edge, ~23% darker than the
    // field — deep enough to register at tile size, same hue family
    const QImage card = CoverGenerator::generate(paperSpec(), CardSize, lightPalette());
    const int h = card.height();
    const QColor spineTop = card.pixelColor(3, 3);
    const QColor spineMid = card.pixelColor(3, h / 2);
    const QColor spineBottom = card.pixelColor(3, h - 4);
    const QColor fieldMid = card.pixelColor(card.width() - 3, h / 2);

    const int accentHue = CoverGenerator::accentColor(paperSpec().tag, false).hslHue();
    for (const QColor &spine : {spineTop, spineMid, spineBottom}) {
        QVERIFY2(spine.lightnessF() < fieldMid.lightnessF() * 0.9, "the spine must sit clearly darker than the field");
        QVERIFY2(qAbs(spine.hslHue() - accentHue) <= 2, "the spine must stay in the accent's hue family"); // 8-bit rounding may nudge the hue
    }
}

void CoverGeneratorTest::testFieldGradientRunsLighterTopToDarkerBottom()
{
    // The field carries a gentle vertical gradient: slightly lighter at
    // the top than at the bottom, flat enough to stay Penguin-quiet
    const QImage card = CoverGenerator::generate(paperSpec(), CardSize, lightPalette());
    const int x = card.width() - 3; // right band: field only, no spine, no text
    const QColor top = card.pixelColor(x, 3);
    const QColor bottom = card.pixelColor(x, card.height() - 4);
    QVERIFY2(top.lightness() > bottom.lightness(), "field must run lighter top to darker bottom");
    QVERIFY2(top.lightnessF() < bottom.lightnessF() * 1.35, "the gradient must stay gentle");
}

void CoverGeneratorTest::testKeylineRegisters()
{
    // The inner keyline must actually read at tile size: clearly lighter
    // than the field it frames
    const QImage card = CoverGenerator::generate(paperSpec(), CardSize, lightPalette());
    const int w = card.width();
    const int h = card.height();
    // The keyline's right vertical run sits at 5.5% of the width from the
    // right edge; the field reference sits between it and the card edge
    const QColor keyline = card.pixelColor(qRound(w - w * 0.055), h / 2);
    const QColor field = card.pixelColor(w - 3, h / 2);
    QVERIFY2(keyline.lightnessF() > field.lightnessF() + 0.05, "the keyline must register against the field");
}

void CoverGeneratorTest::testLongTitleStaysInsideCard()
{
    CoverGenerator::CoverSpec spec = paperSpec();
    spec.tag = QStringLiteral("Stress");
    spec.title = QStringLiteral(
        "An Uncommonly Long and Meandering Treatise Upon the Statistical Mechanics of Overflowing Text Boxes, "
        "Considered as a Problem in Typography, Together with Notes Toward a General Theory of Shrink-to-Fit "
        "Layout in Small Rectangles of Deterministic Accent Color");
    const QImage card = CoverGenerator::generate(spec, CardSize, lightPalette());
    QCOMPARE(card.size(), CardSize);

    // Nothing may paint outside the content margins: every border sample
    // of the stressed card matches the same pixel of a short-titled card
    // with the same tag — spine, gradient and keyline, but never ink
    CoverGenerator::CoverSpec shortSpec = spec;
    shortSpec.title = QStringLiteral("Short");
    const QImage reference = CoverGenerator::generate(shortSpec, CardSize, lightPalette());
    const QList<QPoint> samples = borderSamplePoints(CardSize);
    for (const QPoint &point : samples) {
        QCOMPARE(card.pixelColor(point.x(), point.y()), reference.pixelColor(point.x(), point.y()));
    }
}

// Verifies the wrap policy: every title line is made of whole words of the
// title, except that a line may end in "…" — and then only its token right
// before the ellipsis may be a word cut short.
static void verifyWordBoundaries(const QStringList &lines, const QString &title)
{
    QVERIFY(!lines.isEmpty());
    const QStringList words = title.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const bool elided = line.endsWith(QStringLiteral("…"));
        QString bare = line;
        bare.chop(elided ? 1 : 0);
        const QStringList tokens = bare.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (int i = 0; i < tokens.size(); ++i) {
            const QString &token = tokens.at(i);
            if (words.contains(token)) {
                continue; // a whole word of the title
            }
            // Only the elided line's last token may be a truncated word —
            // and it must still be the *start* of some word, never a shard
            // from the middle
            QVERIFY2(elided && i == tokens.size() - 1, qPrintable(QStringLiteral("line breaks mid-word without ellipsis: \"%1\"").arg(line)));
            const bool prefixOfAWord = std::any_of(words.cbegin(), words.cend(), [&token](const QString &word) { return word.startsWith(token); });
            QVERIFY2(prefixOfAWord, qPrintable(QStringLiteral("elided token is not a word prefix: \"%1\"").arg(line)));
        }
    }
}

void CoverGeneratorTest::testTitleWrapsAtWordBoundaries()
{
    // Long words that character-wise wrapping used to break mid-word
    // ("Mountaine|ering"); word-boundary wrapping must keep them whole
    CoverGenerator::CoverSpec spec = paperSpec();
    spec.title = QStringLiteral("An Uncommonly Long Mountaineering Treatise on Neuropsychopharmacology and Crystallography");
    verifyWordBoundaries(CoverGenerator::titleLines(spec, CardSize), spec.title);

    // The wrapped card still paints at the requested size
    QCOMPARE(CoverGenerator::generate(spec, CardSize, lightPalette()).size(), CardSize);
}

void CoverGeneratorTest::testOversizedSingleWordElides()
{
    // A single word no font size can fit is the one case where cutting
    // into the word is allowed — and then only with an ellipsis
    CoverGenerator::CoverSpec spec = paperSpec();
    spec.title = QStringLiteral("Pneumonoultramicroscopicsilicovolcanoconiosis Redux");
    const QStringList lines = CoverGenerator::titleLines(spec, CardSize);
    verifyWordBoundaries(lines, spec.title);
    QVERIFY(std::any_of(lines.cbegin(), lines.cend(), [](const QString &line) { return line.endsWith(QStringLiteral("…")); }));
}

void CoverGeneratorTest::testLastLineNotAnOrphan()
{
    // Balanced breaking: greedy wrapping strands a lone short word on the
    // last line ("…and the|Bomb"); the cover must rebreak so the last
    // line carries at least two words or one substantial word
    const QStringList titles = {
        QStringLiteral("American Prometheus: The Triumph and Tragedy of Robert Oppenheimer and the Bomb"),
        QStringLiteral("The Character of Physical Law"),
        QStringLiteral("A General Theory of Love"),
        QStringLiteral("The Structure of Scientific Revolutions and After"),
        QStringLiteral("Consciousness and the Social Brain"),
    };
    for (const QString &title : titles) {
        CoverGenerator::CoverSpec spec = paperSpec();
        spec.title = title;
        const QStringList lines = CoverGenerator::titleLines(spec, CardSize);
        verifyWordBoundaries(lines, title);
        if (lines.size() < 2) {
            continue;
        }
        const QStringList lastWords = lines.last().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QVERIFY2(lastWords.size() >= 2 || lastWords.first().size() >= 6, qPrintable(QStringLiteral("orphaned last line \"%1\" in: %2 | %3").arg(lines.last(), title, lines.join(QLatin1Char('|')))));
    }
}

void CoverGeneratorTest::testEmptyAuthorsAndJournalRenderCleanly()
{
    CoverGenerator::CoverSpec bare = paperSpec();
    bare.authors.clear();
    bare.yearJournal.clear();

    const QImage card = CoverGenerator::generate(bare, CardSize, lightPalette());
    QVERIFY(!card.isNull());
    QCOMPARE(card, CoverGenerator::generate(bare, CardSize, lightPalette()));

    // The authors and footer lines really carry paint: dropping them changes the card
    QVERIFY(card != CoverGenerator::generate(paperSpec(), CardSize, lightPalette()));
}

void CoverGeneratorTest::testLightAndDarkDiffer()
{
    const QImage light = CoverGenerator::generate(paperSpec(), CardSize, lightPalette());
    const QImage dark = CoverGenerator::generate(paperSpec(), CardSize, darkPalette());
    QVERIFY(light != dark);
}

void CoverGeneratorTest::testUntitledSpecStillRenders()
{
    // The pipeline keeps placeholders for untitled entries, but the
    // generator itself must not crash on one
    const CoverGenerator::CoverSpec untitled {QString(), QString(), QString(), QStringLiteral("Aviation")};
    const QImage card = CoverGenerator::generate(untitled, CardSize, lightPalette());
    QCOMPARE(card.size(), CardSize);
}

void CoverGeneratorTest::renderSampleGallery()
{
    // Visual proof, not an assertion: run with PAPERLIBRARY_COVER_SAMPLE_DIR set
    // to write the sample shelf in light and dark for human judgment
    const QString dir = qEnvironmentVariable("PAPERLIBRARY_COVER_SAMPLE_DIR");
    if (dir.isEmpty()) {
        QSKIP("Set PAPERLIBRARY_COVER_SAMPLE_DIR to write sample covers");
    }
    QVERIFY(QDir().mkpath(dir));

    const struct {
        const char *name;
        CoverGenerator::CoverSpec spec;
    } samples[] = {
        {"bayesian", {QStringLiteral("Bayesian Descriptions Are Not Mechanisms"), QStringLiteral("Ian Todd"), QStringLiteral("2026 · Manuscript"), QStringLiteral("Neuroscience")}},
        {"integrity", {QStringLiteral("Integrity Measures in High-dimensional Phase Space"), QStringLiteral("Ian Todd"), QStringLiteral("2026"), QStringLiteral("Peer Review")}},
        {"corrosion", {QStringLiteral("A World Model for Corrosion — Reviewer's Briefing"), QStringLiteral("Ian Todd"), QStringLiteral("2026"), QStringLiteral("Peer Review")}},
        {"atpl", {QStringLiteral("ATPL (Aeroplane) Examination Information Book"), QString(), QString(), QStringLiteral("Aviation")}},
        {"longword", {QStringLiteral("An Uncommonly Long Mountaineering Treatise on Neuropsychopharmacology"), QStringLiteral("A. Verbose, B. Prolix"), QStringLiteral("1897 · Annals of Typography"), QStringLiteral("Stress")}},
        {"longtitle",
         {QStringLiteral("An Uncommonly Long and Meandering Treatise Upon the Statistical Mechanics of Overflowing Text Boxes, Considered as a Problem in Typography"),
          QStringLiteral("A. Verbose, B. Prolix, C. Loquacious, D. Garrulous"),
          QStringLiteral("1897 · Annals of Typography"),
          QStringLiteral("Stress")}},
        {"noauthors", {QStringLiteral("Notes Without Named Authors"), QString(), QStringLiteral("2026 · Working Paper"), QStringLiteral("Manuscript")}},
        // A Books-shelf card as FIX 2 builds it: OPF byline and year, no tag
        {"booksopf", {QStringLiteral("The Body Keeps the Score"), QStringLiteral("Bessel van der Kolk"), QStringLiteral("2014"), QString()}},
        // Bare: title only — everything else missing
        {"baretitle", {QStringLiteral("Meditations"), QString(), QString(), QString()}},
        // Short and tagged: mostly empty field, spine/gradient/keyline carry it
        {"spinefield", {QStringLiteral("Airframe"), QStringLiteral("M. Crichton"), QStringLiteral("1996"), QStringLiteral("Aviation")}},
    };

    for (const auto &sample : samples) {
        const QImage light = CoverGenerator::generate(sample.spec, QSize(400, 500), lightPalette());
        const QImage dark = CoverGenerator::generate(sample.spec, QSize(400, 500), darkPalette());
        QVERIFY(light.save(dir + QStringLiteral("/%1-light.png").arg(QLatin1String(sample.name))));
        QVERIFY(dark.save(dir + QStringLiteral("/%1-dark.png").arg(QLatin1String(sample.name))));

        // Judgment at shelf scale: the exact card the tile pipeline makes
        // (2x the tile's cover box), downscaled like the delegate does
        const QImage tile2x = CoverGenerator::generate(sample.spec, QSize(272, 340), lightPalette());
        QVERIFY(tile2x.scaled(136, 170, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(dir + QStringLiteral("/%1-tilesize.png").arg(QLatin1String(sample.name))));
    }
}

QTEST_MAIN(CoverGeneratorTest)
#include "covergeneratortest.moc"
