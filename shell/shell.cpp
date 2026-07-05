/*
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2002 Chris Cheney <ccheney@cheney.cx>
    SPDX-FileCopyrightText: 2003 Benjamin Meyer <benjamin@csh.rit.edu>
    SPDX-FileCopyrightText: 2003-2004 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2003-2004 Albert Astals Cid <aacid@kde.org>
    SPDX-FileCopyrightText: 2003 Luboš Luňák <l.lunak@kde.org>
    SPDX-FileCopyrightText: 2003 Malcolm Hunter <malcolm.hunter@gmx.co.uk>
    SPDX-FileCopyrightText: 2004 Dominique Devriese <devriese@kde.org>
    SPDX-FileCopyrightText: 2004 Dirk Mueller <mueller@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "shell.h"

// qt/kde includes
#include <KActionCollection>
#include <KConfigGroup>
#include <KIO/Global>
#include <KLocalizedString>
#include <KMessageBox>
#include <KRecentFilesAction>
#include <KSharedConfig>
#include <KStandardAction>
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS) && !defined(Q_OS_HAIKU)
#include <KStartupInfo>
#include <KWindowInfo>
#endif
#include <KToggleFullScreenAction>
#include <KToolBar>
#include <KUrlMimeData>
#include <KWindowSystem>
#include <QAbstractScrollArea>
#include <QActionGroup>
#include <QApplication>
#include <QByteArray>
#if HAVE_DBUS
#include <QDBusConnection>
#endif // HAVE_DBUS
#include <QDebug>
#include <QDockWidget>
#include <QDragMoveEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QMenuBar>
#include <QMimeData>
#include <QObject>
#include <QPainter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QProxyStyle>
#include <QScreen>
#include <QStackedWidget>
#include <QStyleOption>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>

#include <cstdio>

#include <kio_version.h>
#include <kxmlgui_version.h>

// local includes
#include "chrometabbar.h"
#include "chrometabwidget.h"
#include "chrometoolbar.h"
#include <QWindow>
#include <QEasingCurve>
#if defined(Q_OS_MACOS)
#include "macostitlebar.h"
#endif
#include "epubwebreader.h"
#include "epubimporter.h"
#include "libraryautotagger.h"
#include "librarystore.h"
#include "libraryview.h"
#include "pdfview.h"
#include "shellutils.h"

static const char *shouldShowMenuBarComingFromFullScreen = "shouldShowMenuBarComingFromFullScreen";
static const char *shouldShowToolBarComingFromFullScreen = "shouldShowToolBarComingFromFullScreen";

static const char *const SESSION_URL_KEY = "Urls";
static const char *const SESSION_TAB_KEY = "ActiveTab";

static const char *const RESTORE_URLS_KEY = "OpenDocumentUrls";
static const char *const RESTORE_TAB_KEY = "ActiveDocumentTab";

static constexpr char SIDEBAR_LOCKED_KEY[] = "LockSidebar";
static constexpr char SIDEBAR_VISIBLE_KEY[] = "ShowSidebar";
static constexpr char QT_PDF_READER_KEY[] = "UseQtPdfReaderForPdf";
static constexpr char APPLE_BOOKS_SCAN_KEY[] = "ScanAppleBooksOnStartup";
static constexpr char LIBRARY_AUTO_TAG_KEY[] = "LibraryAutoTag";
static constexpr char READER_MOTION_KEY[] = "ReaderMotion";

namespace
{
struct PaperLibraryTabDebugTrace {
    bool active = false;
    int sequence = 0;
    qint64 commandStartMs = 0;
    qint64 lastMs = 0;
};

bool paperLibraryTabDebugEnabled()
{
    static const bool enabled = qEnvironmentVariableIsSet("PAPERLIBRARY_TAB_DEBUG");
    return enabled;
}

QElapsedTimer &paperLibraryTabDebugTimer()
{
    static QElapsedTimer timer;
    if (!timer.isValid()) {
        timer.start();
    }
    return timer;
}

PaperLibraryTabDebugTrace &paperLibraryTabDebugTrace()
{
    static PaperLibraryTabDebugTrace trace;
    return trace;
}

void paperLibraryTabDebugPrint(int sequence, qint64 nowMs, qint64 sinceCommandMs, qint64 deltaMs, const QString &phase)
{
    const QByteArray phaseUtf8 = phase.toUtf8();
    std::fprintf(stderr,
                 "PAPERLIBRARY_TAB_DEBUG seq=%d t=%lldms since_cmd=%lldms delta=%lldms %s\n",
                 sequence,
                 static_cast<long long>(nowMs),
                 static_cast<long long>(sinceCommandMs),
                 static_cast<long long>(deltaMs),
                 phaseUtf8.constData());
    std::fflush(stderr);
}

int paperLibraryTabDebugBegin(const QString &phase)
{
    if (!paperLibraryTabDebugEnabled()) {
        return 0;
    }

    const qint64 nowMs = paperLibraryTabDebugTimer().elapsed();
    PaperLibraryTabDebugTrace &trace = paperLibraryTabDebugTrace();
    trace.active = true;
    ++trace.sequence;
    trace.commandStartMs = nowMs;
    trace.lastMs = nowMs;
    paperLibraryTabDebugPrint(trace.sequence, nowMs, 0, 0, phase);
    return trace.sequence;
}

void paperLibraryTabDebugMark(const QString &phase)
{
    if (!paperLibraryTabDebugEnabled()) {
        return;
    }

    const qint64 nowMs = paperLibraryTabDebugTimer().elapsed();
    PaperLibraryTabDebugTrace &trace = paperLibraryTabDebugTrace();
    if (!trace.active) {
        trace.lastMs = nowMs;
        paperLibraryTabDebugPrint(0, nowMs, 0, 0, phase);
        return;
    }

    paperLibraryTabDebugPrint(trace.sequence, nowMs, nowMs - trace.commandStartMs, nowMs - trace.lastMs, phase);
    trace.lastMs = nowMs;
}

void paperLibraryTabDebugEnd(int sequence)
{
    if (!paperLibraryTabDebugEnabled()) {
        return;
    }

    PaperLibraryTabDebugTrace &trace = paperLibraryTabDebugTrace();
    if (trace.active && trace.sequence == sequence) {
        trace.active = false;
    }
}
}

static inline QString DesktopEntryGroupKey()
{
    return QStringLiteral("Desktop Entry");
}
static inline QString RecentFilesGroupKey()
{
    return QStringLiteral("Recent Files");
}
static inline QString GeneralGroupKey()
{
    return QStringLiteral("General");
}

/**
 * Groups sidebar containers in a QDockWidget.
 *
 * This control groups sidebar content provided by each document tab, allowing
 * the user to dock it to the left and right sides of the window,
 * or detach it from the window altogether.
 */
class Sidebar : public QDockWidget
{
    Q_OBJECT

public:
    explicit Sidebar(QWidget *parent = nullptr)
        : QDockWidget(parent)
    {
        setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        setFeatures(defaultFeatures());

        // Chrome-flat: the dock never shows a title bar, locked or not
        m_dumbTitleWidget = new QWidget;
        setTitleBarWidget(m_dumbTitleWidget);

        m_stackedWidget = new QStackedWidget;
        m_stackedWidget->setAutoFillBackground(true);
        m_stackedWidget->setBackgroundRole(QPalette::Window);
        setWidget(m_stackedWidget);
        // It seems that without requesting a specific minimum size, Qt
        // somehow calculates a (0,-1) minimum size, and then Qt gets angry
        // that negative sizes is not possible.
        setMinimumSize(10, 10);
        m_widthAnimation = new QPropertyAnimation(this, "maximumWidth", this);
        m_widthAnimation->setDuration(180);
        m_widthAnimation->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_widthAnimation, &QPropertyAnimation::finished, this, [this]() {
            const bool hide = m_hideAfterAnimation;
            m_hideAfterAnimation = false;
            if (hide) {
                QDockWidget::setVisible(false);
            }
            setMinimumSize(10, 10);
            setMaximumWidth(QWIDGETSIZE_MAX);
        });
    }

    bool isLocked() const
    {
        return features().testFlag(NoDockWidgetFeatures);
    }

    void setLocked(bool locked)
    {
        setFeatures(locked ? NoDockWidgetFeatures : defaultFeatures());
    }

    int indexOf(QWidget *widget) const
    {
        return m_stackedWidget->indexOf(widget);
    }

    void addWidget(QWidget *widget)
    {
        m_stackedWidget->addWidget(widget);
    }

    void removeWidget(QWidget *widget)
    {
        m_stackedWidget->removeWidget(widget);
    }

    void setCurrentWidget(QWidget *widget)
    {
        m_stackedWidget->setCurrentWidget(widget);
    }

    void setAnimatedVisible(bool visible, bool animated)
    {
        if (!animated || isFloating()) {
            if (m_widthAnimation) {
                m_widthAnimation->stop();
            }
            m_hideAfterAnimation = false;
            setMinimumSize(10, 10);
            setMaximumWidth(QWIDGETSIZE_MAX);
            QDockWidget::setVisible(visible);
            return;
        }

        if (m_widthAnimation) {
            m_widthAnimation->stop();
        }
        const int targetWidth = qBound(220, qMax(width(), m_lastDockWidth), 360);
        if (visible) {
            m_hideAfterAnimation = false;
            m_lastDockWidth = targetWidth;
            setMinimumWidth(1);
            setMaximumWidth(1);
            QDockWidget::setVisible(true);
            if (m_widthAnimation) {
                m_widthAnimation->setStartValue(1);
                m_widthAnimation->setEndValue(targetWidth);
                m_widthAnimation->start();
            }
            return;
        }

        if (!isVisible()) {
            return;
        }
        m_lastDockWidth = qMax(width(), m_lastDockWidth);
        m_hideAfterAnimation = true;
        setMinimumWidth(1);
        if (m_widthAnimation) {
            m_widthAnimation->setStartValue(qMax(1, width()));
            m_widthAnimation->setEndValue(1);
            m_widthAnimation->start();
        } else {
            QDockWidget::setVisible(false);
        }
    }

    /**
     * Reserve @p height at the top of the dock so its content clears the
     * macOS traffic lights when the strip lives in the titlebar (the left
     * dock otherwise rises to y=0 under the buttons). The empty dock title
     * widget doubles as that spacer; 0 restores the flush layout.
     */
    void setTitlebarClearance(int height)
    {
        m_dumbTitleWidget->setFixedHeight(0);
        m_stackedWidget->setContentsMargins(0, qMax(0, height), 0, 0);
    }

private:
    static DockWidgetFeatures defaultFeatures()
    {
        DockWidgetFeatures dockFeatures = DockWidgetClosable | DockWidgetMovable;
        if (!KWindowSystem::isPlatformWayland()) { // TODO : Remove this check when QTBUG-87332 is fixed
            dockFeatures |= DockWidgetFloatable;
        }

        return dockFeatures;
    }

    QStackedWidget *m_stackedWidget = nullptr;
    QWidget *m_dumbTitleWidget = nullptr;
    QPropertyAnimation *m_widthAnimation = nullptr;
    int m_lastDockWidth = 260;
    bool m_hideAfterAnimation = false;
};

