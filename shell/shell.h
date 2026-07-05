/*
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2003 Benjamin Meyer <benjamin@csh.rit.edu>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2003 Luboš Luňák <l.lunak@kde.org>
    SPDX-FileCopyrightText: 2004 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2004 Albert Astals Cid <aacid@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_SHELL_H
#define PAPERLIBRARY_SHELL_H

#include "config-paperlibrary.h"
#include <QAction>
#include <QList>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPointer>
#include <KXmlGuiWindow>

class QWindow;

#if HAVE_DBUS
#include <QDBusAbstractAdaptor> // for Q_NOREPLY
#else
#define Q_NOREPLY
#endif

class ChromeTabWidget;
class EpubWebReader;
class LibraryAutoTagger;
class LibraryStore;
class LibraryView;
class PdfView;
class Sidebar;
class KRecentFilesAction;
class KToggleAction;

/**
 * This is the PaperLibrary application shell. It owns the library tab plus
 * shell-native PDF and EPUB readers.
 *
 * @short Application Shell
 * @author Wilco Greven <greven@kde.org>
 * @version 0.1
 */
class Shell : public KXmlGuiWindow
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.todd866.paperlibrary")

    friend class MainShellTest;
    friend class AnnotationToolBarTest;

public:
    /**
     * Constructor
     */
    explicit Shell(const QString &serializedOptions = QString());

    /**
     * Default Destructor
     */
    ~Shell() override;

    QSize sizeHint() const override;

    /** Returns whether the shell initialized successfully. */
    bool isValid() const;

    bool openDocument(const QUrl &url, const QString &serializedOptions);

    /**
     * Reopens the document tabs saved by the last shell that closed
     * (kcfg RestoreTabsOnStartup). Called for launches without a file
     * argument only: an explicitly opened file suppresses the restore.
     */
    void restoreSavedTabs();

public Q_SLOTS:
    Q_SCRIPTABLE Q_NOREPLY void tryRaise(const QString &startupId);
    Q_SCRIPTABLE bool openDocument(const QString &urlString, const QString &serializedOptions = QString());
    Q_SCRIPTABLE bool canOpenDocs(int numDocs, int desktop);

protected:
    /**
     * This method is called when it is time for the app to save its
     * properties for session management purposes.
     */
    void saveProperties(KConfigGroup &) override;

    /**
     * This method is called when this app is restored.  The KConfig
     * object points to the session management config file that was saved
     * with @ref saveProperties
     */
    void readProperties(const KConfigGroup &) override;

    void readSettings();
    void writeSettings();
    void setFullScreen(bool);

    using KXmlGuiWindow::setCaption;
    void setCaption(const QString &caption) override;

    bool queryClose() override;

    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *) override;
    bool event(QEvent *event) override;

private Q_SLOTS:
    void fileOpen();

    void slotUpdateFullScreen();
    void slotShowMenubar();

    void openUrl(const QUrl &url, const QString &serializedOptions = QString());
    void showOpenRecentMenu();
    void closeUrl();
    void print();
    void handleDroppedUrls(const QList<QUrl> &urls);

    // Tab event handlers
    void setActiveTab(int tab);
    void closeTab(int tab);
    void activateNextTab();
    void activatePrevTab();
    void undoCloseTab();
    void moveTabData(int from, int to);

    void openNewLibraryTab();

    void openFromLibrary(const QUrl &url, double booksProgress);

