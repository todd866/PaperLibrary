/*
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2003 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2003-2007 Albert Astals Cid <aacid@kde.org>
    SPDX-FileCopyrightText: 2004 Andy Goossens <andygoossens@telenet.be>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "shell.h"

#include "macosscrolldebug.h"
#include "paperlibrary_main.h"
#include "paperlibraryaboutdata.h"
#include "epubwebreader.h"
#include "shellutils.h"
#include <KAboutData>
#include <KCrash>
#include <KIconTheme>
#include <KLocalizedString>
#include <KMessageBox>
#include <KWindowSystem>
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileOpenEvent>
#include <QObject>
#include <QStringList>
#include <QTextStream>
#include <QWheelEvent>
#include <QtGlobal>

#include <cstdio>
#include <memory>

#define HAVE_STYLE_MANAGER __has_include(<KStyleManager>)
#if HAVE_STYLE_MANAGER
#include <KStyleManager>
#endif

/**
 * Event handler for macOS file opening via QFileOpenEvent.
 * Should not do anything on Windows/Linux as QFileOpenEvent is never fired there.
 */

class FileOpenEventHandler : public QObject
{
    Q_OBJECT
protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::FileOpen) {
            auto *foe = static_cast<QFileOpenEvent *>(event);
            // Find existing Shell window
            const auto widgets = QApplication::topLevelWidgets();
            for (QWidget *widget : widgets) {
                if (Shell *shell = qobject_cast<Shell *>(widget)) {
                    QString serializedOptions = ShellUtils::serializeOptions(false, false, false, false, false, QString(), QString(), QString());
                    shell->openDocument(foe->url(), serializedOptions);
                    shell->raise();
                    shell->activateWindow();
                    return true;
                }
            }
            return false;
        }
        return QObject::eventFilter(obj, event);
    }
};

const char *scrollPhaseName(Qt::ScrollPhase phase)
{
    switch (phase) {
    case Qt::NoScrollPhase:
        return "NoScrollPhase";
    case Qt::ScrollBegin:
        return "ScrollBegin";
    case Qt::ScrollUpdate:
        return "ScrollUpdate";
    case Qt::ScrollEnd:
        return "ScrollEnd";
    case Qt::ScrollMomentum:
        return "ScrollMomentum";
    }
    return "UnknownScrollPhase";
}

class ScrollDebugEventFilter : public QObject
{
protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::Wheel) {
            auto *wheelEvent = static_cast<QWheelEvent *>(event);
            const char *const receiverClass = (obj && obj->metaObject()) ? obj->metaObject()->className() : "(null)";
            std::fprintf(stderr,
                         "PAPERLIBRARY_SCROLL_DEBUG Qt wheel phase=%s(%d) receiver=%s\n",
                         scrollPhaseName(wheelEvent->phase()),
                         static_cast<int>(wheelEvent->phase()),
                         receiverClass);
            std::fflush(stderr);
        }

        return QObject::eventFilter(obj, event);
    }
};

int main(int argc, char **argv)
{
    EpubWebReader::registerUrlScheme();
    EpubWebReader::configureBuildTreeRuntime();

    /**
     * trigger initialisation of proper icon theme
     */
#if KICONTHEMES_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    KIconTheme::initTheme();
#endif

    QCoreApplication::setAttribute(Qt::AA_CompressTabletEvents);

    QApplication app(argc, argv);

    std::unique_ptr<ScrollDebugEventFilter> scrollDebugEventFilter;
    if (qEnvironmentVariableIsSet("PAPERLIBRARY_SCROLL_DEBUG")) {
#ifdef Q_OS_MACOS
        MacScrollDebug::installLocalMonitorIfEnabled();
#endif
        scrollDebugEventFilter = std::make_unique<ScrollDebugEventFilter>();
        app.installEventFilter(scrollDebugEventFilter.get());
        std::fprintf(stderr, "PAPERLIBRARY_SCROLL_DEBUG Qt wheel event filter installed\n");
        std::fflush(stderr);
    }

    /**
     * Install event filter to handle macOS file opening.
     * This enables double-click and "Open With" on macOS.
     */
    FileOpenEventHandler eventHandler;
    app.installEventFilter(&eventHandler);
    KLocalizedString::setApplicationDomain("paperlibrary");

    /**
     * On macOS, keep Qt's native "macos" widget style instead of forcing Breeze,
     * so the application looks and feels like a native macOS app.
     */
#ifndef Q_OS_MACOS
#if HAVE_STYLE_MANAGER
    /**
     * trigger initialisation of proper application style
     */
    KStyleManager::initStyle();
#else
    /**
     * For Windows and macOS: use Breeze if available
     * Of all tested styles that works the best for us
     */
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    QApplication::setStyle(QStringLiteral("breeze"));
#endif
#endif
#endif

    KAboutData aboutData = paperLibraryAboutData();
    KAboutData::setApplicationData(aboutData);
    // Visible identity (menu bar app name, About, caption suffixes) is
    // PaperLibrary is the application identity and config/XMLGui component.
    QGuiApplication::setApplicationDisplayName(aboutData.displayName());
    // set icon for shells which do not use desktop file metadata
    QIcon appIcon(QStringLiteral(":/shell/icons/paperlibrary.svg"));
    if (appIcon.isNull()) {
        appIcon = QIcon::fromTheme(QStringLiteral("paperlibrary"));
    }
    QApplication::setWindowIcon(appIcon);

    KCrash::initialize();

    QCommandLineParser parser;
    // The KDE4 version accepted flags such as -unique with a single dash -> preserve compatibility
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    aboutData.setupCommandLine(&parser);

    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("p") << QStringLiteral("page"), i18n("Page of the document to be shown"), QStringLiteral("number")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("presentation"), i18n("Start the document in presentation mode")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("print"), i18n("Start with print dialog")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("print-and-exit"), i18n("Start with print dialog and exit after printing")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("unique"), i18n("\"Unique instance\" control")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("noraise"), i18n("Not raise window")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("find"), i18n("Find a string on the text"), QStringLiteral("string")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("editor-cmd"), i18n("Sets the external editor command"), QStringLiteral("string")));
    parser.addPositionalArgument(QStringLiteral("urls"), i18n("Documents to open. Specify '-' to read from stdin."));

    parser.process(app);
    aboutData.processCommandLine(&parser);

    // see if we are starting with session management
    if (app.isSessionRestored()) {
        kRestoreMainWindows<Shell>();
    } else {
        // no session.. just start up normally
        QStringList paths;
        for (int i = 0; i < parser.positionalArguments().count(); ++i) {
            paths << parser.positionalArguments().at(i);
        }
        PaperLibrary::Status status = PaperLibrary::main(paths, ShellUtils::serializeOptions(parser));
        switch (status) {
        case PaperLibrary::Error:
            return -1;
        case PaperLibrary::AttachedOtherProcess:
            return 0;
        case PaperLibrary::Success:
            // Do nothing
            break;
        }
    }

    return app.exec();
}
#include "main.moc"
/* kate: replace-tabs on; indent-width 4; */