Shell::Shell(const QString &serializedOptions)
    : KXmlGuiWindow()
    , m_menuBarWasShown(true)
    , m_toolBarWasShown(true)
    , m_isValid(true)
{
    setObjectName(QStringLiteral("paperlibrary::Shell#"));
    setContextMenuPolicy(Qt::NoContextMenu);
    // otherwise .rc file won't be found by unit test
    setComponentName(QStringLiteral("paperlibrary"), QString());
    // set the shell's ui resource file
    setXMLFile(QStringLiteral("shell.rc"));
    m_showMenuBarAction = nullptr;

    {
        m_libraryStore = new LibraryStore(this);

        // Setup the central chrome: tab strip on top, the slim toolbar row
        // under it, tab pages below. A tab hosts either a shell-owned
        // document reader or a Library view; the strip is always visible — Chrome shows it
        // even with a single tab.
        m_tabWidget = new ChromeTabWidget(this);

        m_tabWidget->setAcceptDrops(true);
        m_tabWidget->tabBar()->installEventFilter(this);

        setCentralWidget(m_tabWidget);

        connect(m_tabWidget, &ChromeTabWidget::currentChanged, this, &Shell::setActiveTab);
        connect(m_tabWidget, &ChromeTabWidget::tabCloseRequested, this, &Shell::closeTab);
        connect(m_tabWidget->tabBar(), &QTabBar::tabMoved, this, &Shell::moveTabData);

        // Untitled library entries pick up metadata from the claude CLI in
        // the background; fresh tags show up right away when a library tab
        // is what's on screen (hidden ones refresh on show anyway)
        m_libraryAutoTagger = new LibraryAutoTagger(m_libraryStore, this);
        connect(m_libraryAutoTagger, &LibraryAutoTagger::tagged, this, [this]() {
            const int current = m_tabWidget->currentIndex();
            if (current >= 0 && current < m_tabs.size() && m_tabs.at(current).libraryView) {
                m_tabs.at(current).libraryView->refresh();
            }
        });

        m_sidebar = new Sidebar;
        m_sidebar->setObjectName(QStringLiteral("paperlibrary_sidebar"));
        m_sidebar->setContextMenuPolicy(Qt::ActionsContextMenu);
        m_sidebar->setWindowTitle(i18n("Sidebar"));
        connect(m_sidebar, &QDockWidget::visibilityChanged, this, [this](bool visible) {
            if (currentTabIsLibrary()) {
                // MainWindow tries hard to make its child dockwidgets shown,
                // but on a library tab we don't want to see the sidebar, so
                // try a bit more to actually hide it.
                m_sidebar->hide();
            } else if (isVisible()) {
                // Remember the user's choice for document tabs; window-level
                // hides (minimize, close) are not choices
                m_sidebarVisibleOnDocTabs = visible;
                if (m_showSidebarAction) {
                    m_showSidebarAction->setChecked(visible);
                }
            }
        });
        addDockWidget(Qt::LeftDockWidgetArea, m_sidebar);
        applyChromeSeparatorStyle();

        // then, setup our actions
        setupActions();
        m_tabWidget->chromeTabBar()->setNewTabAction(actionCollection()->action(QStringLiteral("new-tab")));
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &QObject::deleteLater);
        setupGUI(Keys | ToolBar | Save);

#if defined(Q_OS_MACOS)
        // Chrome-style tabs in the titlebar (the default; disable with
        // paperlibraryrc [General] TitlebarTabs=false to fall back below the
        // titlebar). When on, MacTitlebar manages the native window on show
        // and Qt must not also drive a unified toolbar; when off, keep the
        // classic macOS unified titlebar blend.
        m_titlebarTabs = chromeTitlebarTabsEnabled(KSharedConfig::openConfig()->group(GeneralGroupKey()));
        if (m_titlebarTabs) {
            // MacTitlebar gives the window a full-size content view, so its
            // NSView already spans the titlebar band. But Qt reports that band
            // as a top safe-area margin and, by default, folds it into the main
            // window's contents margins — which pushes the whole central area
            // (the tab strip included) down below the titlebar, the very bug we
            // are fixing. Opt the main window out of the safe-area inset so its
            // layout starts at the true window top (y=0) and the strip rises
            // into the titlebar beside the traffic lights. The left dock is kept
            // clear of the buttons separately, via setTitlebarClearance().
            applyTitlebarSafeAreaOptOut();
            connect(m_tabWidget, &ChromeTabWidget::tabCountChanged, this, [this](int) {
                scheduleTitlebarLayoutUpdate();
            });
        } else {
            setUnifiedTitleAndToolBarOnMac(true);
        }
#endif

        // NOTE : apply default sidebar width only after calling setupGUI(...)
        resizeDocks({m_sidebar}, {200}, Qt::Horizontal);

        // The strip always has at least one tab; launch lands on the
        // Library — Chrome's new-tab page — until a document navigates it
        LibraryView *const libraryView = createLibraryView();
        m_tabs.append(TabState(libraryView));
        m_tabWidget->addTab(libraryView, i18n("Library"));

        readSettings();

        if (m_libraryStore->isEmpty()) {
            // First run with the library: seed it from the recent files
            m_libraryStore->importUrls(m_recent->urls());
        }

        // Work through the backlog of untitled entries gradually, one
        // document at a time (the tagger skips everything else)
        const QList<LibraryStore::Entry> libraryEntries = m_libraryStore->entries();
        for (const LibraryStore::Entry &libraryEntry : libraryEntries) {
            if (libraryEntry.title.isEmpty()) {
                m_libraryAutoTagger->enqueue(libraryEntry.url);
            }
        }

        m_unique = ShellUtils::unique(serializedOptions);
#if HAVE_DBUS
        if (m_unique) {
            m_unique = QDBusConnection::sessionBus().registerService(QStringLiteral("io.github.todd866.paperlibrary"));
            if (!m_unique) {
                KMessageBox::information(this, i18n("There is already a unique PaperLibrary instance running. This instance won't be the unique one."));
            }
        } else {
            // TODO When porting to KF7 Remove
            // PID is not unique in containers and "-" in the name violates D-Bus naming conventions.
            // Was left for compatibility with 3rd-party scripts.
            QString serviceName = QStringLiteral("io.github.todd866.paperlibrary.") + QString::number(qApp->applicationPid());
            QDBusConnection::sessionBus().registerService(serviceName);

            QDBusConnection::sessionBus().registerService(ShellUtils::currentProcessDbusName());
        }
        if (ShellUtils::noRaise(serializedOptions)) {
            setAttribute(Qt::WA_ShowWithoutActivating);
        }

        QDBusConnection::sessionBus().registerObject(QStringLiteral("/paperlibraryshell"), this, QDBusConnection::ExportScriptableSlots);
#endif // HAVE_DBUS
    }

}

LibraryView *Shell::createLibraryView(bool deferInitialRefresh)
{
    LibraryView *const view = new LibraryView(m_libraryStore, this, deferInitialRefresh);
    connect(view, &LibraryView::openClicked, this, &Shell::fileOpen);
    connect(view, &LibraryView::itemActivated, this, &Shell::openFromLibrary);
    connect(view, &LibraryView::closeRequested, this, [this, view]() {
        // Escape on a library tab closes it (a no-op when it is the only tab)
        closeTab(findTabIndex(view));
    });
    view->installEventFilter(this); // accept dropped documents
    return view;
}

bool Shell::currentTabIsLibrary() const
{
    const int current = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    return current >= 0 && current < m_tabs.size() && m_tabs.at(current).isLibrary();
}

bool Shell::shouldUseEpubWebReader(const QUrl &url) const
{
    return EpubWebReader::canOpen(url);
}

bool Shell::shouldUsePdfView(const QUrl &url) const
{
    const KConfigGroup group = KSharedConfig::openConfig()->group(GeneralGroupKey());
    return group.readEntry(QT_PDF_READER_KEY, true) && PdfView::canOpen(url);
}

void Shell::connectEpubWebReader(EpubWebReader *reader)
{
    connect(reader, &EpubWebReader::titleChanged, this, [this, reader](const QString &title) {
        const int tab = findTabIndex(reader);
        if (tab >= 0 && !title.isEmpty()) {
            m_tabWidget->setTabText(tab, title);
        }
    });
    connect(reader, &EpubWebReader::renderProcessTerminated, this, [](const QString &message) {
        qWarning().noquote() << message;
    });
    connect(reader, &EpubWebReader::loadFinished, this, [this, reader](bool) {
        applyTitlebarSafeAreaOptOut(reader);
        scheduleTitlebarLayoutUpdate();
    });
    connect(reader, &EpubWebReader::scrollModeChanged, this, [this, reader](EpubWebReader::ScrollMode, bool) {
        const int tab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
        if (tab >= 0 && tab < m_tabs.size() && m_tabs[tab].epubReader == reader) {
            syncEpubScrollModeActions();
        }
    });
    connect(reader, &EpubWebReader::historyAvailabilityChanged, this, [this, reader](bool, bool) {
        const int tab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
        if (tab >= 0 && tab < m_tabs.size() && m_tabs[tab].epubReader == reader) {
            syncEpubHistoryActions();
        }
    });
    connect(reader, &EpubWebReader::outlineAvailableChanged, this, [this, reader](bool available) {
        const int tab = findTabIndex(reader);
        if (tab < 0 || tab != m_tabWidget->currentIndex()) {
            return;
        }
        if (m_pdfShowSidebarAction) {
            m_pdfShowSidebarAction->setEnabled(available);
        }
        if (!available) {
            m_sidebar->setAnimatedVisible(false, PdfView::readerMotionEnabled());
        } else if (m_sidebarVisibleOnDocTabs) {
            m_sidebar->setAnimatedVisible(true, PdfView::readerMotionEnabled());
        }
    });
}

void Shell::connectPdfView(PdfView *reader)
{
    connect(reader, &PdfView::titleChanged, this, [this, reader](const QString &title) {
        const int tab = findTabIndex(reader);
        if (tab >= 0 && !title.isEmpty()) {
            m_tabWidget->setTabText(tab, title);
        }
    });
    connect(reader, &PdfView::errorOccurred, this, [](const QString &message) {
        qWarning().noquote() << message;
    });
    connect(reader, &PdfView::loadFinished, this, [this, reader](bool) {
        applyTitlebarSafeAreaOptOut(reader);
        scheduleTitlebarLayoutUpdate();
    });
    connect(reader, &PdfView::outlineAvailableChanged, this, [this, reader](bool available) {
        const int tab = findTabIndex(reader);
        if (tab < 0 || tab != m_tabWidget->currentIndex()) {
            return;
        }
        if (m_pdfShowSidebarAction) {
            m_pdfShowSidebarAction->setEnabled(available);
        }
        if (!available) {
            m_sidebar->setAnimatedVisible(false, PdfView::readerMotionEnabled());
        } else if (m_sidebarVisibleOnDocTabs) {
            m_sidebar->setAnimatedVisible(true, PdfView::readerMotionEnabled());
        }
    });
}

bool Shell::openEpubWebReaderInTab(int tab, const QUrl &url)
{
    if (tab < 0 || tab >= m_tabs.size()) {
        return false;
    }

    EpubWebReader *const reader = new EpubWebReader(this);
    connectEpubWebReader(reader);
    if (!reader->open(url)) {
        reader->deleteLater();
        return false;
    }
    applyTitlebarSafeAreaOptOut(reader);

    m_tabs[tab] = TabState(reader);
    replaceTabPage(tab, reader, url.fileName(), url.fileName());
    const QMimeType epubMime = QMimeDatabase().mimeTypeForName(QStringLiteral("application/epub+zip"));
    m_tabWidget->setTabIcon(tab, QIcon::fromTheme(epubMime.iconName()));
    return true;
}

bool Shell::openPdfViewInTab(int tab, const QUrl &url)
{
    if (tab < 0 || tab >= m_tabs.size()) {
        return false;
    }

    PdfView *const reader = new PdfView(this);
    connectPdfView(reader);
    if (!reader->open(url)) {
        reader->deleteLater();
        return false;
    }
    applyTitlebarSafeAreaOptOut(reader);

    m_tabs[tab] = TabState(reader);
    replaceTabPage(tab, reader, url.fileName(), url.fileName());
    const QMimeType pdfMime = QMimeDatabase().mimeTypeForName(QStringLiteral("application/pdf"));
    m_tabWidget->setTabIcon(tab, QIcon::fromTheme(pdfMime.iconName()));
    return true;
}

void Shell::removePdfSideContainer(PdfView *reader)
{
    if (!reader || !reader->outlineWidget()) {
        return;
    }
    m_sidebar->removeWidget(reader->outlineWidget());
}

void Shell::removeEpubSideContainer(EpubWebReader *reader)
{
    if (!reader || !reader->outlineWidget()) {
        return;
    }
    m_sidebar->removeWidget(reader->outlineWidget());
}

/**
 * Draws the QMainWindow dock separator — the seam around the sidebar — as
 * a 1px hairline at 12% of the text color, read live from the palette.
 * Installed on the Shell window alone (children keep the application
 * style), so nothing else changes; a stylesheet would have pushed
 * QStyleSheetStyle onto every descendant widget.
 */
class ChromeSeparatorStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override
    {
        if (metric == PM_DockWidgetSeparatorExtent) {
            return 1;
        }
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const override
    {
        if (element == PE_IndicatorDockWidgetResizeHandle) {
            QColor hairline = option->palette.color(QPalette::WindowText);
            hairline.setAlphaF(0.12);
            painter->fillRect(option->rect, hairline);
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};

void Shell::applyChromeSeparatorStyle()
{
    // A default-constructed proxy follows the application style without
    // owning it; parented here so it dies with the window
    ChromeSeparatorStyle *const separatorStyle = new ChromeSeparatorStyle;
    separatorStyle->setParent(this);
    setStyle(separatorStyle);
}

void Shell::setEpubFontActionsEnabled(bool enabled)
{
    if (m_epubFontIncreaseAction) {
        m_epubFontIncreaseAction->setEnabled(enabled);
    }
    if (m_epubFontDecreaseAction) {
        m_epubFontDecreaseAction->setEnabled(enabled);
    }
    if (m_epubFontResetAction) {
        m_epubFontResetAction->setEnabled(enabled);
    }
}

void Shell::setEpubScrollModeActionsEnabled(bool enabled)
{
    const QList<QAction *> actions = {
        m_epubBookUseGlobalAction,
        m_epubBookPaginatedAction,
        m_epubBookContinuousAction,
    };
    for (QAction *action : actions) {
        if (action) {
            action->setEnabled(enabled);
        }
    }
}

void Shell::syncEpubScrollModeActions()
{
    const int tab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    EpubWebReader *const reader = (tab >= 0 && tab < m_tabs.size()) ? m_tabs[tab].epubReader : nullptr;
    setEpubScrollModeActionsEnabled(reader != nullptr);

    const EpubWebReader::ScrollMode globalMode = EpubWebReader::globalScrollMode();
    if (m_epubGlobalPaginatedAction) {
        m_epubGlobalPaginatedAction->setChecked(globalMode == EpubWebReader::ScrollMode::Paginated);
    }
    if (m_epubGlobalContinuousAction) {
        m_epubGlobalContinuousAction->setChecked(globalMode == EpubWebReader::ScrollMode::Continuous);
    }

    if (!reader) {
        if (m_epubBookUseGlobalAction) {
            m_epubBookUseGlobalAction->setChecked(true);
        }
        return;
    }

    if (!reader->hasBookScrollModeOverride()) {
        if (m_epubBookUseGlobalAction) {
            m_epubBookUseGlobalAction->setChecked(true);
        }
        return;
    }

    const EpubWebReader::ScrollMode mode = reader->scrollMode();
    if (m_epubBookPaginatedAction) {
        m_epubBookPaginatedAction->setChecked(mode == EpubWebReader::ScrollMode::Paginated);
    }
    if (m_epubBookContinuousAction) {
        m_epubBookContinuousAction->setChecked(mode == EpubWebReader::ScrollMode::Continuous);
    }
}

void Shell::syncEpubHistoryActions()
{
    const int tab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
    EpubWebReader *const reader = (tab >= 0 && tab < m_tabs.size()) ? m_tabs[tab].epubReader : nullptr;
    if (m_epubBackAction) {
        m_epubBackAction->setEnabled(reader && reader->canNavigateBackInBook());
    }
    if (m_epubForwardAction) {
        m_epubForwardAction->setEnabled(reader && reader->canNavigateForwardInBook());
    }
}

void Shell::setPdfReaderActionsEnabled(bool enabled)
{
    const QList<QAction *> actions = {
        m_pdfFindAction,
        m_pdfFindNextAction,
        m_pdfFindPreviousAction,
        m_pdfCopyAction,
        m_pdfSelectAllAction,
        m_pdfZoomInAction,
        m_pdfZoomOutAction,
        m_pdfFitWidthAction,
    };
    for (QAction *action : actions) {
        if (action) {
            action->setEnabled(enabled);
        }
    }
}

void Shell::hideXmlGuiToolbars()
{
    // The XMLGui-merged toolbars are not presented anymore — the shell's
    // ChromeToolbar row is the toolbar. Keep any standard XMLGui bars hidden
    // so the shell controls the full document chrome.
    const QList<KToolBar *> bars = toolBars();
    for (KToolBar *bar : bars) {
        bar->setVisible(false);
        bar->setMovable(false);
    }
}

void Shell::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape && window()->isFullScreen()) {
        setFullScreen(false);
    }
}

bool Shell::event(QEvent *event)
{
    const bool handled = KXmlGuiWindow::event(event);

#if defined(Q_OS_MACOS)
    if (m_titlebarTabs && event->type() == QEvent::SafeAreaMarginsChange) {
        scheduleTitlebarLayoutUpdate();
    }
#endif

    return handled;
}

bool Shell::eventFilter(QObject *obj, QEvent *event)
{
    QDragMoveEvent *dmEvent = dynamic_cast<QDragMoveEvent *>(event);
    if (dmEvent) {
        bool accept = dmEvent->mimeData()->hasUrls();
        event->setAccepted(accept);
        return accept;
    }

    QDropEvent *dEvent = dynamic_cast<QDropEvent *>(event);
    if (dEvent) {
        const QList<QUrl> list = KUrlMimeData::urlsFromMimeData(dEvent->mimeData());
        handleDroppedUrls(list);
        dEvent->setAccepted(true);
        return true;
    }

    // Handle middle button click events on the tab bar
    if (obj == m_tabWidget->tabBar() && event->type() == QEvent::MouseButtonRelease) {
        QMouseEvent *mEvent = static_cast<QMouseEvent *>(event);
        if (mEvent->button() == Qt::MiddleButton) {
            int tabIndex = m_tabWidget->tabBar()->tabAt(mEvent->pos());
            if (tabIndex != -1) {
                closeTab(tabIndex);
                return true;
            }
        }
    }
    return KXmlGuiWindow::eventFilter(obj, event);
}

bool Shell::isValid() const
{
    return m_isValid;
}

void Shell::showOpenRecentMenu()
{
    m_recent->menu()->popup(QCursor::pos());
}

Shell::~Shell()
{
    // The sidebar outlives the tab widget during teardown and its
    // visibilityChanged handler walks the tabs; stop listening first
    if (m_sidebar) {
        m_sidebar->disconnect(this);
    }

    if (!m_tabs.empty()) {
        writeSettings();
        for (const TabState &tab : std::as_const(m_tabs)) {
            if (tab.epubReader) {
                tab.epubReader->saveReadingPosition();
            } else if (tab.pdfReader) {
                tab.pdfReader->saveReadingPosition();
            }
        }
        m_tabs.clear();
    }
#if HAVE_DBUS
    if (m_unique) {
        QDBusConnection::sessionBus().unregisterService(QStringLiteral("io.github.todd866.paperlibrary"));
    }
#endif // HAVE_DBUS

    delete m_tabWidget;
    m_tabWidget = nullptr;
}

// Open a new document if we have space for it
// This can hang if called on a unique instance and openUrl pops a messageBox
bool Shell::openDocument(const QUrl &url, const QString &serializedOptions)
{
    if (m_tabs.size() <= 0) {
        return false;
    }

    openUrl(url, serializedOptions);

    return true;
}

bool Shell::openDocument(const QString &urlString, const QString &serializedOptions)
{
    return openDocument(QUrl(urlString), serializedOptions);
}

bool Shell::canOpenDocs(int numDocs, int desktop)
{
    if (m_tabs.size() <= 0 || numDocs <= 0 || m_unique) {
        return false;
    }

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS) && !defined(Q_OS_HAIKU)
    const KWindowInfo winfo(window()->effectiveWinId(), NET::WMDesktop);
    if (winfo.desktop() != desktop) {
        return false;
    }
#else
    Q_UNUSED(desktop);
#endif

    return true;
}

void Shell::openUrl(const QUrl &url, const QString &serializedOptions)
{
    Q_UNUSED(serializedOptions);

    // Apple Books ships EPUBs as directory bundles the EPUB engine cannot
    // open; repackage into a local .epub and open that instead. The imported
    // copy is a normal zipped file, so the recursion below terminates at once.
    if (EpubImporter::isDirectoryBundle(url)) {
        openImportedBundle(url, serializedOptions);
        return;
    }

    const int activeTab = m_tabWidget->currentIndex();
    if (m_tabs[activeTab].isLibrary()) {
        // Chrome semantics: a document chosen on a new-tab page navigates
        // that tab in place — unless it is already open in another tab, in
        // which case this library tab closes and that one gets the focus
        const int existingTab = findTabIndex(url);
        if (existingTab >= 0) {
            closeTab(activeTab);
            const int tabIndex = findTabIndex(url); // indexes shifted by the close
            setActiveTab(tabIndex);
            recordDocumentOpened(url);
            // The file may have changed on disk since that tab loaded it
            if (m_tabs[tabIndex].epubReader) {
                m_tabs[tabIndex].epubReader->reload();
            } else if (m_tabs[tabIndex].pdfReader) {
                m_tabs[tabIndex].pdfReader->reload();
            }
            return;
        }
        navigateLibraryTabToUrl(activeTab, url, serializedOptions);
        return;
    }

    if (m_tabs[activeTab].epubReader) {
        if (m_unique && shouldUseEpubWebReader(url)) {
            EpubWebReader *const oldReader = m_tabs[activeTab].epubReader;
            if (openEpubWebReaderInTab(activeTab, url)) {
                oldReader->deleteLater();
                recordDocumentOpened(url);
            }
        } else if (m_unique && shouldUsePdfView(url)) {
            EpubWebReader *const oldReader = m_tabs[activeTab].epubReader;
            if (openPdfViewInTab(activeTab, url)) {
                oldReader->deleteLater();
                recordDocumentOpened(url);
            }
        } else {
            openNewTab(url, serializedOptions);
        }
        return;
    }

    if (m_tabs[activeTab].pdfReader) {
        if (m_unique && shouldUsePdfView(url)) {
            PdfView *const oldReader = m_tabs[activeTab].pdfReader;
            if (openPdfViewInTab(activeTab, url)) {
                removePdfSideContainer(oldReader);
                oldReader->deleteLater();
                recordDocumentOpened(url);
            }
        } else if (m_unique && shouldUseEpubWebReader(url)) {
            PdfView *const oldReader = m_tabs[activeTab].pdfReader;
            if (openEpubWebReaderInTab(activeTab, url)) {
                removePdfSideContainer(oldReader);
                oldReader->deleteLater();
                recordDocumentOpened(url);
            }
        } else {
            openNewTab(url, serializedOptions);
        }
        return;
    }

    if (shouldUseEpubWebReader(url) || shouldUsePdfView(url)) {
        openNewTab(url, QString());
        return;
    }

    KMessageBox::error(this,
                       i18nc("@info", "PaperLibrary can open PDF and EPUB files. “%1” is not a supported document type.", url.fileName()),
                       i18nc("@title:window", "Unsupported Document"));
}

void Shell::navigateLibraryTabToUrl(int tab, const QUrl &url, const QString &serializedOptions)
{
    if (shouldUseEpubWebReader(url)) {
        LibraryView *const oldView = m_tabs[tab].libraryView;
        if (openEpubWebReaderInTab(tab, url)) {
            oldView->deleteLater();
            recordDocumentOpened(url);
        }
        return;
    }

    if (shouldUsePdfView(url)) {
        LibraryView *const oldView = m_tabs[tab].libraryView;
        if (openPdfViewInTab(tab, url)) {
            oldView->deleteLater();
            recordDocumentOpened(url);
        }
        return;
    }

    Q_UNUSED(serializedOptions);
    KMessageBox::error(this,
                       i18nc("@info", "PaperLibrary can open PDF and EPUB files. “%1” is not a supported document type.", url.fileName()),
                       i18nc("@title:window", "Unsupported Document"));
}

void Shell::recordDocumentOpened(const QUrl &url)
{
    m_recent->addUrl(url);
    m_libraryStore->recordOpen(url);
    m_libraryAutoTagger->enqueue(url);
}

