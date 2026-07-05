/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_EPUBWEBREADER_H
#define PAPERLIBRARY_EPUBWEBREADER_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#ifndef PAPERLIBRARY_EPUBWEBREADER_CORE_ONLY
#include <QWidget>
#endif

class KZip;
class QCloseEvent;
class QHideEvent;
class QLabel;
class QModelIndex;
class QStandardItemModel;
class QTreeView;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineUrlRequestInterceptor;
class QWebEngineUrlSchemeHandler;
class QWebEngineView;

namespace EpubWebReaderCore
{
struct EpubNavigationEntry {
    QString title;
    QString fragment;
    int spineIndex = -1;
    int level = 1;
};

struct EpubInspection {
    bool isEpub = false;
    bool drmProtected = false;
    bool fixedLayout = false;
    QString packagePath;
    QString opfDir;
    QStringList spine;
    QList<EpubNavigationEntry> navigation;
    QHash<QString, QString> manifestMediaTypesByPath;
    QHash<QString, QString> manifestPathsByHref;
    QString title;

    bool supported() const
    {
        return isEpub && !drmProtected && !fixedLayout && !spine.isEmpty();
    }
};

struct ArchiveLookup {
    QString requestedPath;
    QString zipEntry;
    QStringList candidates;
    bool found = false;
};

struct StoredPosition {
    int spineIndex = 0;
    double scrollOffset = 0.0;
    bool restored = false;
    QString status;
};

QString cleanArchivePath(const QString &path);
QString resolvePackageHref(const QString &opfDir, const QString &href);
QStringList resolvePackageHrefCandidates(const QString &opfDir, const QString &href);
EpubInspection inspectEpub(const QString &path);
QString contentTypeForPath(const QString &path, const QString &manifestMediaType = QString());
ArchiveLookup lookupArchivePath(const KZip &zip, const QString &requestPath, const QString &opfDir, const QHash<QString, QString> &manifestPathsByHref);
QByteArray readArchiveFile(const KZip &zip, const QString &path);
QString positionKey(const QString &epubPath);
double cleanRestoredScrollOffset(double scrollOffset);
bool isRestorableScrollOffset(double scrollOffset);
double cleanReportedScrollOffset(double scrollOffset);
int cleanFontScaleStep(int step);
StoredPosition restoreStoredPosition(const QStringList &saved, int spineCount);
}

#ifndef PAPERLIBRARY_EPUBWEBREADER_CORE_ONLY
/**
 * Shell-owned WebEngine EPUB reader for PaperLibrary.
 *
 * The EPUB DOM reflows to the view width and paginates through CSS columns.
 */
class EpubWebReader : public QWidget
{
    Q_OBJECT

public:
    enum class ScrollMode {
        Paginated,
        Continuous,
    };
    Q_ENUM(ScrollMode)

    explicit EpubWebReader(QWidget *parent = nullptr);
    ~EpubWebReader() override;

    static void registerUrlScheme();
    static void configureBuildTreeRuntime();
    static bool canOpen(const QUrl &url);
    static bool readerMotionEnabled();
    static void setReaderMotionEnabled(bool enabled);
    static ScrollMode globalScrollMode();
    static void setGlobalScrollMode(ScrollMode mode);

    bool open(const QUrl &url);
    QUrl url() const;
    QWidget *outlineWidget() const;
    bool hasOutline() const;
    ScrollMode scrollMode() const;
    bool hasBookScrollModeOverride() const;
    void applyReaderMotionSetting();
    void reload();
    void jumpToApproximateProgress(double progress);
    void saveReadingPosition();

public Q_SLOTS:
    void adjustFontScale(int delta);
    void setBookScrollModeOverride(ScrollMode mode);
    void clearBookScrollModeOverride();
    void setScrollMode(ScrollMode mode);

Q_SIGNALS:
    void titleChanged(const QString &title);
    void loadFinished(bool ok);
    void renderProcessTerminated(const QString &message);
    void outlineAvailableChanged(bool available);
    void scrollModeChanged(EpubWebReader::ScrollMode mode, bool hasBookOverride);

protected:
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    bool loadSpineItem(int spineIndex, double scrollOffset, bool scrollToEnd = false, const QString &fragment = QString());
    void installReaderScripts();
    void applyPendingScrollOffset();
    void applyScrollMode();
    void saveScrollOffset(double scrollOffset);
    void rebuildOutlineModel();
    void selectCurrentNavigationEntry();
    void jumpToNavigationEntry(const QModelIndex &index);
    QUrl urlForSpineItem(int spineIndex) const;

    QUrl m_url;
    QString m_epubPath;
    QString m_bookId;
    QString m_bookTitle;
    QStringList m_spine;
    QList<EpubWebReaderCore::EpubNavigationEntry> m_navigationEntries;
    int m_spineIndex = 0;
    double m_pendingScrollOffset = 0.0;
    bool m_havePendingScrollOffset = false;
    bool m_pendingScrollToEnd = false;
    int m_fontScaleStep = 0;
    ScrollMode m_scrollMode = ScrollMode::Paginated;
    bool m_hasBookScrollModeOverride = false;

    QWebEngineProfile *m_profile = nullptr;
    QWebEnginePage *m_page = nullptr;
    QWebEngineView *m_view = nullptr;
    QWidget *m_outlineWidget = nullptr;
    QLabel *m_outlineTitle = nullptr;
    QTreeView *m_outlineView = nullptr;
    QStandardItemModel *m_outlineModel = nullptr;
    QWebEngineUrlSchemeHandler *m_schemeHandler = nullptr;
    QWebEngineUrlRequestInterceptor *m_requestInterceptor = nullptr;
};
#endif

#endif
