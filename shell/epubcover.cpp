/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "epubcover.h"

#include <KZip>

#include <QDir>
#include <QDirIterator>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QRegularExpression>
#include <QTextDocumentFragment>
#include <QUrl>
#include <QVector>
#include <QXmlStreamReader>

namespace
{
const qint64 FallbackImageSizeLimit = 5 * 1024 * 1024; // don't decode giant scans speculatively
const qint64 MetadataFileSizeLimit = 8 * 1024 * 1024;
const qint64 DecodedImagePixelLimit = 80LL * 1000 * 1000;
const qsizetype ArchiveEntryLimit = 20000;
const qint64 ArchiveTotalSizeLimit = 1024LL * 1024 * 1024;
const int ArchiveDepthLimit = 64;

bool hasImageSuffix(const QString &name)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    return suffix == QLatin1String("png") || suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg") || suffix == QLatin1String("gif") || suffix == QLatin1String("webp") || suffix == QLatin1String("bmp");
}

/** A safe archive-relative path, or empty: no absolute paths, no escapes. */
QString cleanRelativePath(const QString &path)
{
    const QString clean = QDir::cleanPath(path); // resolves any ../ in hrefs
    if (clean.isEmpty() || clean.startsWith(QLatin1String("../")) || QDir::isAbsolutePath(clean)) {
        return QString();
    }
    return clean;
}

/**
 * Reads file bytes out of either shape of EPUB — the usual zip archive
 * or Apple Books' unpacked directory bundle — so the container/OPF
 * resolution above it is written exactly once.
 */
class EpubSource
{
public:
    virtual ~EpubSource() = default;
    /** Bytes of the file at archive-relative @p path; null when absent. */
    virtual QByteArray read(const QString &path) const = 0;
    /** Entire source satisfies the bounded traversal contract. */
    virtual bool withinBudget() const = 0;
    /** Bytes of the largest image within the size limit; null when none. */
    virtual QByteArray largestImage() const = 0;
};

class ZipSource : public EpubSource
{
public:
    explicit ZipSource(const KArchiveDirectory *root)
        : m_root(root)
    {
    }

    QByteArray read(const QString &path) const override
    {
        const QString clean = cleanRelativePath(path);
        if (clean.isEmpty()) {
            return QByteArray();
        }
        const KArchiveEntry *entry = m_root->entry(clean);
        if (!entry || !entry->isFile()) {
            return QByteArray();
        }
        const auto *file = static_cast<const KArchiveFile *>(entry);
        return file->size() >= 0 && file->size() <= MetadataFileSizeLimit ? file->data() : QByteArray();
    }

    bool withinBudget() const override
    {
        return scan(nullptr);
    }

    QByteArray largestImage() const override
    {
        const KArchiveFile *best = nullptr;
        if (!scan(&best)) {
            return QByteArray();
        }
        return best ? best->data() : QByteArray();
    }

private:
    /** Iterative, bounded archive traversal; optionally returns the best image. */
    bool scan(const KArchiveFile **bestOut) const
    {
        const KArchiveFile *best = nullptr;
        qsizetype entriesSeen = 0;
        qint64 totalBytes = 0;
        QVector<QPair<const KArchiveDirectory *, int>> pending;
        pending.append({m_root, 0});
        while (!pending.isEmpty()) {
            const auto current = pending.takeLast();
            const QStringList names = current.first->entries();
            for (const QString &name : names) {
                if (++entriesSeen > ArchiveEntryLimit) {
                    return false;
                }
                const KArchiveEntry *entry = current.first->entry(name);
                if (!entry) {
                    return false;
                }
                if (entry->isDirectory()) {
                    if (current.second >= ArchiveDepthLimit) {
                        return false;
                    }
                    pending.append({static_cast<const KArchiveDirectory *>(entry), current.second + 1});
                    continue;
                }
                if (!entry->isFile()) {
                    continue;
                }
                const auto *file = static_cast<const KArchiveFile *>(entry);
                const qint64 size = file->size();
                if (size < 0 || size > ArchiveTotalSizeLimit - totalBytes) {
                    return false;
                }
                totalBytes += size;
                if (hasImageSuffix(name) && size <= FallbackImageSizeLimit
                    && (!best || size > best->size())) {
                    best = file;
                }
            }
        }
        if (bestOut) {
            *bestOut = best;
        }
        return true;
    }

    const KArchiveDirectory *m_root;
};

class DirSource : public EpubSource
{
public:
    explicit DirSource(const QString &root)
        : m_root(root)
    {
    }