void Shell::replaceTabPage(int index, QWidget *page, const QString &label, const QString &toolTip)
{
    // QTabWidget cannot swap a tab's page in place; remove and re-insert at
    // the same position with signals blocked, then re-run the activation by
    // hand so the tab never appears to leave the strip
    applyTitlebarSafeAreaOptOut(page);
    {
        const QSignalBlocker blocker(m_tabWidget);
        m_tabWidget->removeTab(index);
        m_tabWidget->insertTab(index, page, label);
        m_tabWidget->setTabToolTip(index, toolTip);
        m_tabWidget->setCurrentIndex(index);
    }
    setActiveTab(index);
    scheduleTitlebarLayoutUpdate();
}

void Shell::closeUrl()
{
    closeTab(m_tabWidget->currentIndex());

    // When closing the current tab two things can happen:
    //  * the focus was on the tab
    //  * the focus was somewhere in the toolbar
    // we don't have other places that accept focus
    //  * If it was on the tab, logic says it should go back to the next current tab
    //  * If it was on the toolbar, we could leave it there, but since we redo the menus/toolbars for the new tab, it gets kind of lost
    //    so it's easier to set it to the next current tab which also makes sense as consistency
    const int currentTab = m_tabWidget->currentIndex();
    if (currentTab >= 0 && currentTab < m_tabs.size()) {
        const TabState &state = m_tabs[currentTab];
        QWidget *const page = state.epubReader ? static_cast<QWidget *>(state.epubReader) : (state.pdfReader ? static_cast<QWidget *>(state.pdfReader) : static_cast<QWidget *>(state.libraryView));
        if (page) {
            page->setFocus();
        }
    }
}

void Shell::readSettings()
{
    readRecentFilesSettings();

    const KConfigGroup group = KSharedConfig::openConfig()->group(DesktopEntryGroupKey());
    bool fullScreen = group.readEntry("FullScreen", false);
    setFullScreen(fullScreen);

    if (fullScreen) {
        m_menuBarWasShown = group.readEntry(shouldShowMenuBarComingFromFullScreen, true);
        m_toolBarWasShown = group.readEntry(shouldShowToolBarComingFromFullScreen, true);
    }

    const KConfigGroup sidebarGroup = KSharedConfig::openConfig()->group(GeneralGroupKey());
    m_sidebarVisibleOnDocTabs = sidebarGroup.readEntry(SIDEBAR_VISIBLE_KEY, true);
    m_sidebar->setLocked(sidebarGroup.readEntry(SIDEBAR_LOCKED_KEY, true));

    // The startup tab is a library tab, which never shows the sidebar; the
    // remembered visibility applies once a document tab activates
    m_sidebar->setVisible(!currentTabIsLibrary() && m_sidebarVisibleOnDocTabs);
    if (m_showSidebarAction) {
        m_showSidebarAction->setChecked(m_sidebarVisibleOnDocTabs);
    }
    m_lockSidebarAction->setChecked(m_sidebar->isLocked());
    if (m_readerMotionAction) {
        m_readerMotionAction->setChecked(sidebarGroup.readEntry(READER_MOTION_KEY, true));
    }
}

void Shell::writeSettings()
{
    saveRecents();

    KConfigGroup sidebarGroup = KSharedConfig::openConfig()->group(GeneralGroupKey());
    sidebarGroup.writeEntry(SIDEBAR_LOCKED_KEY, m_sidebar->isLocked());
    // NOTE : m_sidebarVisibleOnDocTabs rather than the live visibility,
    // because the sidebar is forcibly hidden while a library tab is current
    sidebarGroup.writeEntry(SIDEBAR_VISIBLE_KEY, m_sidebarVisibleOnDocTabs);

    // Tab restore across launches: remember the open documents and which
    // one was current. Library tabs are transient new-tab pages — they are
    // not saved (matching the session-manager path in saveProperties)
    QStringList docUrls;
    int activeDocIndex = 0;
    const int activeTab = m_tabWidget->currentIndex();
    for (int i = 0; i < m_tabs.size(); ++i) {
        const TabState &state = m_tabs.at(i);
        if (state.epubReader) {
            state.epubReader->saveReadingPosition();
        } else if (state.pdfReader) {
            state.pdfReader->saveReadingPosition();
        }
        const QUrl url = state.epubReader ? state.epubReader->url() : (state.pdfReader ? state.pdfReader->url() : QUrl());
        if (!url.isLocalFile()) {
            continue;
        }
        if (i == activeTab) {
            activeDocIndex = docUrls.size();
        }
        docUrls.append(url.url());
    }
    sidebarGroup.writePathEntry(RESTORE_URLS_KEY, docUrls);
    sidebarGroup.writeEntry(RESTORE_TAB_KEY, activeDocIndex);

    KConfigGroup group = KSharedConfig::openConfig()->group(DesktopEntryGroupKey());
    group.writeEntry("FullScreen", m_fullScreenAction->isChecked());
    if (m_fullScreenAction->isChecked()) {
        group.writeEntry(shouldShowMenuBarComingFromFullScreen, m_menuBarWasShown);
        group.writeEntry(shouldShowToolBarComingFromFullScreen, m_toolBarWasShown);
    }
    KSharedConfig::openConfig()->sync();
}

void Shell::saveRecents()
{
    m_recent->saveEntries(KSharedConfig::openConfig()->group(RecentFilesGroupKey()));
}

void Shell::setupActions()
{
    KStandardAction::open(this, SLOT(fileOpen()), actionCollection());
    m_recent = KStandardAction::openRecent(this, SLOT(openUrl(QUrl)), actionCollection());
    m_recent->setToolBarMode(KRecentFilesAction::MenuMode);
    connect(m_recent, &QAction::triggered, this, &Shell::showOpenRecentMenu);
    m_recent->setToolTip(i18n("Click to open a file\nClick and hold to open a recent file"));
    m_recent->setWhatsThis(i18n("<b>Click</b> to open a file or <b>Click and hold</b> to select a recent file"));
    m_printAction = KStandardAction::print(this, SLOT(print()), actionCollection());
    m_printAction->setEnabled(false);
    m_closeAction = KStandardAction::close(this, SLOT(closeUrl()), actionCollection());
    m_closeAction->setEnabled(false);
    KStandardAction::quit(this, SLOT(close()), actionCollection());

    setStandardToolBarMenuEnabled(true);

    m_showMenuBarAction = KStandardAction::showMenubar(this, SLOT(slotShowMenubar()), actionCollection());
    m_fullScreenAction = KStandardAction::fullScreen(this, SLOT(slotUpdateFullScreen()), this, actionCollection());

    QAction *newTabAction = actionCollection()->addAction(QStringLiteral("new-tab"));
    newTabAction->setText(i18n("New Tab"));
    newTabAction->setIcon(QIcon::fromTheme(QStringLiteral("tab-new")));
#if defined(Q_OS_MACOS)
    // ⌘T (Qt::CTRL is ⌘ on macOS), as in Chrome/Safari. It appends a Library
    // tab — the new-tab page — from which a chosen document navigates that
    // tab in place; ⌘O keeps the file dialog.
    actionCollection()->setDefaultShortcut(newTabAction, QKeySequence(Qt::CTRL | Qt::Key_T));
#endif
    connect(newTabAction, &QAction::triggered, this, &Shell::openNewLibraryTab);

    m_nextTabAction = actionCollection()->addAction(QStringLiteral("tab-next"));
    m_nextTabAction->setText(i18n("Next Tab"));
#if defined(Q_OS_MACOS)
    // Add the tab-switching chords macOS users expect. Qt swaps Ctrl and Cmd
    // on macOS: Qt::CTRL is ⌘ and Qt::META is ⌃, so these are ⌃Tab, ⌘⇧] and ⌘⌥→.
    QList<QKeySequence> nextTabShortcuts = KStandardShortcut::tabNext();
    nextTabShortcuts << QKeySequence(Qt::META | Qt::Key_Tab) << QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketRight) << QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Right);
    actionCollection()->setDefaultShortcuts(m_nextTabAction, nextTabShortcuts);
#else
    actionCollection()->setDefaultShortcuts(m_nextTabAction, KStandardShortcut::tabNext());
#endif
    m_nextTabAction->setEnabled(false);
    connect(m_nextTabAction, &QAction::triggered, this, &Shell::activateNextTab);

    m_prevTabAction = actionCollection()->addAction(QStringLiteral("tab-previous"));
    m_prevTabAction->setText(i18n("Previous Tab"));
#if defined(Q_OS_MACOS)
    // ⌃⇧Tab, ⌘⇧[ and ⌘⌥←
    QList<QKeySequence> prevTabShortcuts = KStandardShortcut::tabPrev();
    prevTabShortcuts << QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Tab) << QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketLeft) << QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Left);
    actionCollection()->setDefaultShortcuts(m_prevTabAction, prevTabShortcuts);
#else
    actionCollection()->setDefaultShortcuts(m_prevTabAction, KStandardShortcut::tabPrev());
#endif
    m_prevTabAction->setEnabled(false);
    connect(m_prevTabAction, &QAction::triggered, this, &Shell::activatePrevTab);

    // add shortcuts for Shift+Alt+1 to Shift+Alt+9 to switch tabs(browser logic- Shift+Alt+9 is always going to be last tab)
    for (int i = 1; i <= 9; ++i) {
        QAction *action = actionCollection()->addAction(QStringLiteral("tab-switch-%1").arg(i));
        action->setText(i18n("Switch to Tab %1", i));

        // static cast to Qt::Key satisfies Qt 6 strict type checking for QKeySequence
#if defined(Q_OS_MACOS)
        // ⌘1..9 (Qt::CTRL is ⌘ on macOS), as in Chrome/Safari; the mouse-mode
        // PDF/EPUB document actions avoid these defaults on macOS to keep
        // the expected tab-switching keys free
        actionCollection()->setDefaultShortcut(action, QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_0 + i)));
#else
        actionCollection()->setDefaultShortcut(action, QKeySequence(Qt::SHIFT | Qt::ALT | static_cast<Qt::Key>(Qt::Key_0 + i)));
