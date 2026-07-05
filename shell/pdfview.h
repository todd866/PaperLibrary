/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_PDFVIEW_H
#define PAPERLIBRARY_PDFVIEW_H

#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QWidget>

class QCloseEvent;
class QEvent;
class QHideEvent;
class QIdentityProxyModel;
class QAction;
class QModelIndex;
class QPdfBookmarkModel;
class QPdfDocument;
class QPdfPageNavigator;
class QPdfSearchModel;
class QPdfView;
class QPixmap;
class QProcess;
class QProgressBar;
class QLineEdit;
class QLabel;
class QStandardItemModel;
class QToolButton;
class QTreeView;

/** Shell-owned QtPdf/PDFium PDF reader for PaperLibrary. */
class PdfView : public QWidget
{
    Q_OBJECT

public:
    struct AiNavigationEntry {
        QString title;
        int pageOneBased = 0;
        int level = 1;
    };

    explicit PdfView(QWidget *parent = nullptr);
    ~PdfView() override;

    static bool canOpen(const QUrl &url);
    static bool readerMotionEnabled();
    static void setReaderMotionEnabled(bool enabled);

    bool open(const QUrl &url);
    QUrl url() const;
    void reload();
    void saveReadingPosition();

    QWidget *outlineWidget() const;
    bool hasOutline() const;

    int currentPageOneBased() const;
    int pageCount() const;
    qreal zoomFactor() const;
    bool fitWidthMode() const;
    QAction *findAction() const;
    QAction *aiNavigationAction() const;

    void jumpToApproximateProgress(double progress);

public Q_SLOTS:
    void goToPageOneBased(int page);
    void previousPage();
    void nextPage();
    void zoomIn();
    void zoomOut();
    void fitToWidth();
    void showFindBar();
    void closeFindBar();
    void findNext();
    void findPrevious();
    void copy();
    void selectAll();

Q_SIGNALS:
    void titleChanged(const QString &title);
    void loadFinished(bool ok);
    void errorOccurred(const QString &message);
    void pageStateChanged(int currentPage, int pageCount);
    void zoomStateChanged(qreal zoomFactor, bool fitWidthMode);
    void outlineAvailableChanged(bool available);

protected:
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void jumpToBookmark(const QModelIndex &index);
    void restoreReadingPosition();
    void scheduleDeferredScrollRestore();
    void emitPageState();
    void emitZoomState();
    void emitOutlineState();
    void activateSearchResult(int index);
    void updateFindState();
    void resetFindState();
    void setupFindBar();
    QString positionKey() const;
    void jumpToPage(int zeroBasedPage, const QPointF &location, qreal zoom, bool animated);
    void animatePageTurn(const QPixmap &snapshot, int direction);
    void generateAiNavigation();
    void cancelAiNavigationRun();
    void resetAiNavigationForDocument();
    void loadAiNavigationCache();
    void startAiNavigationTextExtraction();
    void continueAiNavigationTextExtraction();
    void askClaudeForAiNavigation();
    void finishAiNavigationRun(const QString &message);
    void setAiNavigationStatus(const QString &message, bool keepVisible = true);
    void updateAiNavigationActionState();
    void updateAiNavigationUi();
    void rebuildAiNavigationModel();
    void jumpToAiNavigationEntry(const QModelIndex &index);
    void saveAiNavigationCache() const;
    QList<AiNavigationEntry> realOutlineEntries() const;

    QUrl m_url;
    QString m_pdfPath;
    QString m_title;

    QPdfDocument *m_document = nullptr;
    QPdfView *m_view = nullptr;
    QPdfSearchModel *m_searchModel = nullptr;
    QPdfBookmarkModel *m_bookmarkModel = nullptr;
    QIdentityProxyModel *m_bookmarkTitleModel = nullptr;
    QAction *m_findAction = nullptr;
    QWidget *m_findBar = nullptr;
    QLineEdit *m_findField = nullptr;
    QLabel *m_findStatusLabel = nullptr;
    QWidget *m_outlineWidget = nullptr;
    QLabel *m_outlineTitle = nullptr;
    QTreeView *m_outlineView = nullptr;
    QAction *m_aiNavigationAction = nullptr;
    QLabel *m_aiNavigationStatusLabel = nullptr;
    QProgressBar *m_aiNavigationProgress = nullptr;
    QToolButton *m_aiNavigationButton = nullptr;
    QTreeView *m_aiNavigationView = nullptr;
    QStandardItemModel *m_aiNavigationModel = nullptr;
    QLabel *m_pageTransitionOverlay = nullptr;
    QProcess *m_aiNavigationProcess = nullptr;
    QString m_claudeExecutable;
    QString m_aiNavigationCachePath;
    QString m_aiNavigationFingerprintHash;
    QStringList m_aiNavigationExtractedPages;
    QList<AiNavigationEntry> m_aiNavigationEntries;

    int m_currentSearchIndex = -1;
    int m_pendingRestorePage = 0;
    int m_pendingRestoreScroll = 0;
    qreal m_pendingRestoreZoom = 1.0;
    bool m_pendingRestoreFitWidth = true;
    bool m_havePendingRestore = false;
    int m_aiNavigationExtractionPage = 0;
    int m_aiNavigationWordCount = 0;
    bool m_aiNavigationTextTruncated = false;
    bool m_aiNavigationRunning = false;
    bool m_aiNavigationStatusVisible = false;
};

#endif