    QByteArray read(const QString &path) const override
    {
        const QString clean = cleanRelativePath(path);
        if (clean.isEmpty()) {
            return QByteArray();
        }
        // Symlinks could still point out of the bundle: only follow paths
        // that canonically stay inside it
        const QString canonicalRoot = QFileInfo(m_root.path()).canonicalFilePath();
        const QString canonical = QFileInfo(m_root.filePath(clean)).canonicalFilePath();
        if (canonicalRoot.isEmpty() || canonical.isEmpty() || !canonical.startsWith(canonicalRoot + QLatin1Char('/'))) {
            return QByteArray();
        }
        QFile file(canonical);
        return file.size() >= 0 && file.size() <= MetadataFileSizeLimit && file.open(QIODevice::ReadOnly)
            ? file.readAll()
            : QByteArray();
    }

    bool withinBudget() const override
    {
        return scan(nullptr);
    }

    QByteArray largestImage() const override
    {
        QString bestPath;
        if (!scan(&bestPath)) {
            return QByteArray();
        }
        if (bestPath.isEmpty()) {
            return QByteArray();
        }
        QFile file(bestPath);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

private:
    bool scan(QString *bestOut) const
    {
        QString bestPath;
        qint64 bestSize = -1;
        qint64 totalBytes = 0;
        qsizetype entriesSeen = 0;
        QVector<QPair<QString, int>> pending;
        pending.append({m_root.path(), 0});
        while (!pending.isEmpty()) {
            const auto current = pending.takeLast();
            QDirIterator entries(
                current.first,
                QDir::AllEntries | QDir::NoDotAndDotDot,
                QDirIterator::NoIteratorFlags);
            while (entries.hasNext()) {
                entries.next();
                const QFileInfo info = entries.fileInfo();
                if (++entriesSeen > ArchiveEntryLimit || info.isSymLink()) {
                    return false;
                }
                if (info.isDir()) {
                    if (current.second >= ArchiveDepthLimit) {
                        return false;
                    }
                    pending.append({info.absoluteFilePath(), current.second + 1});
                    continue;
                }
                if (!info.isFile()) {
                    continue;
                }
                const qint64 size = info.size();
                if (size < 0 || size > ArchiveTotalSizeLimit - totalBytes) {
                    return false;
                }
                totalBytes += size;
                if (hasImageSuffix(info.fileName()) && size <= FallbackImageSizeLimit
                    && size > bestSize) {
                    bestSize = size;
                    bestPath = info.absoluteFilePath();
                }
            }
        }
        if (bestOut) {
            *bestOut = bestPath;
        }
        return true;
    }

    QDir m_root;
};

/** The OPF package document's path, per META-INF/container.xml. */
QString opfPath(const QByteArray &container)
{
    QXmlStreamReader xml(container);
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("rootfile")) {
            return xml.attributes().value(QLatin1String("full-path")).toString();
        }
    }
    return QString();
}

/** The OPF package bytes, with the OPF's directory in @p opfDirOut. */
QByteArray opfBytes(const EpubSource &source, QString *opfDirOut)
{
    if (!source.withinBudget()) {
        return QByteArray();
    }
    const QString packagePath = opfPath(source.read(QStringLiteral("META-INF/container.xml")));
    if (packagePath.isEmpty()) {
        return QByteArray();
    }
    if (opfDirOut) {
        *opfDirOut = QFileInfo(packagePath).path();
    }
    return source.read(packagePath);
}

/** The cover image's href declared in the OPF manifest, or empty. */
QString declaredCoverHref(const QByteArray &opf)
{
    QHash<QString, QString> hrefById;
    QString epub3Href;
    QString epub2CoverId;

    QXmlStreamReader xml(opf);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        const QXmlStreamAttributes attributes = xml.attributes();
        if (xml.name() == QLatin1String("item")) {
            const QString href = attributes.value(QLatin1String("href")).toString();
            hrefById.insert(attributes.value(QLatin1String("id")).toString(), href);
            // EPUB 3: <item properties="cover-image" …/> (space-separated list)
            const auto properties = attributes.value(QLatin1String("properties")).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (properties.contains(QLatin1String("cover-image"))) {
                epub3Href = href;
            }
        } else if (xml.name() == QLatin1String("meta") && attributes.value(QLatin1String("name")) == QLatin1String("cover")) {
            // EPUB 2: <meta name="cover" content="manifest-id"/>
            epub2CoverId = attributes.value(QLatin1String("content")).toString();
        }
    }
    return !epub3Href.isEmpty() ? epub3Href : hrefById.value(epub2CoverId);
}

