/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "covergenerator.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPen>
#include <QTextLayout>
#include <QTextOption>

#include <limits>

// The curated 10-hue palette: muted, mid-dark accents that near-white type
// reads well against. Order matters — it is baked into the tag hash.
static const QColor AccentPalette[] = {
    QColor(0x25, 0x6D, 0x65), // deep teal
    QColor(0x71, 0x38, 0x3E), // oxblood
    QColor(0x4C, 0x5F, 0x7C), // slate blue
    QColor(0x5C, 0x6B, 0x3C), // moss
    QColor(0x9A, 0x7B, 0x2E), // ochre
    QColor(0x5B, 0x3A, 0x5E), // aubergine
    QColor(0x1F, 0x56, 0x73), // petrol
    QColor(0x9C, 0x5B, 0x41), // clay
    QColor(0x2E, 0x59, 0x43), // pine
    QColor(0x4A, 0x3B, 0x4F), // charcoal-plum
};
static constexpr int AccentCount = 10;

// Warm near-white, the single type color on every card
static const QColor Ink(0xF5, 0xF2, 0xEA);

static QColor whiteAlpha(double alpha)
{
    QColor color(Qt::white);
    color.setAlphaF(alpha);
    return color;
}

static QColor inkAlpha(double alpha)
{
    QColor color = Ink;
    color.setAlphaF(alpha);
    return color;
}

QColor CoverGenerator::accentColor(const QString &seed, bool darkMode)
{
    // FNV-1a over the normalized seed: stable across sessions and builds,
    // unlike qHash, whose per-process seed would reshuffle the shelves
    const QByteArray key = seed.trimmed().toLower().toUtf8();
    quint32 hash = 2166136261u;
    for (const char c : key) {
        hash = (hash ^ static_cast<uchar>(c)) * 16777619u;
    }

    QColor accent = AccentPalette[hash % AccentCount];
    if (darkMode) {
        // Deepen, never invert: same hue family in a dark shelf
        float h, s, l, a;
        accent.getHslF(&h, &s, &l, &a);
        accent = QColor::fromHslF(h, s, l * 0.85f, a);
    }
    return accent;
}

/**
 * Word-boundary wrapping only — never inside a word. A word wider than
 * @p width lands alone on its own overflowing line (flagged tooWide);
 * text left over past @p maxLines is flagged truncated. Callers shrink
 * the font until neither flag is set, or elide at the minimum size.
 */
struct WrappedText {
    QStringList lines;
    bool tooWide = false;
    bool truncated = false;
};

static WrappedText wrapAtWords(const QString &text, const QFont &font, qreal width, int maxLines)
{
    QTextLayout layout(text, font);
    QTextOption wrapOption;
    wrapOption.setWrapMode(QTextOption::WordWrap);
    layout.setTextOption(wrapOption);

    WrappedText wrapped;
    layout.beginLayout();
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            break;
        }
        if (wrapped.lines.size() == maxLines) {
            wrapped.truncated = true;
            break;
        }
        line.setLineWidth(width);
        wrapped.tooWide = wrapped.tooWide || line.naturalTextWidth() > width + 0.5;
        wrapped.lines.append(text.mid(line.textStart(), line.textLength()).trimmed());
    }
    layout.endLayout();
    return wrapped;
}

// An orphan: a lone short word on the last line ("…and the|Bomb").
// A substantial single word ("…Robert|Oppenheimer") may close a title.
static constexpr qsizetype OrphanMaxChars = 6;

static bool isOrphanWord(const QString &word)
{
    return word.size() < OrphanMaxChars;
}

static bool hasOrphanLastLine(const QStringList &lines)
{
    if (lines.size() < 2) {
        return false;
    }
    const QString &last = lines.constLast();
    return !last.contains(QLatin1Char(' ')) && isOrphanWord(last);
}

