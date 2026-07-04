/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "epubcover.h"

#include <KZip>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QTextDocumentFragment>
#include <QUrl>
#include <QXmlStreamReader>

namespace
{
const qint64 FallbackImageSizeLimit = 5 * 1024 * 1024; // don't decode giant scans speculatively

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
        return entry && entry->isFile() ? static_cast<const KArchiveFile *>(entry)->data() : QByteArray();
    }

    QByteArray largestImage() const override
    {
        const KArchiveFile *best = largestIn(m_root);
        return best ? best->data() : QByteArray();
    }

private:
    /** The largest image file in the archive within the size limit, or null. */
    static const KArchiveFile *largestIn(const KArchiveDirectory *dir)
    {
        const KArchiveFile *best = nullptr;
        const QStringList names = dir->entries();
        for (const QString &name : names) {
            const KArchiveEntry *entry = dir->entry(name);
            if (entry->isDirectory()) {
                const KArchiveFile *candidate = largestIn(static_cast<const KArchiveDirectory *>(entry));
                if (candidate && (!best || candidate->size() > best->size())) {
                    best = candidate;
                }
            } else if (entry->isFile() && hasImageSuffix(name)) {
                const KArchiveFile *file = static_cast<const KArchiveFile *>(entry);
                if (file->size() <= FallbackImageSizeLimit && (!best || file->size() > best->size())) {
                    best = file;
                }
            }
        }
        return best;
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
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    QByteArray largestImage() const override
    {
        QString bestPath;
        qint64 bestSize = -1;
        QDirIterator it(m_root.path(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo info = it.fileInfo();
            if (hasImageSuffix(info.fileName()) && info.size() <= FallbackImageSizeLimit && info.size() > bestSize) {
                bestSize = info.size();
                bestPath = info.filePath();
            }
        }
        if (bestPath.isEmpty()) {
            return QByteArray();
        }
        QFile file(bestPath);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

private:
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
    // The declared cover: container.xml → OPF → manifest
    QString opfDir;
    const QByteArray opf = opfBytes(source, &opfDir);
    if (!opf.isEmpty()) {
        QString href = declaredCoverHref(opf);
        if (!href.isEmpty()) {
            href = QUrl::fromPercentEncoding(href.toUtf8()); // hrefs are URLs ("My%20Cover.png")
            const QString coverPath = (opfDir.isEmpty() || opfDir == QLatin1String(".")) ? href : opfDir + QLatin1Char('/') + href;
            const QImage cover = QImage::fromData(source.read(coverPath));
            if (!cover.isNull()) {
                return cover;
            }
        }
    }

    // No (decodable) declared cover: fall back to the largest bundled image
    return QImage::fromData(source.largestImage());
}

EpubCover::Metadata parseMetadata(const QByteArray &opf)
{
    EpubCover::Metadata metadata;
    QStringList creators;
    QString date;
    QString description;

    QXmlStreamReader xml(opf);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        if (xml.name() == QLatin1String("creator")) {
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
