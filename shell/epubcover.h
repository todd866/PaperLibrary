/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_EPUBCOVER_H
#define PAPERLIBRARY_EPUBCOVER_H

#include <QImage>
#include <QString>

/**
 * Pulls the real cover image and the Dublin Core metadata straight out
 * of an EPUB, so the library never has to hand EPUBs to QuickLook —
 * which hangs forever on them on this machine (see the dev notes'
 * qlmanage watchdog saga). Both shapes of EPUB are read: the usual zip
 * archive, and the unpacked *directory* bundles Apple Books keeps.
 */
namespace EpubCover
{
/**
 * What the OPF package's Dublin Core block says about the book.
 * Display-only, opaque strings; every field may be empty.
 */
struct Metadata {
    QString title;       /**< dc:title, promoted from description when the package title is too sparse */
    QString creators;    /**< dc:creator values joined with ", " */
    QString year;        /**< first plausible 4-digit year in dc:date */
    QString description; /**< dc:description as plain text, HTML stripped */
};

/**
 * The declared cover of @p epubPath: the OPF manifest's EPUB 3
 * cover-image item, else the EPUB 2 meta name="cover" item, else the
 * largest image in the archive under 5 MB. A null image when the file
 * is unreadable, not an EPUB, or contains no decodable image (DRM'd
 * bundles keep their manifest readable but their images encrypted).
 */
QImage extract(const QString &epubPath);

/** The Dublin Core metadata of @p epubPath; all-empty when unreadable. */
Metadata metadata(const QString &epubPath);
}

#endif
