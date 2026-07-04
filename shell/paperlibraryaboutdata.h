/*
    SPDX-FileCopyrightText: 2007 Pino Toscano <pino@kde.org>
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARYABOUTDATA_H
#define PAPERLIBRARYABOUTDATA_H

#include <KAboutData>
#include <KLocalizedString>

/**
 * About data for the PaperLibrary shell.
 *
 * The application identity is PaperLibrary. This is a day-one project; local
 * settings can be rebuilt instead of migrating old Okular config files.
 *
 * GPL attribution: the upstream Okular authors and copyright lines are
 * retained beneath the fork credit.
 */
inline KAboutData paperLibraryAboutData()
{
    KAboutData about(QStringLiteral("paperlibrary"),
                     i18n("PaperLibrary"),
                     QStringLiteral("1.0.0-dev"),
                     i18n("The PaperLibrary reader — a Chrome-style PDF/EPUB front-end for a local, AI-curated document library"),
                     KAboutLicense::GPL,
                     i18n("(C) 2026 Ian Todd\n"
                          "Based on Okular:\n"
                          "(C) 2002 Wilco Greven, Christophe Devriese\n"
                          "(C) 2004-2005 Enrico Ros\n"
                          "(C) 2005 Piotr Szymanski\n"
                          "(C) 2004-2017 Albert Astals Cid\n"
                          "(C) 2006-2009 Pino Toscano"),
                     QString(),
                     QStringLiteral("https://github.com/todd866/PaperLibrary"));

    about.addAuthor(i18n("Ian Todd"), i18n("PaperLibrary fork"), QStringLiteral("todd.ian@gmail.com"));

    // Upstream Okular authors and credits, retained for GPL attribution.
    about.addAuthor(i18n("Pino Toscano"), i18n("Former maintainer"), QStringLiteral("pino@kde.org"));
    about.addAuthor(i18n("Tobias Koenig"), i18n("Lots of framework work, FictionBook backend and former ODT backend"), QStringLiteral("tokoe@kde.org"));
    about.addAuthor(i18n("Albert Astals Cid"), i18n("Developer"), QStringLiteral("aacid@kde.org"));
    about.addAuthor(i18n("Piotr Szymanski"), i18n("Created Okular from KPDF codebase"), QStringLiteral("djurban@pld-dc.org"));
    about.addAuthor(i18n("Enrico Ros"), i18n("KPDF developer"), QStringLiteral("eros.kde@email.it"));
    about.addCredit(i18n("Eugene Trounev"), i18n("Annotations artwork"), QStringLiteral("eugene.trounev@gmail.com"));
    about.addCredit(i18n("Jiri Baum - NICTA"), i18n("Table selection tool"), QStringLiteral("jiri@baum.com.au"));
    about.addCredit(i18n("Fabio D'Urso"), i18n("Annotation improvements"), QStringLiteral("fabiodurso@hotmail.it"));

    return about;
}

#endif