/**
 * Rebreaks @p words into at most @p lineCount lines of at most @p width,
 * choosing the break points that minimize raggedness — squared leftover
 * space on every line but the last — and penalizing a lone short word
 * stranded on the last line, greedy wrapping's orphan ("…and the|Bomb").
 * Returns an empty list when no space-only breaking fits, and the caller
 * keeps the greedy lines (QTextLayout also breaks at hyphens, so its
 * line count can be unreachable on spaces alone).
 */
static QStringList balanceLines(const QStringList &words, const QFontMetricsF &metrics, qreal width, int lineCount)
{
    const int wordCount = words.size();

    // Width of words[from..to] joined by single spaces, measured as one
    // shaped run — per-word advances don't add up to the real thing
    QList<QList<qreal>> joinedWidth(wordCount);
    for (int i = 0; i < wordCount; ++i) {
        joinedWidth[i].resize(wordCount);
        QString run = words.at(i);
        joinedWidth[i][i] = metrics.horizontalAdvance(run);
        for (int j = i + 1; j < wordCount; ++j) {
            run += QLatin1Char(' ') + words.at(j);
            joinedWidth[i][j] = metrics.horizontalAdvance(run);
        }
    }
    const auto lineWidth = [&](int from, int to) { return joinedWidth.at(from).at(to); };

    constexpr qreal Unreachable = std::numeric_limits<qreal>::max();
    const qreal orphanPenalty = width * width * 8; // dominates any raggedness
    const auto lastLineCost = [&](int from) {
        if (lineWidth(from, wordCount - 1) > width + 0.5) {
            return Unreachable;
        }
        // A short lone word may not close the title; a substantial one may
        return (from == wordCount - 1 && isOrphanWord(words.at(from))) ? orphanPenalty : 0.0;
    };

    // cost[i][k]: cheapest typesetting of words[i..] in at most k lines;
    // breakAfter[i][k] records the chosen line end, -1 for "final line"
    QList<QList<qreal>> cost(wordCount, QList<qreal>(lineCount + 1, Unreachable));
    QList<QList<int>> breakAfter(wordCount, QList<int>(lineCount + 1, -1));
    for (int k = 1; k <= lineCount; ++k) {
        for (int i = wordCount - 1; i >= 0; --i) {
            cost[i][k] = lastLineCost(i);
            for (int j = i; k > 1 && j < wordCount - 1; ++j) {
                const qreal w = lineWidth(i, j);
                if (w > width + 0.5) {
                    break;
                }
                const qreal rest = cost.at(j + 1).at(k - 1);
                if (rest == Unreachable) {
                    continue;
                }
                const qreal slack = width - w;
                if (slack * slack + rest < cost.at(i).at(k)) {
                    cost[i][k] = slack * slack + rest;
                    breakAfter[i][k] = j;
                }
            }
        }
    }
    if (cost.at(0).at(lineCount) == Unreachable) {
        return QStringList();
    }

    QStringList lines;
    for (int i = 0, k = lineCount; i < wordCount;) {
        const int j = breakAfter.at(i).at(k);
        if (j < 0) {
            lines.append(QStringList(words.mid(i)).join(QLatin1Char(' ')));
            break;
        }
        lines.append(QStringList(words.mid(i, j - i + 1)).join(QLatin1Char(' ')));
        i = j + 1;
        --k;
    }
    return lines;
}

/**
 * Ends @p line in an ellipsis at a word boundary: whole words are
 * dropped from the end until the ellipsis fits @p width. Only a line
 * that is down to a single over-wide word is cut inside the word — the
 * one permitted case, and still under an ellipsis.
 */
static QString elideLineAtWordBoundary(QString line, const QFontMetricsF &metrics, qreal width)
{
    static const QString ellipsis = QStringLiteral("…");
    while (metrics.horizontalAdvance(line + ellipsis) > width) {
        const int lastSpace = line.lastIndexOf(QLatin1Char(' '));
        if (lastSpace <= 0) {
            return metrics.elidedText(line, Qt::ElideRight, width);
        }
        line.truncate(lastSpace);
        line = line.trimmed();
    }
    return line + ellipsis;
}