#endif
        connect(action, &QAction::triggered, this, [this, i]() {
            if (m_tabs.isEmpty()) {
                return;
            }
            int index = (i == 9) ? m_tabs.size() - 1 : i - 1;
            if (index >= 0 && index < m_tabs.size()) {
                setActiveTab(index);
            }
        });
    }

    m_undoCloseTab = actionCollection()->addAction(QStringLiteral("undo-close-tab"));
    m_undoCloseTab->setText(i18n("Undo close tab"));
    actionCollection()->setDefaultShortcut(m_undoCloseTab, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    m_undoCloseTab->setIcon(QIcon::fromTheme(QStringLiteral("edit-undo")));
    m_undoCloseTab->setEnabled(false);
    connect(m_undoCloseTab, &QAction::triggered, this, &Shell::undoCloseTab);

    const auto adjustCurrentEpubFont = [this](int delta) {
        const int tab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
        if (tab >= 0 && tab < m_tabs.size() && m_tabs[tab].epubReader) {
            m_tabs[tab].epubReader->adjustFontScale(delta);
        }
    };

    m_epubFontIncreaseAction = actionCollection()->addAction(QStringLiteral("epub-font-increase"));
    m_epubFontIncreaseAction->setText(i18n("Increase EPUB Font Size"));
    QList<QKeySequence> epubFontIncreaseShortcuts;
    epubFontIncreaseShortcuts << QKeySequence(Qt::CTRL | Qt::Key_Equal) << QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Equal) << QKeySequence(Qt::CTRL | Qt::Key_Plus);
    actionCollection()->setDefaultShortcuts(m_epubFontIncreaseAction, epubFontIncreaseShortcuts);
    m_epubFontIncreaseAction->setEnabled(false);
    connect(m_epubFontIncreaseAction, &QAction::triggered, this, [adjustCurrentEpubFont]() {
        adjustCurrentEpubFont(1);
    });

    m_epubFontDecreaseAction = actionCollection()->addAction(QStringLiteral("epub-font-decrease"));
    m_epubFontDecreaseAction->setText(i18n("Decrease EPUB Font Size"));
    actionCollection()->setDefaultShortcut(m_epubFontDecreaseAction, QKeySequence(Qt::CTRL | Qt::Key_Minus));
    m_epubFontDecreaseAction->setEnabled(false);
    connect(m_epubFontDecreaseAction, &QAction::triggered, this, [adjustCurrentEpubFont]() {
        adjustCurrentEpubFont(-1);
    });

    m_epubFontResetAction = actionCollection()->addAction(QStringLiteral("epub-font-reset"));
    m_epubFontResetAction->setText(i18n("Reset EPUB Font Size"));
    actionCollection()->setDefaultShortcut(m_epubFontResetAction, QKeySequence(Qt::CTRL | Qt::Key_0));
    m_epubFontResetAction->setEnabled(false);
    connect(m_epubFontResetAction, &QAction::triggered, this, [adjustCurrentEpubFont]() {
        adjustCurrentEpubFont(0);
    });

    auto *globalEpubScrollGroup = new QActionGroup(this);
    globalEpubScrollGroup->setExclusive(true);
    m_epubGlobalPaginatedAction = actionCollection()->addAction(QStringLiteral("epub-scroll-global-paginated"));
    m_epubGlobalPaginatedAction->setText(i18n("Global EPUB Scroll Mode: Paginated Pages"));
    m_epubGlobalPaginatedAction->setCheckable(true);
    globalEpubScrollGroup->addAction(m_epubGlobalPaginatedAction);
    connect(m_epubGlobalPaginatedAction, &QAction::triggered, this, [this]() {
        EpubWebReader::setGlobalScrollMode(EpubWebReader::ScrollMode::Paginated);
        for (TabState &state : m_tabs) {
            if (state.epubReader && !state.epubReader->hasBookScrollModeOverride()) {
                state.epubReader->setScrollMode(EpubWebReader::ScrollMode::Paginated);
            }
        }
        syncEpubScrollModeActions();
    });

    m_epubGlobalContinuousAction = actionCollection()->addAction(QStringLiteral("epub-scroll-global-continuous"));
    m_epubGlobalContinuousAction->setText(i18n("Global EPUB Scroll Mode: Infinite Scroll"));
    m_epubGlobalContinuousAction->setCheckable(true);
    globalEpubScrollGroup->addAction(m_epubGlobalContinuousAction);
    connect(m_epubGlobalContinuousAction, &QAction::triggered, this, [this]() {
        EpubWebReader::setGlobalScrollMode(EpubWebReader::ScrollMode::Continuous);
        for (TabState &state : m_tabs) {
            if (state.epubReader && !state.epubReader->hasBookScrollModeOverride()) {
                state.epubReader->setScrollMode(EpubWebReader::ScrollMode::Continuous);
            }
        }
        syncEpubScrollModeActions();
    });

    auto *bookEpubScrollGroup = new QActionGroup(this);
    bookEpubScrollGroup->setExclusive(true);
    const auto currentEpubReader = [this]() -> EpubWebReader * {
        const int tab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
        if (tab < 0 || tab >= m_tabs.size()) {
            return nullptr;
        }
        return m_tabs[tab].epubReader;
    };

    m_epubBookUseGlobalAction = actionCollection()->addAction(QStringLiteral("epub-scroll-book-global"));
    m_epubBookUseGlobalAction->setText(i18n("This EPUB: Use Global Scroll Mode"));
    m_epubBookUseGlobalAction->setCheckable(true);
    m_epubBookUseGlobalAction->setEnabled(false);
    bookEpubScrollGroup->addAction(m_epubBookUseGlobalAction);
    connect(m_epubBookUseGlobalAction, &QAction::triggered, this, [this, currentEpubReader]() {
        if (EpubWebReader *const reader = currentEpubReader()) {
            reader->clearBookScrollModeOverride();
        }
        syncEpubScrollModeActions();
    });

    m_epubBookPaginatedAction = actionCollection()->addAction(QStringLiteral("epub-scroll-book-paginated"));
    m_epubBookPaginatedAction->setText(i18n("This EPUB: Paginated Pages"));
    m_epubBookPaginatedAction->setCheckable(true);
    m_epubBookPaginatedAction->setEnabled(false);
    bookEpubScrollGroup->addAction(m_epubBookPaginatedAction);
    connect(m_epubBookPaginatedAction, &QAction::triggered, this, [this, currentEpubReader]() {
        if (EpubWebReader *const reader = currentEpubReader()) {
            reader->setBookScrollModeOverride(EpubWebReader::ScrollMode::Paginated);
        }
        syncEpubScrollModeActions();
    });

    m_epubBookContinuousAction = actionCollection()->addAction(QStringLiteral("epub-scroll-book-continuous"));
    m_epubBookContinuousAction->setText(i18n("This EPUB: Infinite Scroll"));
    m_epubBookContinuousAction->setCheckable(true);
    m_epubBookContinuousAction->setEnabled(false);
    bookEpubScrollGroup->addAction(m_epubBookContinuousAction);
    connect(m_epubBookContinuousAction, &QAction::triggered, this, [this, currentEpubReader]() {
        if (EpubWebReader *const reader = currentEpubReader()) {
            reader->setBookScrollModeOverride(EpubWebReader::ScrollMode::Continuous);
        }
        syncEpubScrollModeActions();
    });

    m_epubBackAction = actionCollection()->addAction(QStringLiteral("epub-history-back"));
    m_epubBackAction->setText(i18n("Back in EPUB"));
    QList<QKeySequence> epubBackShortcuts;
    epubBackShortcuts << QKeySequence(Qt::ALT | Qt::Key_Left) << QKeySequence(Qt::META | Qt::Key_BracketLeft);
    actionCollection()->setDefaultShortcuts(m_epubBackAction, epubBackShortcuts);
    m_epubBackAction->setEnabled(false);
    connect(m_epubBackAction, &QAction::triggered, this, [this, currentEpubReader]() {
        if (EpubWebReader *const reader = currentEpubReader()) {
            reader->navigateBackInBook();
        }
        syncEpubHistoryActions();
    });

    m_epubForwardAction = actionCollection()->addAction(QStringLiteral("epub-history-forward"));
    m_epubForwardAction->setText(i18n("Forward in EPUB"));
    QList<QKeySequence> epubForwardShortcuts;
    epubForwardShortcuts << QKeySequence(Qt::ALT | Qt::Key_Right) << QKeySequence(Qt::META | Qt::Key_BracketRight);
    actionCollection()->setDefaultShortcuts(m_epubForwardAction, epubForwardShortcuts);
    m_epubForwardAction->setEnabled(false);
    connect(m_epubForwardAction, &QAction::triggered, this, [this, currentEpubReader]() {
        if (EpubWebReader *const reader = currentEpubReader()) {
            reader->navigateForwardInBook();
        }
        syncEpubHistoryActions();
    });
    syncEpubScrollModeActions();
    syncEpubHistoryActions();

    m_scanAppleBooksAction = actionCollection()->addAction(QStringLiteral("scan-apple-books-on-startup"));
    m_scanAppleBooksAction->setText(i18n("Scan Apple Books on Startup"));
    m_scanAppleBooksAction->setCheckable(true);
    {
        const KConfigGroup group = KSharedConfig::openConfig()->group(GeneralGroupKey());
        m_scanAppleBooksAction->setChecked(group.readEntry(APPLE_BOOKS_SCAN_KEY, true));
    }
    connect(m_scanAppleBooksAction, &QAction::triggered, this, [this](bool checked) {
        KConfigGroup group = KSharedConfig::openConfig()->group(GeneralGroupKey());
        group.writeEntry(APPLE_BOOKS_SCAN_KEY, checked);
        group.sync();
        for (const TabState &state : m_tabs) {
            if (state.libraryView) {
                state.libraryView->refresh();
            }
        }
    });

    m_readerMotionAction = actionCollection()->addAction(QStringLiteral("reader-motion"));
    m_readerMotionAction->setText(i18n("Reader Motion"));
    m_readerMotionAction->setCheckable(true);
    {
        const KConfigGroup group = KSharedConfig::openConfig()->group(GeneralGroupKey());
        m_readerMotionAction->setChecked(group.readEntry(READER_MOTION_KEY, true));
    }
    connect(m_readerMotionAction, &QAction::triggered, this, [this](bool checked) {
        PdfView::setReaderMotionEnabled(checked);
        EpubWebReader::setReaderMotionEnabled(checked);
        for (TabState &state : m_tabs) {
            if (state.epubReader) {
                state.epubReader->applyReaderMotionSetting();
            }
        }
    });

    m_autoTagLibraryAction = actionCollection()->addAction(QStringLiteral("library-auto-tag"));
    m_autoTagLibraryAction->setText(i18n("Auto-Tag Library with Local Claude"));
    m_autoTagLibraryAction->setCheckable(true);
    {
        const KConfigGroup group = KSharedConfig::openConfig()->group(GeneralGroupKey());
        m_autoTagLibraryAction->setChecked(group.readEntry(LIBRARY_AUTO_TAG_KEY, false));
    }
    connect(m_autoTagLibraryAction, &QAction::triggered, this, [this](bool checked) {
        KConfigGroup group = KSharedConfig::openConfig()->group(GeneralGroupKey());
        group.writeEntry(LIBRARY_AUTO_TAG_KEY, checked);
        group.sync();
        if (!checked || !m_libraryAutoTagger) {
            return;
        }
        const QList<LibraryStore::Entry> entries = m_libraryStore ? m_libraryStore->entries() : QList<LibraryStore::Entry>();
        for (const LibraryStore::Entry &entry : entries) {
            if (entry.title.isEmpty()) {
                m_libraryAutoTagger->enqueue(entry.url);
            }
        }
    });

    m_pdfShowSidebarAction = actionCollection()->addAction(QStringLiteral("pdf_show_leftpanel"));
    m_pdfShowSidebarAction->setText(i18n("Show Contents"));
    m_pdfShowSidebarAction->setCheckable(true);
    m_pdfShowSidebarAction->setIcon(QIcon::fromTheme(QStringLiteral("sidebar-expand")));
    m_pdfShowSidebarAction->setEnabled(false);

    const auto currentPdfReader = [this]() -> PdfView * {
        const int tab = m_tabWidget ? m_tabWidget->currentIndex() : -1;
        if (tab < 0 || tab >= m_tabs.size()) {
            return nullptr;
        }
        return m_tabs[tab].pdfReader;
    };

    m_pdfFindAction = actionCollection()->addAction(QStringLiteral("pdf-find"));
    m_pdfFindAction->setText(i18n("Find in PDF"));
    m_pdfFindAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-find")));
    actionCollection()->setDefaultShortcut(m_pdfFindAction, QKeySequence(QKeySequence::Find));
    m_pdfFindAction->setEnabled(false);
    connect(m_pdfFindAction, &QAction::triggered, this, [currentPdfReader]() {
        if (PdfView *const reader = currentPdfReader()) {
            reader->showFindBar();
        }
    });

    m_pdfFindNextAction = actionCollection()->addAction(QStringLiteral("pdf-find-next"));
    m_pdfFindNextAction->setText(i18n("Find Next in PDF"));
    actionCollection()->setDefaultShortcut(m_pdfFindNextAction, QKeySequence(Qt::CTRL | Qt::Key_G));
    m_pdfFindNextAction->setEnabled(false);
    connect(m_pdfFindNextAction, &QAction::triggered, this, [currentPdfReader]() {
        if (PdfView *const reader = currentPdfReader()) {
            reader->findNext();
        }
    });

    m_pdfFindPreviousAction = actionCollection()->addAction(QStringLiteral("pdf-find-previous"));
    m_pdfFindPreviousAction->setText(i18n("Find Previous in PDF"));
    actionCollection()->setDefaultShortcut(m_pdfFindPreviousAction, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    m_pdfFindPreviousAction->setEnabled(false);
    connect(m_pdfFindPreviousAction, &QAction::triggered, this, [currentPdfReader]() {
        if (PdfView *const reader = currentPdfReader()) {
            reader->findPrevious();
        }
    });

    m_pdfCopyAction = actionCollection()->addAction(QStringLiteral("pdf-copy"));
    m_pdfCopyAction->setText(i18n("Copy PDF Text"));
    actionCollection()->setDefaultShortcut(m_pdfCopyAction, QKeySequence(QKeySequence::Copy));
    m_pdfCopyAction->setEnabled(false);
    connect(m_pdfCopyAction, &QAction::triggered, this, [currentPdfReader]() {
        if (PdfView *const reader = currentPdfReader()) {
            reader->copy();
        }
    });

    m_pdfSelectAllAction = actionCollection()->addAction(QStringLiteral("pdf-select-all"));
    m_pdfSelectAllAction->setText(i18n("Select All PDF Text"));
    actionCollection()->setDefaultShortcut(m_pdfSelectAllAction, QKeySequence(QKeySequence::SelectAll));
    m_pdfSelectAllAction->setEnabled(false);
    connect(m_pdfSelectAllAction, &QAction::triggered, this, [currentPdfReader]() {
        if (PdfView *const reader = currentPdfReader()) {
            reader->selectAll();
        }
    });

    m_pdfZoomInAction = actionCollection()->addAction(QStringLiteral("pdf-zoom-in"));
    m_pdfZoomInAction->setText(i18n("Zoom In PDF"));
    QList<QKeySequence> pdfZoomInShortcuts;
    pdfZoomInShortcuts << QKeySequence(Qt::CTRL | Qt::Key_Equal) << QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Equal) << QKeySequence(Qt::CTRL | Qt::Key_Plus);
    actionCollection()->setDefaultShortcuts(m_pdfZoomInAction, pdfZoomInShortcuts);
    m_pdfZoomInAction->setEnabled(false);
    connect(m_pdfZoomInAction, &QAction::triggered, this, [currentPdfReader]() {
        if (PdfView *const reader = currentPdfReader()) {
            reader->zoomIn();
        }
    });

    m_pdfZoomOutAction = actionCollection()->addAction(QStringLiteral("pdf-zoom-out"));
    m_pdfZoomOutAction->setText(i18n("Zoom Out PDF"));
    actionCollection()->setDefaultShortcut(m_pdfZoomOutAction, QKeySequence(Qt::CTRL | Qt::Key_Minus));
    m_pdfZoomOutAction->setEnabled(false);
    connect(m_pdfZoomOutAction, &QAction::triggered, this, [currentPdfReader]() {
        if (PdfView *const reader = currentPdfReader()) {
            reader->zoomOut();
        }
    });

    m_pdfFitWidthAction = actionCollection()->addAction(QStringLiteral("pdf-fit-width"));
    m_pdfFitWidthAction->setText(i18n("Fit PDF to Width"));
    actionCollection()->setDefaultShortcut(m_pdfFitWidthAction, QKeySequence(Qt::CTRL | Qt::Key_0));
    m_pdfFitWidthAction->setEnabled(false);
    connect(m_pdfFitWidthAction, &QAction::triggered, this, [currentPdfReader]() {
        if (PdfView *const reader = currentPdfReader()) {
            reader->fitToWidth();
        }
    });

    m_lockSidebarAction = actionCollection()->addAction(QStringLiteral("paperlibrary_lock_sidebar"));
    m_lockSidebarAction->setCheckable(true);
    m_lockSidebarAction->setIcon(QIcon::fromTheme(QStringLiteral("lock")));
    m_lockSidebarAction->setText(i18n("Lock Sidebar"));
    connect(m_lockSidebarAction, &QAction::triggered, m_sidebar, &Sidebar::setLocked);
    m_sidebar->addAction(m_lockSidebarAction);
}