private:
    void saveRecents();
    void setupAccel();
    void setupActions();
    void openNewTab(const QUrl &url, const QString &serializedOptions);
    int findTabIndex(QObject *sender) const;
    int findTabIndex(const QUrl &url) const;
    void readRecentFilesSettings();
    LibraryView *createLibraryView(bool deferInitialRefresh = false);
    bool currentTabIsLibrary() const;
    bool shouldUseEpubWebReader(const QUrl &url) const;
    bool openEpubWebReaderInTab(int tab, const QUrl &url);
    void connectEpubWebReader(EpubWebReader *reader);
    void removeEpubSideContainer(EpubWebReader *reader);
    bool shouldUsePdfView(const QUrl &url) const;
    bool openPdfViewInTab(int tab, const QUrl &url);
    void connectPdfView(PdfView *reader);
    void removePdfSideContainer(PdfView *reader);
    void hideXmlGuiToolbars();
    void applyChromeSeparatorStyle();
    void setEpubFontActionsEnabled(bool enabled);
    void setEpubScrollModeActionsEnabled(bool enabled);
    void syncEpubScrollModeActions();
    void syncEpubHistoryActions();
    void setPdfReaderActionsEnabled(bool enabled);

    /**
     * macOS titlebar-tabs mode: adopt the native window when requested,
     * re-assert the no-native-toolbar titlebar on every update, refresh sane
     * titlebar metrics, and push the cached geometry to the tab strip —
     * collapsing the inset in fullscreen.
     */
    void updateTitlebarLayout(bool adoptNativeWindow = true);
    void scheduleTitlebarLayoutUpdate();
    void applyTitlebarSafeAreaOptOut();
    void applyTitlebarSafeAreaOptOut(QWidget *widget);
    void replaceTabPage(int index, QWidget *page, const QString &label, const QString &toolTip);
    void navigateLibraryTabToUrl(int tab, const QUrl &url, const QString &serializedOptions);
    void recordDocumentOpened(const QUrl &url);

    /**
     * Apple Books stores EPUBs as directory bundles the EPUB engine cannot
     * open. Repackage @p bundleUrl into a local zipped .epub (idempotent) and
     * open that instead, carrying the Apple Books reading position across on a
     * fresh import. DRM'd, not-yet-downloaded and failed imports surface a
     * clear message rather than the raw "could not open" error.
     */
    void openImportedBundle(const QUrl &bundleUrl, const QString &serializedOptions);

    /** Land the document at @p url on @p progress (0..1) once its pages load; a no-op for progress < 0. */
    void jumpToBooksProgressWhenReady(const QUrl &url, double progress);

    bool eventFilter(QObject *obj, QEvent *event) override;

    KRecentFilesAction *m_recent;
    LibraryStore *m_libraryStore = nullptr;
    LibraryAutoTagger *m_libraryAutoTagger = nullptr;
    QAction *m_printAction;
    QAction *m_closeAction;
    KToggleAction *m_fullScreenAction;
    KToggleAction *m_showMenuBarAction;
    bool m_menuBarWasShown, m_toolBarWasShown;
    bool m_unique;
    ChromeTabWidget *m_tabWidget;

    // macOS titlebar-tabs mode state (see updateTitlebarLayout)
    bool m_titlebarTabs = false;        // escape hatch: [General] TitlebarTabs
    QPointer<QWindow> m_titlebarHandle; // the window handle whose signals we wired (re-wire if Qt recreates it)
    bool m_titlebarLayoutUpdatePending = false;
    bool m_titlebarLayoutFollowupPending = false;
    int m_measuredStripHeight = 40;     // last good native titlebar height (points)
    int m_measuredInset = 78;           // last good traffic-light clearance (points)
    KToggleAction *m_openInTab;
    Sidebar *m_sidebar = nullptr;
    bool m_sidebarVisibleOnDocTabs = true;

    /**
     * One entry per tab strip position. A tab hosts either the shell-owned
     * WebEngine EPUB reader, the shell-owned QtPdf PDF reader, or the Library
     * (Chrome's new-tab page as a real tab).
     */
    struct TabState {
        explicit TabState(EpubWebReader *reader)
            : epubReader(reader)
            , closeEnabled(true)
        {
        }
        explicit TabState(PdfView *reader)
            : pdfReader(reader)
            , closeEnabled(true)
        {
        }
        explicit TabState(LibraryView *v)
            : libraryView(v)
        {
        }
        bool isLibrary() const
        {
            return epubReader == nullptr && pdfReader == nullptr;
        }
        bool isDocument() const
        {
            return epubReader != nullptr || pdfReader != nullptr;
        }
        EpubWebReader *epubReader = nullptr;
        PdfView *pdfReader = nullptr;
        LibraryView *libraryView = nullptr;
        bool closeEnabled = false;
    };
    QList<TabState> m_tabs;
    QList<QUrl> m_closedTabUrls;
    QAction *m_nextTabAction;
    QAction *m_prevTabAction;
    QAction *m_undoCloseTab;
    QAction *m_showSidebarAction = nullptr;
    QMetaObject::Connection m_showSidebarConnection;
    QAction *m_lockSidebarAction = nullptr;
    QAction *m_epubFontIncreaseAction = nullptr;
    QAction *m_epubFontDecreaseAction = nullptr;
    QAction *m_epubFontResetAction = nullptr;
    QAction *m_epubGlobalPaginatedAction = nullptr;
    QAction *m_epubGlobalContinuousAction = nullptr;
    QAction *m_epubBookUseGlobalAction = nullptr;
    QAction *m_epubBookPaginatedAction = nullptr;
    QAction *m_epubBookContinuousAction = nullptr;
    QAction *m_epubBackAction = nullptr;
    QAction *m_epubForwardAction = nullptr;
    QAction *m_scanAppleBooksAction = nullptr;
    QAction *m_autoTagLibraryAction = nullptr;
    QAction *m_readerMotionAction = nullptr;
    QAction *m_pdfShowSidebarAction = nullptr;
    QAction *m_pdfFindAction = nullptr;
    QAction *m_pdfFindNextAction = nullptr;
    QAction *m_pdfFindPreviousAction = nullptr;
    QAction *m_pdfCopyAction = nullptr;
    QAction *m_pdfSelectAllAction = nullptr;
    QAction *m_pdfZoomInAction = nullptr;
    QAction *m_pdfZoomOutAction = nullptr;
    QAction *m_pdfFitWidthAction = nullptr;

    bool m_isValid;
};

#endif

// vim:ts=2:sw=2:tw=78:et