// ---- Card design metrics ------------------------------------------------
// The card reads as a tiny Penguin paperback: a darker spine strip down
// the left edge, a gently graded field, a keyline frame that registers at
// tile size, and generous content margins. generate() paints exactly what
// these helpers measure, and titleLines() exposes the same measurement to
// tests: every metric has one definition.

static constexpr qreal SpineWidthFraction = 0.045;   // of card width
static constexpr qreal SpineDarkening = 0.77;        // ~23% darker than the field
static constexpr qreal FieldTopLightness = 1.06;     // gentle vertical grade:
static constexpr qreal FieldBottomLightness = 0.94;  // lighter top, darker foot
static constexpr qreal KeylineInsetFraction = 0.055; // of card width, from the field edges
static constexpr qreal PadFraction = 0.115;          // content margins, of card width

/** @p color with its HSL lightness scaled by @p factor, same hue. */
static QColor scaledLightness(const QColor &color, qreal factor)
{
    float h, s, l, a;
    color.getHslF(&h, &s, &l, &a);
    return QColor::fromHslF(h, s, qBound(0.0f, l * float(factor), 1.0f), a);
}

static qreal spineWidthFor(const QSize &px)
{
    return px.width() * SpineWidthFraction;
}

/** The content margin: against the card's right edge and vertically. */
static qreal padFor(const QSize &px)
{
    return px.width() * PadFraction;
}

/** Where content starts on the left: the margin is measured off the spine. */
static qreal contentLeftFor(const QSize &px)
{
    return spineWidthFor(px) + padFor(px);
}

static qreal contentWidthFor(const QSize &px)
{
    return px.width() - contentLeftFor(px) - padFor(px);
}

static QFont chipFontFor(const QFont &base, const QSize &px)
{
    QFont font = base;
    // Sized to stay legible at tile rendering, subordinate to the title
    font.setPixelSize(qMax(1, qRound(px.height() * 0.053)));
    font.setCapitalization(QFont::SmallCaps);
    font.setLetterSpacing(QFont::PercentageSpacing, 106);
    font.setWeight(QFont::DemiBold);
    return font;
}

static QFont footFontFor(const QFont &base, const QSize &px)
{
    QFont font = base;
    font.setPixelSize(qMax(1, qRound(px.height() * 0.034)));
    return font;
}

static QFont authorFontFor(const QFont &base, const QSize &px)
{
    QFont font = base;
    font.setPixelSize(qMax(1, qRound(px.height() * 0.040)));
    return font;
}

static qreal chipPadXFor(const QSize &px)
{
    return px.height() * 0.022;
}

static qreal chipPadYFor(const QSize &px)
{
    return px.height() * 0.010;
}

/** Where the title block starts: under the tag chip when there is one. */
static qreal titleTopFor(const CoverGenerator::CoverSpec &spec, const QSize &px, const QFont &baseFont)
{
    const qreal pad = padFor(px);
    if (spec.tag.isEmpty()) {
        return pad + px.height() * 0.02;
    }
    const QFontMetricsF chipMetrics(chipFontFor(baseFont, px));
    return pad + chipMetrics.height() + 2 * chipPadYFor(px) + px.height() * 0.055;
}

/** Where the title+authors block must end: above the foot when there is one. */
static qreal bottomLimitFor(const CoverGenerator::CoverSpec &spec, const QSize &px, const QFont &baseFont)
{
    const qreal pad = padFor(px);
    if (spec.yearJournal.isEmpty()) {
        return px.height() - pad;
    }
    return px.height() - pad - QFontMetricsF(footFontFor(baseFont, px)).height() - px.height() * 0.030;
}

/** Height reserved under the title for the authors line (0 when none). */
static qreal authorsBlockFor(const CoverGenerator::CoverSpec &spec, const QSize &px, const QFont &baseFont)
{
    return spec.authors.isEmpty() ? 0.0 : px.height() * 0.035 + QFontMetricsF(authorFontFor(baseFont, px)).height();
}