void Shell::saveProperties(KConfigGroup &group)
{
    if (!m_isValid) {
        return;
    }

    // Gather lists of settings to preserve. Library tabs are transient
    // new-tab pages: the session keeps the documents only, and the active
    // index is remapped to the saved list
    QStringList urls;
    int activeDocIndex = 0;
    const int activeTab = m_tabWidget->currentIndex();
    for (int i = 0; i < m_tabs.size(); ++i) {
        const TabState &state = m_tabs.at(i);
        if (state.pdfReader) {
            state.pdfReader->saveReadingPosition();
        }
        const QUrl url = state.epubReader ? state.epubReader->url() : (state.pdfReader ? state.pdfReader->url() : QUrl());
        if (url.isEmpty()) {
            continue;
        }
        if (i == activeTab) {
            activeDocIndex = urls.size();
        }
        urls.append(url.url());
    }
    group.writePathEntry(SESSION_URL_KEY, urls);
    group.writeEntry(SESSION_TAB_KEY, activeDocIndex);
}

void Shell::readProperties(const KConfigGroup &group)
{
    // Reopen documents based on saved settings
    QStringList urls = group.readPathEntry(SESSION_URL_KEY, QStringList());
    int desiredTab = group.readEntry<int>(SESSION_TAB_KEY, 0);

    while (!urls.isEmpty()) {
        openUrl(QUrl(urls.takeFirst()));
    }

    if (desiredTab < m_tabs.size()) {
        setActiveTab(desiredTab);
    }
}

// The restore switch is read from the app config directly so tab restore stays
// independent from any document widget implementation.
static bool restoreTabsOnStartup()
{
    const QString overridePath = qEnvironmentVariable("PAPERLIBRARY_CONFIG_PATH");
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString configFilePath =
        !overridePath.isEmpty() ? overridePath : (configDir.isEmpty() ? QStringLiteral("paperlibraryrc") : configDir + QLatin1String("/paperlibraryrc"));
    const KConfigGroup group = KSharedConfig::openConfig(configFilePath, KConfig::SimpleConfig)->group(GeneralGroupKey());
    return group.readEntry("RestoreTabsOnStartup", true);
}

void Shell::restoreSavedTabs()
{
    if (!restoreTabsOnStartup()) {
        return;
    }

    const KConfigGroup group = KSharedConfig::openConfig()->group(GeneralGroupKey());
    const QStringList urls = group.readPathEntry(RESTORE_URLS_KEY, QStringList());
    for (const QString &urlString : urls) {
        const QUrl url(urlString);
        if (!url.isLocalFile() || !QFileInfo::exists(url.toLocalFile())) {
            continue; // the document has moved on since the last run
        }
        if (currentTabIsLibrary()) {
            openUrl(url); // the launch library tab navigates in place
        } else {
            openNewTab(url, QString()); // deterministic tabs, whatever the open-in-tabs setting
        }
    }

    const int desiredTab = group.readEntry(RESTORE_TAB_KEY, 0);
    if (desiredTab >= 0 && desiredTab < m_tabs.size()) {
        setActiveTab(desiredTab);
    }
}

void Shell::fileOpen()
{
    // this slot is called whenever the File->Open menu is selected,
    // the Open shortcut is pressed (usually CTRL+O) or the Open toolbar
    // button is clicked
    const int activeTab = m_tabWidget->currentIndex();

    QUrl startDir;
    if (activeTab >= 0 && activeTab < m_tabs.size()) {
        const TabState &state = m_tabs.at(activeTab);
        const QUrl currentUrl = state.epubReader ? state.epubReader->url() : (state.pdfReader ? state.pdfReader->url() : QUrl());
        if (currentUrl.isLocalFile()) {
            startDir = KIO::upUrl(currentUrl);
        }
    }
    if (startDir.isEmpty() || (startDir == QUrl::fromLocalFile(QDir::rootPath()))) {
        startDir = QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    }

    QPointer<QFileDialog> dlg(new QFileDialog(this));
    dlg->setDirectoryUrl(startDir);
    dlg->setAcceptMode(QFileDialog::AcceptOpen);
    dlg->setOption(QFileDialog::HideNameFilterDetails, true);
    dlg->setFileMode(QFileDialog::ExistingFiles); // Allow selection of more than one file

    // Unfortunately non Plasma file dialogs don't support the "All supported files" when using
    // setMimeTypeFilters instead of setNameFilters, so for those use setNameFilters which is a bit
    // worse because doesn't show you pdf files named bla.blo when you say "show me the pdf files", but
    // that's solvable by choosing "All Files" and it's not that common while it's more convenient to
    // only get shown the files that the application can open by default instead of all of them
    const bool useMimeTypeFilters = qgetenv("XDG_CURRENT_DESKTOP").toLower() == "kde";
    if (useMimeTypeFilters) {
        QStringList mimetypes {QStringLiteral("application/pdf"), QStringLiteral("application/epub+zip")};
        dlg->setMimeTypeFilters(mimetypes);
    } else {
        const QStringList namePatterns {
            i18n("All supported files (*.pdf *.epub)"),
            i18n("PDF documents (*.pdf)"),
            i18n("EPUB books (*.epub)"),
            i18n("All files (*)"),
        };
        dlg->setNameFilters(namePatterns);
    }

    dlg->setWindowTitle(i18n("Open Document")); /* cppcheck-suppress nullPointerRedundantCheck ; QPointer things here is not understood*/
    if (dlg->exec() && dlg) {                   /* cppcheck-suppress nullPointerRedundantCheck ; QPointer things here is not understood*/
        const QList<QUrl> urlList = dlg->selectedUrls();
        for (const QUrl &url : urlList) {
            openUrl(url);
        }
    }

    if (dlg) {
        delete dlg.data();
    }
}

void Shell::tryRaise(const QString &startupId)
{
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS) && !defined(Q_OS_HAIKU)
    if (KWindowSystem::isPlatformWayland()) {
        KWindowSystem::setCurrentXdgActivationToken(startupId);
    } else if (KWindowSystem::isPlatformX11()) {
        KStartupInfo::setNewStartupId(window()->windowHandle(), startupId.toUtf8());
    }
#else
    Q_UNUSED(startupId);
#endif

    KWindowSystem::activateWindow(window()->windowHandle());
}

// only called when starting the program
void Shell::setFullScreen(bool useFullScreen)
{
    if (useFullScreen) {
        setWindowState(windowState() | Qt::WindowFullScreen); // set
    } else {
        setWindowState(windowState() & ~Qt::WindowFullScreen); // reset
    }
}

void Shell::setCaption(const QString &caption)
{
    KXmlGuiWindow::setCaption(caption, false);
}

void Shell::showEvent(QShowEvent *e)
{
    if (!menuBar()->isNativeMenuBar() && m_showMenuBarAction) {
        m_showMenuBarAction->setChecked(menuBar()->isVisible());
    }

    KXmlGuiWindow::showEvent(e);

#if defined(Q_OS_MACOS)
    if (m_titlebarTabs) {
        QWindow *const handle = windowHandle();
        // Wire once per native window. If Qt tears the platform window down
        // and builds a new one, the QPointer differs from the live handle and
        // we re-wire (the old connections died with the old QWindow).
        if (handle && handle != m_titlebarHandle) {
            m_titlebarHandle = handle;
            // Qt can restyle the native surface on window-state and screen
            // changes; re-adopt and remeasure whenever it might have.
            connect(handle, &QWindow::windowStateChanged, this, [this](Qt::WindowState) { updateTitlebarLayout(); });
            connect(handle, &QWindow::screenChanged, this, [this](QScreen *) { updateTitlebarLayout(); });
        }
        updateTitlebarLayout();
        // The native toolbar can finish coming up a tick after show; adopt
        // once more so the first painted frame is already styled.
        QTimer::singleShot(0, this, [this]() { updateTitlebarLayout(); });
    }
#endif
}

void Shell::scheduleTitlebarLayoutUpdate()
{
#if defined(Q_OS_MACOS)
    if (!m_titlebarTabs) {
        return;
    }

    // Tab insert/remove can make Qt revisit safe-area-aware contents margins.
    // The synchronous work here is intentionally tiny so the tab can become
    // current before any native titlebar measuring/re-layout happens.
    paperLibraryTabDebugMark(QStringLiteral("titlebar safe-area opt-out start"));
    applyTitlebarSafeAreaOptOut();
    paperLibraryTabDebugMark(QStringLiteral("titlebar safe-area opt-out end"));

    if (!m_titlebarLayoutUpdatePending) {
        m_titlebarLayoutUpdatePending = true;
        QTimer::singleShot(0, this, [this]() {
            m_titlebarLayoutUpdatePending = false;
            updateTitlebarLayout(false);
        });
    }
    if (!m_titlebarLayoutFollowupPending) {
        m_titlebarLayoutFollowupPending = true;
        QTimer::singleShot(50, this, [this]() {
            m_titlebarLayoutFollowupPending = false;
            updateTitlebarLayout(false);
        });
    }
#endif
}