QImage extractFrom(const EpubSource &source)
{
    const auto decodeImage = [](const QByteArray &bytes) {
        QBuffer buffer;
        buffer.setData(bytes);
        if (!buffer.open(QIODevice::ReadOnly)) {
            return QImage();
        }
        QImageReader reader(&buffer);
        reader.setDecideFormatFromContent(true);
        const QSize size = reader.size();
        if (!size.isValid() || static_cast<qint64>(size.width()) * size.height() > DecodedImagePixelLimit) {
            return QImage();
        }
        return reader.read();
    };
    // The declared cover: container.xml → OPF → manifest
    QString opfDir;
    const QByteArray opf = opfBytes(source, &opfDir);
    if (!opf.isEmpty()) {
        QString href = declaredCoverHref(opf);
        if (!href.isEmpty()) {
            href = QUrl::fromPercentEncoding(href.toUtf8()); // hrefs are URLs ("My%20Cover.png")
            const QString coverPath = (opfDir.isEmpty() || opfDir == QLatin1String(".")) ? href : opfDir + QLatin1Char('/') + href;
            const QImage cover = decodeImage(source.read(coverPath));
            if (!cover.isNull()) {
                return cover;
            }
        }
    }

    // No (decodable) declared cover: fall back to the largest bundled image
    return decodeImage(source.largestImage());
}

bool sparsePackageTitle(const QString &title)
{
    const QString simplified = title.simplified();
    if (simplified.isEmpty()) {
        return true;
    }
    if (simplified.size() <= 8) {
        return true;
    }
    const QStringList words = simplified.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    return words.size() <= 2 && !simplified.contains(QLatin1Char(':')) && !simplified.contains(QLatin1Char('&'));
}

QString promotedTitleFromDescription(const QString &packageTitle, const QString &descriptionHtml)
{
    const QString base = packageTitle.simplified();
    if (!sparsePackageTitle(base) || descriptionHtml.isEmpty()) {
        return QString();
    }

    static const QRegularExpression emphasizedTitlePattern(QStringLiteral("<\\s*(?:i|em)\\b[^>]*>([^<]{3,180})<\\s*/\\s*(?:i|em)\\s*>"),
                                                           QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator matches = emphasizedTitlePattern.globalMatch(descriptionHtml);
    while (matches.hasNext()) {
        const QString candidate = QTextDocumentFragment::fromHtml(matches.next().captured(1)).toPlainText().simplified();
        if (candidate.size() <= base.size() + 4) {
            continue;
        }
        if (base.isEmpty()) {
            return candidate;
        }
        const QString candidateLower = candidate.toCaseFolded();
        const QString baseLower = base.toCaseFolded();
        if (candidateLower.startsWith(baseLower + QLatin1Char(':')) || candidateLower.startsWith(baseLower + QStringLiteral(" - "))
            || candidateLower.startsWith(baseLower + QStringLiteral(" – "))) {
            return candidate;
        }
    }
    return QString();
}

EpubCover::Metadata parseMetadata(const QByteArray &opf)
{
    EpubCover::Metadata metadata;
    QString title;
    QStringList creators;
    QString date;
    QString description;

    QXmlStreamReader xml(opf);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        if (xml.name() == QLatin1String("title") && title.isEmpty()) {
            title = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
        } else if (xml.name() == QLatin1String("creator")) {
            const QString creator = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
            if (!creator.isEmpty()) {
                creators.append(creator);
            }
        } else if (xml.name() == QLatin1String("date") && date.isEmpty()) {
            date = xml.readElementText(QXmlStreamReader::IncludeChildElements);
        } else if (xml.name() == QLatin1String("description") && description.isEmpty()) {
            description = xml.readElementText(QXmlStreamReader::IncludeChildElements);
        }
    }

    metadata.title = title.simplified();
    const QString promotedTitle = promotedTitleFromDescription(metadata.title, description);
    if (!promotedTitle.isEmpty()) {
        metadata.title = promotedTitle;
    }
    metadata.creators = creators.join(QStringLiteral(", "));

    // dc:date shapes vary ("1987", "1987-03-01", ISO timestamps): the
    // first plausible 4-digit year in it is the display year
    static const QRegularExpression yearPattern(QStringLiteral("\\b([12][0-9]{3})\\b"));
    const QRegularExpressionMatch yearMatch = yearPattern.match(date);
    if (yearMatch.hasMatch()) {
        metadata.year = yearMatch.captured(1);
    }

    // dc:description is nominally plain text but ships as HTML often
    // enough that stores render it; strip to plain text either way
    if (!description.isEmpty()) {
        metadata.description = QTextDocumentFragment::fromHtml(description).toPlainText().simplified();
    }
    return metadata;
}
}

QImage EpubCover::extract(const QString &epubPath)
{
    if (QFileInfo(epubPath).isDir()) {
        return extractFrom(DirSource(epubPath));
    }
    KZip zip(epubPath);
    if (!zip.open(QIODevice::ReadOnly)) {
        return QImage();
    }
    return extractFrom(ZipSource(zip.directory()));
}

EpubCover::Metadata EpubCover::metadata(const QString &epubPath)
{
    if (QFileInfo(epubPath).isDir()) {
        return parseMetadata(opfBytes(DirSource(epubPath), nullptr));
    }
    KZip zip(epubPath);
    if (!zip.open(QIODevice::ReadOnly)) {
        return Metadata();
    }
    return parseMetadata(opfBytes(ZipSource(zip.directory()), nullptr));
}
