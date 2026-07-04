/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "coverheuristic.h"

#include <QColor>
#include <QImage>

// The measurement samples a grid of at most MaxSamples² pixels straight from
// the image — no smoothing, which would blur black-text-on-white into the
// very mid-grays the ink test is trying to tell apart from paper.
static constexpr int MaxSamples = 64;

// "Near-white paper": bright and colorless. Yellowish or tinted stock still
// counts as paper as long as it stays this close to white.
static constexpr double PaperMinLuminance = 0.85;
static constexpr double PaperMaxSaturation = 0.20;

// Verdict thresholds. A saturated page (cover art, illustrations) keeps its
// render on color alone; a colorless page keeps it only when it is mostly
// ink (grayscale photographs, scans). Paper text pages measure a few
// percent ink at most and fall through to the typographic cover.
static constexpr double ColorfulnessKeepsRender = 0.10;
static constexpr double InkCoverageKeepsRender = 0.35;

CoverHeuristic::Verdict CoverHeuristic::analyze(const QImage &pageOne, Score *scoreOut)
{
    Score score;
    if (pageOne.isNull() || pageOne.width() < 1 || pageOne.height() < 1) {
        if (scoreOut) {
            *scoreOut = score;
        }
        return GenerateTypographic;
    }

    const QImage image = pageOne.convertToFormat(QImage::Format_ARGB32);
    const int strideX = qMax(1, image.width() / MaxSamples);
    const int strideY = qMax(1, image.height() / MaxSamples);

    double saturationSum = 0.0;
    double luminanceSum = 0.0;
    double luminanceSquaredSum = 0.0;
    qint64 inked = 0;
    qint64 sampled = 0;

    for (int y = 0; y < image.height(); y += strideY) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x += strideX) {
            const QRgb rgb = line[x];
            if (qAlpha(rgb) < 128) {
                continue; // transparent margins say nothing about the page
            }
            ++sampled;

            const double saturation = QColor(rgb).hsvSaturationF();
            const double luminance = (0.2126 * qRed(rgb) + 0.7152 * qGreen(rgb) + 0.0722 * qBlue(rgb)) / 255.0;
            saturationSum += saturation;
            luminanceSum += luminance;
            luminanceSquaredSum += luminance * luminance;
            if (luminance < PaperMinLuminance || saturation > PaperMaxSaturation) {
                ++inked;
            }
        }
    }

    if (sampled > 0) {
        const double meanLuminance = luminanceSum / sampled;
        score.colorfulness = saturationSum / sampled;
        score.inkCoverage = double(inked) / sampled;
        score.variance = qMax(0.0, luminanceSquaredSum / sampled - meanLuminance * meanLuminance);
    }
    if (scoreOut) {
        *scoreOut = score;
    }

    if (sampled == 0) {
        return GenerateTypographic; // fully transparent: nothing to show
    }
    if (score.colorfulness >= ColorfulnessKeepsRender) {
        return KeepRender;
    }
    if (score.inkCoverage >= InkCoverageKeepsRender) {
        return KeepRender;
    }
    return GenerateTypographic;
}