void Shell::applyTitlebarSafeAreaOptOut()
{
#if defined(Q_OS_MACOS)
    // Qt can re-enable the titlebar safe-area margin while rebuilding the
    // central-widget layout after tab insert/remove. Clear it on every widget
    // in the chrome stack that may participate in that geometry pass, so the
    // top row keeps starting at the true window top.
    if (!m_titlebarTabs) {
        return;
    }

    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    if (!m_tabWidget) {
        return;
    }

    m_tabWidget->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    if (m_tabWidget->tabBar()) {
        m_tabWidget->tabBar()->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    }
    if (m_tabWidget->toolbar()) {
        m_tabWidget->toolbar()->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    }
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        applyTitlebarSafeAreaOptOut(m_tabWidget->widget(i));
    }
#endif
}

void Shell::applyTitlebarSafeAreaOptOut(QWidget *widget)
{
#if defined(Q_OS_MACOS)
    if (!m_titlebarTabs || !widget) {
        return;
    }

    const auto optOutWidget = [](QWidget *target) {
        if (!target) {
            return;
        }
        target->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
        if (QAbstractScrollArea *const scrollArea = qobject_cast<QAbstractScrollArea *>(target)) {
            if (QWidget *const viewport = scrollArea->viewport()) {
                viewport->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
            }
        }
    };

    optOutWidget(widget);
    const QList<QWidget *> childWidgets = widget->findChildren<QWidget *>();
    for (QWidget *const child : childWidgets) {
        optOutWidget(child);
    }
#else
    Q_UNUSED(widget);
#endif
}

void Shell::updateTitlebarLayout(bool adoptNativeWindow)
{
#if defined(Q_OS_MACOS)
    if (!m_titlebarTabs || !m_tabWidget) {
        return;
    }
    QWindow *const handle = windowHandle();
    if (!handle) {
        return;
    }

    paperLibraryTabDebugMark(QStringLiteral("titlebar update start mode=%1").arg(adoptNativeWindow ? QStringLiteral("full-adopt") : QStringLiteral("light-repin")));
    applyTitlebarSafeAreaOptOut();
    if (adoptNativeWindow) {
        paperLibraryTabDebugMark(QStringLiteral("adoptWindow start"));
        MacTitlebar::adoptWindow(handle);
        paperLibraryTabDebugMark(QStringLiteral("adoptWindow end"));
    } else {
        paperLibraryTabDebugMark(QStringLiteral("repinWindow start"));
        MacTitlebar::repinWindow(handle);
        paperLibraryTabDebugMark(QStringLiteral("repinWindow end"));
    }

    const bool fullscreen = handle->windowStates().testFlag(Qt::WindowFullScreen);
    const MacTitlebar::Metrics metrics = MacTitlebar::measure(handle);
    // Fullscreen hides the traffic lights, which zeroes the measurement; keep
    // the last good windowed height/inset and just collapse the inset below.
    // Accept only sane values so a transient degenerate measurement (a
    // momentarily-zero content region during a transition) cannot balloon the
    // strip and persist as "last good".
    if (!fullscreen) {
        // With no native toolbar the titlebar is the standard height (~28-32
        // pt), which would cramp the tabs; keep the strip at a comfortable
        // Chrome height (floored) and let the traffic lights rest near its top.
        static constexpr int kChromeMinStripHeight = 40;
        const int stripHeight = qMax(metrics.titlebarHeight, kChromeMinStripHeight);
        if (stripHeight >= 24 && stripHeight <= 96) {
            m_measuredStripHeight = stripHeight;
        }
        if (metrics.leadingInset > 0 && metrics.leadingInset <= 200) {
            m_measuredInset = metrics.leadingInset;
        }
    }

    m_tabWidget->setStripHeight(m_measuredStripHeight);
    m_tabWidget->setLeadingInset(m_measuredInset);
    m_tabWidget->setFullscreen(fullscreen);
    // Bare-strip drag / double-click only makes sense while the strip is the
    // windowed titlebar (fullscreen has no titlebar to move or zoom).
    m_tabWidget->setWindowDragEnabled(!fullscreen);

    // Keep the left dock's content out from under the traffic lights; needed
    // only when it actually sits under them (left dock, windowed).
    if (m_sidebar) {
        const bool underButtons = !fullscreen && dockWidgetArea(m_sidebar) == Qt::LeftDockWidgetArea;
        m_sidebar->setTitlebarClearance(underButtons ? m_measuredStripHeight : 0);
    }

    // Verify-by-launch hook: dump the live NSWindow properties on demand.
    if (qEnvironmentVariableIsSet("PAPERLIBRARY_TITLEBAR_DUMP")) {
        qInfo().noquote() << MacTitlebar::describe(handle);
        const QPoint clientTop = handle->geometry().topLeft();
        const QPoint stripTop = m_tabWidget->chromeTabBar() ? m_tabWidget->chromeTabBar()->mapToGlobal(QPoint(0, 0)) : QPoint();
        // The strip's top should equal the window's client top once the
        // safe-area opt-out lands the central widget at y=0; a gap means the
        // titlebar inset crept back.
        qInfo().noquote() << QStringLiteral("MacTitlebar: windowClientTop=%1 stripTop=%2 (gap=%3)").arg(clientTop.y()).arg(stripTop.y()).arg(stripTop.y() - clientTop.y());
    }
    paperLibraryTabDebugMark(QStringLiteral("titlebar update end mode=%1").arg(adoptNativeWindow ? QStringLiteral("full-adopt") : QStringLiteral("light-repin")));
#endif
}

void Shell::slotUpdateFullScreen()
{
    // The XMLGui toolbars stay hidden in and out of fullscreen — the
    // ChromeToolbar row is the toolbar now — so only the menubar toggles
    if (m_fullScreenAction->isChecked()) {
        m_menuBarWasShown = !menuBar()->isHidden();
        menuBar()->hide();

        KToggleFullScreenAction::setFullScreen(this, true);
    } else {
        if (m_menuBarWasShown) {
            menuBar()->show();
        }
        KToggleFullScreenAction::setFullScreen(this, false);
    }
}

void Shell::slotShowMenubar()
{
    if (menuBar()->isHidden()) {
        menuBar()->show();
    } else {
        menuBar()->hide();
    }
}

QSize Shell::sizeHint() const
{
    const QSize baseSize = QApplication::primaryScreen()->availableSize() * 0.6;
    // Set an arbitrary yet sensible sane minimum size for very small screens;
    // for example we don't want people using 1366x768 screens to get a tiny
    // default window size of 820 x 460 which will elide most of the toolbar buttons.
    return baseSize.expandedTo(QSize(1000, 700));
}

bool Shell::queryClose()
{
    int documentTabs = 0;
    for (const TabState &tab : std::as_const(m_tabs)) {
        if (tab.isDocument()) {
            ++documentTabs;
        }
    }

    if (documentTabs > 1) {
        const QString dontAskAgainName = QStringLiteral("ShowTabWarning");
        KMessageBox::ButtonCode dummy = KMessageBox::PrimaryAction;
        if (KMessageBox::shouldBeShownTwoActions(dontAskAgainName, dummy)) {
            QDialog *dialog = new QDialog(this);
            dialog->setWindowTitle(i18n("Confirm Close"));

            QDialogButtonBox *buttonBox = new QDialogButtonBox(dialog);
            buttonBox->setStandardButtons(QDialogButtonBox::Yes | QDialogButtonBox::No);
            KGuiItem::assign(buttonBox->button(QDialogButtonBox::Yes), KGuiItem(i18n("Close Tabs"), QStringLiteral("tab-close")));
            KGuiItem::assign(buttonBox->button(QDialogButtonBox::No), KStandardGuiItem::cancel());

            bool checkboxResult = true;
            const int result = KMessageBox::createKMessageBox(dialog,
                                                              buttonBox,
                                                              QMessageBox::Question,
                                                              i18n("You are about to close %1 tabs. Are you sure you want to continue?", documentTabs),
                                                              QStringList(),
                                                              i18n("Warn me when I attempt to close multiple tabs"),
                                                              &checkboxResult,
                                                              KMessageBox::Notify);

            if (!checkboxResult) {
                KMessageBox::saveDontShowAgainTwoActions(dontAskAgainName, dummy);
            }

            if (result != QDialogButtonBox::Yes) {
                return false;
            }
        }
    }

    return true;
}

void Shell::setActiveTab(int tab)
{
    if (tab < 0 || tab >= m_tabs.size()) {
        return;
    }

    if (m_showSidebarConnection) {
        disconnect(m_showSidebarConnection);
        m_showSidebarConnection = {};
    }
    m_showSidebarAction = nullptr;

    m_tabWidget->setCurrentIndex(tab);
    setEpubFontActionsEnabled(m_tabs[tab].epubReader != nullptr);
    syncEpubScrollModeActions();
    syncEpubHistoryActions();
    setPdfReaderActionsEnabled(m_tabs[tab].pdfReader != nullptr);
    if (m_pdfShowSidebarAction) {
        m_pdfShowSidebarAction->setEnabled(false);
    }
    applyTitlebarSafeAreaOptOut(m_tabWidget->widget(tab));
    scheduleTitlebarLayoutUpdate();

    if (m_tabs[tab].isLibrary()) {
        hideXmlGuiToolbars();
        m_tabWidget->toolbar()->clearDocument();
        m_sidebar->hide();
        setCaption(i18n("PaperLibrary"));
        m_printAction->setEnabled(false);
        m_closeAction->setEnabled(true); // ⌘W closes the library tab
        return;
    }

    if (m_tabs[tab].epubReader) {
        EpubWebReader *const reader = m_tabs[tab].epubReader;
        hideXmlGuiToolbars();

        QWidget *const outlineWidget = reader->outlineWidget();
        if (outlineWidget && m_sidebar->indexOf(outlineWidget) == -1) {
            m_sidebar->addWidget(outlineWidget);
        }
        if (outlineWidget) {
            m_sidebar->setCurrentWidget(outlineWidget);
        }

        const bool outlineAvailable = reader->hasOutline();
        m_showSidebarAction = m_pdfShowSidebarAction;
        if (m_showSidebarAction) {
            m_showSidebarAction->setEnabled(outlineAvailable);
            m_showSidebarAction->setChecked(outlineAvailable && m_sidebarVisibleOnDocTabs);
            m_showSidebarConnection = connect(m_showSidebarAction, &QAction::triggered, this, [this](bool visible) {
                if (m_sidebar) {
                    m_sidebar->setAnimatedVisible(visible, PdfView::readerMotionEnabled());
                }
            });
        }

        m_tabWidget->toolbar()->setNavigationAction(m_pdfShowSidebarAction);
        m_sidebar->setAnimatedVisible(outlineAvailable && m_sidebarVisibleOnDocTabs, PdfView::readerMotionEnabled());
        setCaption(reader->url().fileName());
        m_printAction->setEnabled(false);
        m_closeAction->setEnabled(true);
        return;
    }

    if (m_tabs[tab].pdfReader) {
        PdfView *const reader = m_tabs[tab].pdfReader;
        hideXmlGuiToolbars();

        QWidget *const outlineWidget = reader->outlineWidget();
        if (outlineWidget && m_sidebar->indexOf(outlineWidget) == -1) {
            m_sidebar->addWidget(outlineWidget);
        }
        if (outlineWidget) {
            m_sidebar->setCurrentWidget(outlineWidget);
        }

        const bool outlineAvailable = reader->hasOutline();
        m_showSidebarAction = m_pdfShowSidebarAction;
        if (m_showSidebarAction) {
            m_showSidebarAction->setEnabled(outlineAvailable);
            m_showSidebarAction->setChecked(outlineAvailable && m_sidebarVisibleOnDocTabs);
            m_showSidebarConnection = connect(m_showSidebarAction, &QAction::triggered, this, [this](bool visible) {
                if (m_sidebar) {
                    m_sidebar->setAnimatedVisible(visible, PdfView::readerMotionEnabled());
                }
            });
        }

        m_tabWidget->toolbar()->setPdfView(reader, m_pdfShowSidebarAction);
        m_sidebar->setAnimatedVisible(outlineAvailable && m_sidebarVisibleOnDocTabs, PdfView::readerMotionEnabled());
        setCaption(reader->url().fileName());
        m_printAction->setEnabled(false);
        m_closeAction->setEnabled(true);
        return;
    }
}

