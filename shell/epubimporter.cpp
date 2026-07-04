/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "epubimporter.h"

#include "applebooksprogress.h"

#include <KZip>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace
{
const QByteArray EpubMimeType = QByteArrayLiteral("application/epub+zip");

/** Whether @p name should be left out of the repackaged zip. */
bool isExcludedEntry(const QString &fileName)
{
    // Finder/iCloud droppings that are not part of the book
    return fileName == QLatin1String(".DS_Store") || fileName.endsWith(QLatin1String(".icloud"), Qt::CaseInsensitive);
}

/** A short filesystem-safe token derived from a book's file name. */
QString sanitizedBase(const QString &sourcePath)
{
    QString base = QFileInfo(sourcePath).completeBaseName();
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9._ -]+"));
    base.replace(unsafe, QStringLiteral("_"));
    base = base.simplified();
    base.replace(QLatin1Char(' '), QLatin1Char('_'));
    if (base.isEmpty()) {
        base = QStringLiteral("book");
    }
    return base.left(80);
}

/**
 * The regular files to repackage (excluding the top-level mimetype, which is
 * written separately and first), each as an (absolute, archive-relative) pair,
 * plus a cheap content signature (count, total size, newest mtime). Symlinks
 * are skipped so nothing in the zip can point outside the bundle.
 */
struct BundleScan {
    QList<QPair<QString, QString>> files; // absolute path, archive-relative path
    qint64 totalSize = 0;
    qint64 newestMTime = 0;
    QString mimetype; // the bundle's own mimetype bytes, or empty
};

BundleScan scanBundle(const QDir &bundle)
{
    BundleScan scan;
    const QString rootPath = bundle.absolutePath();
    QDirIterator it(rootPath, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isSymLink() || isExcludedEntry(info.fileName())) {
            continue;
        }
        const QString relative = bundle.relativeFilePath(info.absoluteFilePath());
        if (relative == QLatin1String("mimetype")) {
            QFile file(info.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly)) {
                scan.mimetype = QString::fromLatin1(file.readAll().trimmed());
            }
            continue; // written first, stored, by the caller
        }
        scan.files.append({info.absoluteFilePath(), relative});
        scan.totalSize += info.size();
        scan.newestMTime = qMax(scan.newestMTime, info.lastModified().toMSecsSinceEpoch());
    }
    // Deterministic order so the archive is reproducible for a given source
    std::sort(scan.files.begin(), scan.files.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    return scan;
}

QString signatureOf(const BundleScan &scan)
{
    return QStringLiteral("%1:%2:%3").arg(scan.files.size()).arg(scan.totalSize).arg(scan.newestMTime);
}

/** Whether the bundle looks not-yet-downloaded (iCloud placeholder / dataless). */
bool looksNotDownloaded(const QDir &bundle)
{
    const QFileInfo container(bundle.filePath(QStringLiteral("META-INF/container.xml")));
    if (!container.isFile() || container.size() == 0) {
        return true; // the one file every EPUB must have isn't materialised
    }
    // Any iCloud sentinel anywhere means the bundle isn't fully local yet
    QDirIterator it(bundle.absolutePath(), {QStringLiteral("*.icloud")}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    return it.hasNext();
}

/** Whether @p path is a zip whose first-and-stored entry is the EPUB mimetype. */
bool isValidEpubZip(const QString &path)
{
    KZip zip(path);
    if (!zip.open(QIODevice::ReadOnly)) {
        return false;
    }
    const KArchiveEntry *entry = zip.directory()->entry(QStringLiteral("mimetype"));
    const bool ok = entry && entry->isFile();
    zip.close();
    return ok;
}

/** Read this bundle's Apple Books reading fraction, or -1 when unknown. */
double bundleReadingProgress(const QString &bundlePath, const QString &dbPath)
{
    const QString target = QFileInfo(bundlePath).canonicalFilePath();
    if (target.isEmpty()) {
        return -1.0;
    }
    bool ok = false;
    const QList<AppleBooksProgress::BookEntry> books = AppleBooksProgress::read(dbPath, &ok);
    if (!ok) {
        return -1.0;
    }
    for (const AppleBooksProgress::BookEntry &book : books) {
        if (QFileInfo(book.path).canonicalFilePath() == target) {
            return book.progress;
        }
    }
    return -1.0;
}

/**
 * Write a spec-valid EPUB zip at @p target from @p scan: mimetype first and
 * STORED, everything else deflated. Written to a sibling temp file and
 * atomically renamed so an interrupted run never leaves a half-built archive.
 */
bool buildZip(const QString &target, const BundleScan &scan)
{
    const QString tempPath = target + QStringLiteral(".part-%1").arg(QCoreApplication::applicationPid());
    QFile::remove(tempPath);
    {
        KZip zip(tempPath);
        if (!zip.open(QIODevice::WriteOnly)) {
            qWarning().noquote() << "EpubImporter: could not open temp zip for writing:" << tempPath;
            return false;
        }
        // No per-entry "extra field": KZip writes a KDE timestamp field by
        // default, which would push a non-zero extra-field length into the
        // mimetype's local header and break the OCF magic-byte layout readers
        // sniff (mimetype value at offset 38). It also keeps the archive
        // reproducible for a given source.
        zip.setExtraField(KZip::NoExtraField);
        // The mimetype entry must be first and uncompressed (OCF requirement).
        zip.setCompression(KZip::NoCompression);
        const QByteArray mimetype = scan.mimetype.isEmpty() ? EpubMimeType : scan.mimetype.toLatin1();
        if (!zip.writeFile(QStringLiteral("mimetype"), mimetype)) {
            qWarning().noquote() << "EpubImporter: could not write mimetype entry:" << tempPath;
            zip.close();
            QFile::remove(tempPath);
            return false;
        }
        // Everything else is deflated to keep the imported copy compact.
        zip.setCompression(KZip::DeflateCompression);
        for (const auto &entry : scan.files) {
            if (!zip.addLocalFile(entry.first, entry.second)) {
                qWarning().noquote() << "EpubImporter: could not add file to zip:" << entry.first << "as" << entry.second;
                zip.close();
                QFile::remove(tempPath);
                return false;
            }
        }
        if (!zip.close()) {
            qWarning().noquote() << "EpubImporter: could not close temp zip:" << tempPath;
            QFile::remove(tempPath);
            return false;
        }
    }
    QFile::remove(target); // rename won't clobber on some platforms
    if (!QFile::rename(tempPath, target)) {
        qWarning().noquote() << "EpubImporter: could not rename temp zip:" << tempPath << "to" << target;
        QFile::remove(tempPath);
        return false;
    }
    return true;
}
}