/**
 * The title as the artwork: start large and shrink until at most six
 * whole-word lines fit the content width and the space above the
 * authors/foot, never smaller than 6% of the card. @p fontOut receives
 * the size that fit.
 */
static QStringList fitTitleLines(const CoverGenerator::CoverSpec &spec, const QSize &px, const QFont &baseFont, QFont *fontOut)
{
    const qreal contentWidth = contentWidthFor(px);
    const qreal cursorY = titleTopFor(spec, px, baseFont);
    const qreal bottomLimit = bottomLimitFor(spec, px, baseFont);
    const qreal authorsBlock = authorsBlockFor(spec, px, baseFont);

    const QStringList words = spec.title.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);

    QFont titleFont = baseFont;
    titleFont.setWeight(QFont::DemiBold);
    qreal titleSize = px.height() * 0.13;
    const qreal minTitleSize = px.height() * 0.06;
    WrappedText wrapped;
    QStringList balanced;
    for (;;) {
        titleFont.setPixelSize(qMax(1, qRound(titleSize)));
        const QFontMetricsF metrics(titleFont);
        wrapped = wrapAtWords(spec.title, titleFont, contentWidth, 6);
        const qreal lineHeight = titleFont.pixelSize() * 1.05; // tight leading
        bool fits = !wrapped.tooWide && !wrapped.truncated && cursorY + wrapped.lines.size() * lineHeight + authorsBlock <= bottomLimit;

        // The size fits; now the breaks must be *set*, not merely wrapped:
        // rebalance them so line lengths even out — and when even the best
        // breaks at this size strand a lone short word on the last line,
        // step the size once more so a two-word close becomes reachable
        balanced.clear();
        if (fits && wrapped.lines.size() > 1) {
            balanced = balanceLines(words, metrics, contentWidth, wrapped.lines.size());
            fits = balanced.isEmpty() || !hasOrphanLastLine(balanced);
        }
        if (fits || titleSize <= minTitleSize) {
            break;
        }
        titleSize *= 0.94;
    }

    QStringList titleLines = balanced.isEmpty() ? wrapped.lines : balanced;
    const QFontMetricsF titleMetrics(titleFont);

    // At the minimum size a monster title may still overflow the card:
    // keep the whole lines that have room and end the last kept one in an
    // ellipsis, placed at a word boundary
    const qreal lineHeight = titleFont.pixelSize() * 1.05;
    const int roomFor = qMax(1, int((bottomLimit - authorsBlock - cursorY) / lineHeight));
    if (roomFor < titleLines.size() || wrapped.truncated) {
        titleLines = titleLines.mid(0, qMin<qsizetype>(roomFor, titleLines.size()));
        titleLines.last() = elideLineAtWordBoundary(titleLines.last(), titleMetrics, contentWidth);
    }

    // A single word wider than the card even now is the one case where
    // the word itself is cut — always under an ellipsis
    for (QString &line : titleLines) {
        if (titleMetrics.horizontalAdvance(line) > contentWidth) {
            line = titleMetrics.elidedText(line, Qt::ElideRight, contentWidth);
        }
    }

    if (fontOut) {
        *fontOut = titleFont;
    }
    return titleLines;
}

QStringList CoverGenerator::titleLines(const CoverSpec &spec, const QSize &px)
{
    if (px.isEmpty()) {
        return QStringList();
    }
    return fitTitleLines(spec, px, QFont(), nullptr);
}