void Shell::closeTab(int tab)
{
    if (tab < 0 || tab >= m_tabs.size()) {
        return;
    }

    if (m_tabs[tab].isLibrary()) {
        // Library tabs close without questions and are not restorable
        // (⌘⇧T is for documents); the sole tab of the window stays put
        if (m_tabs.count() == 1) {
            return;
        }
        LibraryView *const view = m_tabs[tab].libraryView;
        m_tabs.removeAt(tab);
        m_tabWidget->removeTab(tab);
        view->deleteLater();

        if (m_tabWidget->count() == 1) {
            m_nextTabAction->setEnabled(false);
            m_prevTabAction->setEnabled(false);
        }
        return;
    }

    if (m_tabs[tab].epubReader) {
        EpubWebReader *const reader = m_tabs[tab].epubReader;
        const QUrl url = reader->url();
        reader->saveReadingPosition();
        removeEpubSideContainer(reader);
        reader->disconnect(this);
        reader->deleteLater();

        if (!url.isEmpty()) {
            m_undoCloseTab->setEnabled(true);
            m_closedTabUrls.append(url);
        }

        if (m_tabs.count() > 1) {
            m_tabs.removeAt(tab);
            m_tabWidget->removeTab(tab);

            if (m_tabWidget->count() == 1) {
                m_nextTabAction->setEnabled(false);
                m_prevTabAction->setEnabled(false);
            }
        } else {
            LibraryView *const view = createLibraryView(true);
            m_tabs[tab] = TabState(view);
            replaceTabPage(tab, view, i18n("PaperLibrary"), QString());
        }
        return;
    }

    if (m_tabs[tab].pdfReader) {
        PdfView *const reader = m_tabs[tab].pdfReader;
        const QUrl url = reader->url();
        reader->saveReadingPosition();
        removePdfSideContainer(reader);
        reader->disconnect(this);
        reader->deleteLater();

        if (!url.isEmpty()) {
            m_undoCloseTab->setEnabled(true);
            m_closedTabUrls.append(url);
        }

        if (m_tabs.count() > 1) {
            m_tabs.removeAt(tab);
            m_tabWidget->removeTab(tab);

            if (m_tabWidget->count() == 1) {
                m_nextTabAction->setEnabled(false);
                m_prevTabAction->setEnabled(false);
            }
        } else {
            LibraryView *const view = createLibraryView(true);
            m_tabs[tab] = TabState(view);
            replaceTabPage(tab, view, i18n("PaperLibrary"), QString());
        }
        return;
    }

}

void Shell::openNewTab(const QUrl &url, const QString &serializedOptions)
{
    Q_UNUSED(serializedOptions);

    const int tabIndex = findTabIndex(url);
    if (tabIndex >= 0) {
        setActiveTab(tabIndex);
        recordDocumentOpened(url);
        // The file may have changed on disk since this tab loaded it
        // (e.g. re-exported and double-clicked again in Finder).
        if (m_tabs[tabIndex].epubReader) {
            m_tabs[tabIndex].epubReader->reload();
        } else if (m_tabs[tabIndex].pdfReader) {
            m_tabs[tabIndex].pdfReader->reload();
        }
        return;
    }

    const int newIndex = m_tabs.size();

    if (shouldUseEpubWebReader(url)) {
        EpubWebReader *const reader = new EpubWebReader(this);
        connectEpubWebReader(reader);
        if (reader->open(url)) {
            applyTitlebarSafeAreaOptOut(reader);
            m_tabs.append(TabState(reader));
            m_tabWidget->addTab(reader, url.fileName());
            m_tabWidget->setTabToolTip(newIndex, url.fileName());
            const QMimeType epubMime = QMimeDatabase().mimeTypeForName(QStringLiteral("application/epub+zip"));
            m_tabWidget->setTabIcon(newIndex, QIcon::fromTheme(epubMime.iconName()));
            m_nextTabAction->setEnabled(true);
            m_prevTabAction->setEnabled(true);
            setActiveTab(newIndex);
            recordDocumentOpened(url);
            return;
        }
        reader->deleteLater();
    }

    if (shouldUsePdfView(url)) {
        PdfView *const reader = new PdfView(this);
        connectPdfView(reader);
        if (reader->open(url)) {
            applyTitlebarSafeAreaOptOut(reader);
            m_tabs.append(TabState(reader));
            m_tabWidget->addTab(reader, url.fileName());
            m_tabWidget->setTabToolTip(newIndex, url.fileName());
            const QMimeType pdfMime = QMimeDatabase().mimeTypeForName(QStringLiteral("application/pdf"));
            m_tabWidget->setTabIcon(newIndex, QIcon::fromTheme(pdfMime.iconName()));
            m_nextTabAction->setEnabled(true);
            m_prevTabAction->setEnabled(true);
            setActiveTab(newIndex);
            recordDocumentOpened(url);
            return;
        }
        reader->deleteLater();
    }

    KMessageBox::error(this,
                       i18nc("@info", "PaperLibrary can open PDF and EPUB files. “%1” is not a supported document type.", url.fileName()),
                       i18nc("@title:window", "Unsupported Document"));
}

void Shell::openNewLibraryTab()
{
    const int debugSequence = paperLibraryTabDebugBegin(QStringLiteral("new-tab command handler entry"));

    // ⌘T appends a Library tab and selects it; several at once are fine —
    // Chrome allows unlimited new-tab pages
    if (m_tabs.size() == 1) {
        m_nextTabAction->setEnabled(true);
        m_prevTabAction->setEnabled(true);
    }

    LibraryView *const view = createLibraryView(true);
    paperLibraryTabDebugMark(QStringLiteral("page widget constructed"));
    m_tabs.append(TabState(view));
    const int index = m_tabWidget->addTab(view, i18n("PaperLibrary"));
    setActiveTab(index);
    paperLibraryTabDebugMark(QStringLiteral("new tab shown/current index=%1 count=%2").arg(index).arg(m_tabWidget->count()));
    QTimer::singleShot(120, this, [debugSequence]() { paperLibraryTabDebugEnd(debugSequence); });
}

void Shell::print()
{
    // Native print support has not been implemented for the shell-owned
    // readers yet. The action stays disabled until it is.
}

void Shell::activateNextTab()
{
    if (m_tabs.size() < 2) {
        return;
    }

    const int activeTab = m_tabWidget->currentIndex();
    const int nextTab = (activeTab == m_tabs.size() - 1) ? 0 : activeTab + 1;

    setActiveTab(nextTab);
}

void Shell::activatePrevTab()
{
    if (m_tabs.size() < 2) {
        return;
    }

    const int activeTab = m_tabWidget->currentIndex();
    const int prevTab = (activeTab == 0) ? m_tabs.size() - 1 : activeTab - 1;

    setActiveTab(prevTab);
}

void Shell::undoCloseTab()
{
    if (m_closedTabUrls.isEmpty()) {
        return;
    }

    const QUrl lastTabUrl = m_closedTabUrls.takeLast();

    if (m_closedTabUrls.isEmpty()) {
        m_undoCloseTab->setEnabled(false);
    }

    openUrl(lastTabUrl);
}

int Shell::findTabIndex(QObject *sender) const
{
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i].epubReader == sender || m_tabs[i].pdfReader == sender || (m_tabs[i].libraryView && m_tabs[i].libraryView == sender)) {
            return i;
        }
    }
    return -1;
}

int Shell::findTabIndex(const QUrl &url) const
{
    auto it = std::find_if(m_tabs.begin(), m_tabs.end(), [&url](const TabState &state) {
        return (state.epubReader && state.epubReader->url() == url) || (state.pdfReader && state.pdfReader->url() == url);
    });
    return (it != m_tabs.end()) ? std::distance(m_tabs.begin(), it) : -1;
}

void Shell::handleDroppedUrls(const QList<QUrl> &urls)
{
    for (const QUrl &url : urls) {
        openUrl(url);
    }
}

void Shell::moveTabData(int from, int to)
{
    m_tabs.move(from, to);
}

void Shell::jumpToBooksProgressWhenReady(const QUrl &url, double progress)
{
    if (progress < 0) {
        return;
    }
    const int tabIndex = findTabIndex(url);
    if (tabIndex < 0) {
        return; // the open failed, or spawned a separate window
    }
    if (m_tabs[tabIndex].epubReader) {
        m_tabs[tabIndex].epubReader->jumpToApproximateProgress(progress);
        return;
    }
    if (m_tabs[tabIndex].pdfReader) {
        m_tabs[tabIndex].pdfReader->jumpToApproximateProgress(progress);
        return;
    }
}

void Shell::openImportedBundle(const QUrl &bundleUrl, const QString &serializedOptions)
{
    const EpubImporter::Result result = EpubImporter::import(bundleUrl.toLocalFile());
    if (result.status != EpubImporter::Status::Imported) {
        const QString title = QFileInfo(bundleUrl.toLocalFile()).completeBaseName();
        QString message;
        switch (result.status) {
        case EpubImporter::Status::DrmProtected:
            message = i18nc("@info when opening a DRM-protected Apple Books EPUB", "“%1” is DRM-protected and can't be imported.", title);
            break;
        case EpubImporter::Status::NotDownloaded:
            message = i18nc("@info when opening an Apple Books EPUB that isn't downloaded", "“%1” isn't downloaded yet. Download it in Apple Books first, then try again.", title);
            break;
        default:
            message = i18nc("@info when an Apple Books EPUB can't be imported", "PaperLibrary couldn't import “%1”.", title);
            break;
        }
        KMessageBox::information(this, message, i18nc("@title:window", "Can't Open Book"));
        return;
    }

    // Open the local copy through the normal path (records it in the store, so
    // its next open is direct) and carry the Apple Books reading position over.
    const QUrl localUrl = QUrl::fromLocalFile(result.importedPath);
    openUrl(localUrl, serializedOptions);
    jumpToBooksProgressWhenReady(localUrl, result.progress);
}

void Shell::openFromLibrary(const QUrl &url, double booksProgress)
{
    // Activation navigates the library tab it happened on (normally the
    // current tab already; being explicit keeps programmatic activations honest)
    const int senderTab = findTabIndex(sender());
    if (senderTab >= 0 && senderTab != m_tabWidget->currentIndex()) {
        setActiveTab(senderTab);
    }

    // A directory-bundle EPUB imports on open (openUrl → openImportedBundle),
    // which itself lands the reading position from Apple Books; the tile's
    // progress only applies to documents opened directly, in place, here.
    openUrl(url);
    jumpToBooksProgressWhenReady(url, booksProgress);
}

void Shell::readRecentFilesSettings()
{
    // Read the max recent item count and populate File->Open Recent.
    const QString overridePath = qEnvironmentVariable("PAPERLIBRARY_CONFIG_PATH");
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString configFilePath =
        !overridePath.isEmpty() ? overridePath : (configDir.isEmpty() ? QStringLiteral("paperlibraryrc") : configDir + QLatin1String("/paperlibraryrc"));
    const KConfigGroup confgrp = KSharedConfig::openConfig(configFilePath, KConfig::SimpleConfig).data()->group(QStringLiteral("General"));
    const int defaultMaxRecentItems = 10;
    int maxRecentItems = confgrp.readEntry<int>("MaxRecentItems", defaultMaxRecentItems);
    m_recent->setMaxItems(maxRecentItems);
    m_recent->loadEntries(KSharedConfig::openConfig()->group(RecentFilesGroupKey()));
}

#include "shell.moc"

/* kate: replace-tabs on; indent-width 4; */