bool EpubImporter::isDirectoryBundle(const QString &path)
{
    const QFileInfo info(path);
    return info.isDir() && info.fileName().endsWith(QLatin1String(".epub"), Qt::CaseInsensitive);
}

bool EpubImporter::isDirectoryBundle(const QUrl &url)
{
    return url.isLocalFile() && isDirectoryBundle(url.toLocalFile());
}

QString EpubImporter::importDir()
{
    const QString overrideDir = qEnvironmentVariable("PAPERLIBRARY_IMPORT_DIR");
    if (!overrideDir.isEmpty()) {
        return overrideDir;
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/imported-books");
}

QString EpubImporter::importedPathFor(const QString &sourcePath)
{
    // Stable per source path: a short hash disambiguates identically-named
    // books in different folders while the name keeps the file recognisable.
    const QByteArray hash = QCryptographicHash::hash(QFileInfo(sourcePath).absoluteFilePath().toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
    return importDir() + QLatin1Char('/') + sanitizedBase(sourcePath) + QLatin1Char('-') + QString::fromLatin1(hash) + QStringLiteral(".epub");
}

EpubImporter::Result EpubImporter::import(const QString &bundlePath, const QString &dbPath)
{
    if (!isDirectoryBundle(bundlePath)) {
        return {Status::NotADirectoryBundle, QString(), -1.0};
    }
    const QDir bundle(bundlePath);

    // DRM: a present encryption.xml means the content is encrypted — refuse,
    // and (checked before download state) a DRM'd book is itself downloaded.
    if (QFileInfo(bundle.filePath(QStringLiteral("META-INF/encryption.xml"))).isFile()) {
        return {Status::DrmProtected, QString(), -1.0};
    }
    if (looksNotDownloaded(bundle)) {
        return {Status::NotDownloaded, QString(), -1.0};
    }

    const BundleScan scan = scanBundle(bundle);
    if (scan.files.isEmpty()) {
        return {Status::NotDownloaded, QString(), -1.0}; // container present but nothing to package
    }

    const QString target = importedPathFor(bundlePath);
    const QString sidecar = target + QStringLiteral(".sig");
    const QString signature = signatureOf(scan);

    // Idempotent: an unchanged source keeps its existing valid copy; reuse
    // never re-copies the reading position (the reader's own saved position
    // now owns it).
    if (QFileInfo::exists(target) && isValidEpubZip(target)) {
        QFile sig(sidecar);
        if (sig.open(QIODevice::ReadOnly) && QString::fromUtf8(sig.readAll()) == signature) {
            return {Status::Imported, target, -1.0};
        }
    }

    if (!QDir().mkpath(importDir())) {
        qWarning().noquote() << "EpubImporter: could not create import directory:" << importDir();
        return {Status::Failed, QString(), -1.0};
    }
    if (!buildZip(target, scan)) {
        return {Status::Failed, QString(), -1.0};
    }
    QFile sig(sidecar);
    if (sig.open(QIODevice::WriteOnly)) {
        sig.write(signature.toUtf8());
        sig.close();
    }

    // One-way copy of the Apple Books reading position onto this fresh import,
    // only when the caller provides the database path. A default global scan
    // here can block ordinary imports and tests; the library view already has
    // its own startup Apple Books progress sync.
    return {Status::Imported, target, dbPath.isEmpty() ? -1.0 : bundleReadingProgress(bundlePath, dbPath)};
}
