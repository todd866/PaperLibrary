/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_EPUBIMPORTER_H
#define PAPERLIBRARY_EPUBIMPORTER_H

#include <QString>

class QUrl;

/**
 * Repackages Apple Books' *directory-bundle* EPUBs into standard zipped
 * .epub files the viewer can actually open.
 *
 * Apple Books stores every book as an unpacked folder (mimetype +
 * META-INF/container.xml + the OPF laid out on disk), and the EPUB engine
 * only opens zip archives — so on this machine every Books entry fails to
 * open. Rather than reference the bundle in place, PaperLibrary imports it:
 * a valid zipped copy is produced under the app's own data directory
 * (AppDataLocation/imported-books/), local and controlled.
 *
 * The produced zip is a spec-valid EPUB OCF: the @c mimetype entry is
 * written FIRST and STORED (uncompressed), everything else after. The import
 * is idempotent — a source whose bytes are unchanged reuses its existing
 * copy rather than re-zipping.
 *
 * Pure repackaging plus a one-way reading-position copy from Apple Books'
 * database: no book content is ever analysed or sent anywhere.
 */
namespace EpubImporter
{
enum class Status {
    Imported,             /**< success; @c importedPath is a valid zipped .epub */
    NotADirectoryBundle,  /**< not a local directory ending .epub; open it directly */
    DrmProtected,         /**< META-INF/encryption.xml present; refused, nothing written */
    NotDownloaded,        /**< iCloud placeholder / dataless bundle; ask the user to download */
    Failed,               /**< unreadable source, bad container, or the zip write failed */
};

struct Result {
    Status status = Status::Failed;
    QString importedPath;   /**< the local zipped .epub, set only when Imported */
    double progress = -1.0; /**< Apple Books reading fraction, copied on a FRESH import only; -1 otherwise */

    bool imported() const
    {
        return status == Status::Imported;
    }
};

/** Whether @p path (or @p url) names a local directory-bundle EPUB. */
bool isDirectoryBundle(const QString &path);
bool isDirectoryBundle(const QUrl &url);

/** AppDataLocation/imported-books — where imported copies live. */
QString importDir();

/** The stable local path a bundle at @p sourcePath imports to (per source path). */
QString importedPathFor(const QString &sourcePath);

/**
 * Import (repackage) the directory-bundle EPUB at @p bundlePath into
 * importDir(), reusing an existing valid copy when the source is unchanged.
 *
 * On a fresh (re)build the source's Apple Books reading fraction is read
 * one-way from the library database and returned in Result::progress so the
 * caller can land the imported document at that position; reuse never
 * recopies it (the reader's own saved position then wins). @p dbPath
 * overrides the database location for tests; empty resolves the default.
 */
Result import(const QString &bundlePath, const QString &dbPath = QString());
}

#endif