QImage CoverGenerator::generate(const CoverSpec &spec, const QSize &px, const QPalette &palette)
{
    if (px.isEmpty()) {
        return QImage();
    }

    const bool darkMode = palette.color(QPalette::Window).lightness() < 128;
    const QColor accent = accentColor(spec.tag.isEmpty() ? spec.title : spec.tag, darkMode);

    QImage card(px, QImage::Format_ARGB32_Premultiplied);

    QPainter painter(&card);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const qreal width = px.width();
    const qreal height = px.height();
    const qreal pad = padFor(px);
    const qreal contentLeft = contentLeftFor(px);
    const qreal contentWidth = contentWidthFor(px);
    const QFont baseFont = painter.font();

    // The field: a gentle vertical grade, lighter top to darker foot —
    // just enough curvature that the card stops reading as a flat swatch
    QLinearGradient field(0, 0, 0, height);
    field.setColorAt(0.0, scaledLightness(accent, FieldTopLightness));
    field.setColorAt(1.0, scaledLightness(accent, FieldBottomLightness));
    painter.fillRect(QRectF(0, 0, width, height), field);

    // The spine: a clearly darker strip down the whole left edge
    painter.fillRect(QRectF(0, 0, spineWidthFor(px), height), scaledLightness(accent, SpineDarkening));

    // Inner keyline framing the field (not the spine), strong enough to
    // register at tile size
    const qreal keylineInset = width * KeylineInsetFraction;
    painter.setPen(QPen(whiteAlpha(0.16), qMax<qreal>(1.0, width / 110.0)));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(spineWidthFor(px) + keylineInset, keylineInset, width - spineWidthFor(px) - 2 * keylineInset, height - 2 * keylineInset));

    // Tag chip top-left: small caps in a rounded rect of 20% white
    if (!spec.tag.isEmpty()) {
        const QFont chipFont = chipFontFor(baseFont, px);
        const QFontMetricsF chipMetrics(chipFont);

        const qreal chipPadX = chipPadXFor(px);
        const QString chipText = chipMetrics.elidedText(spec.tag, Qt::ElideRight, contentWidth - 2 * chipPadX);
        const QRectF chipRect(contentLeft, pad, chipMetrics.horizontalAdvance(chipText) + 2 * chipPadX, chipMetrics.height() + 2 * chipPadYFor(px));

        painter.setPen(Qt::NoPen);
        painter.setBrush(whiteAlpha(0.20));
        painter.drawRoundedRect(chipRect, chipRect.height() / 2, chipRect.height() / 2);
        painter.setFont(chipFont);
        painter.setPen(inkAlpha(0.95));
        painter.drawText(chipRect, Qt::AlignCenter, chipText);
    }

    // Year · journal sits at the very foot; it bounds the title from below
    if (!spec.yearJournal.isEmpty()) {
        const QFont footFont = footFontFor(baseFont, px);
        const QFontMetricsF footMetrics(footFont);
        painter.setFont(footFont);
        painter.setPen(inkAlpha(0.55));
        painter.drawText(QRectF(contentLeft, height - pad - footMetrics.height(), contentWidth, footMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter, footMetrics.elidedText(spec.yearJournal, Qt::ElideRight, contentWidth));
    }

    QFont titleFont;
    const QStringList titleLines = fitTitleLines(spec, px, baseFont, &titleFont);
    const qreal lineHeight = titleFont.pixelSize() * 1.05;
    qreal cursorY = titleTopFor(spec, px, baseFont);

    painter.setFont(titleFont);
    painter.setPen(Ink);
    const QFontMetricsF titleMetrics(titleFont);
    for (const QString &line : std::as_const(titleLines)) {
        painter.drawText(QPointF(contentLeft, cursorY + titleMetrics.ascent()), line);
        cursorY += lineHeight;
    }

    // Authors under the title at 60% alpha; skipped cleanly when empty.
    // The byline follows the title's wrap rule: when it must be cut, it
    // is cut at a word boundary under an ellipsis, never inside a name
    if (!spec.authors.isEmpty()) {
        const QFont authorFont = authorFontFor(baseFont, px);
        const QFontMetricsF authorMetrics(authorFont);
        const QString byline = authorMetrics.horizontalAdvance(spec.authors) > contentWidth ? elideLineAtWordBoundary(spec.authors, authorMetrics, contentWidth) : spec.authors;
        cursorY += height * 0.035;
        painter.setFont(authorFont);
        painter.setPen(inkAlpha(0.60));
        painter.drawText(QPointF(contentLeft, cursorY + authorMetrics.ascent()), byline);
    }

    painter.end();
    return card;
}
