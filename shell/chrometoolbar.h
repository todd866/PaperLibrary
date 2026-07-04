/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_CHROMETOOLBAR_H
#define PAPERLIBRARY_CHROMETOOLBAR_H

#include <QHash>
#include <QPointer>
#include <QWidget>

class ChromeToolButton;
class PdfView;
class QAction;
class QHBoxLayout;
class QLabel;
class QMenu;
class QSpinBox;
class QToolButton;

/**
 * The one slim toolbar row, in Chrome's language: flat, hairline-free,
 * sitting between the tab strip and the document. Left to right: sidebar
 * toggle, back/forward history, the editable page field, then (right
 * aligned) zoom out/level/in, the browse/text-select tool switch, search
 * and an overflow "⋮" menu with the remaining useful document actions.
 *
 * PDF tabs wire this row to shell-native actions; EPUB and Library tabs hide
 * it. Icons come from the shell's monochrome line set, tinted to the palette;
 * theme icons are only a fallback.
 */
class ChromeToolbar : public QWidget
{
    Q_OBJECT

public:
    explicit ChromeToolbar(QWidget *parent = nullptr);

    void clearDocument();
    void setPdfView(PdfView *reader, QAction *showSidebarAction);

    /** The button wired to the named document action; test and tooling hook. */
    QToolButton *button(const QString &actionName) const;

protected:
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    ChromeToolButton *addActionButton(QHBoxLayout *layout, const QString &actionName, const QString &iconName, const QString &themeFallback);
    void setButtonAction(const QString &actionName, QAction *action);
    void clearButtonActions();
    void clearPdfMode();
    void updatePdfPageControls(int currentPage, int pageCount);
    void updatePdfZoomText(qreal zoomFactor, bool fitWidthMode);
    void refreshIcons();

    QHash<QString, ChromeToolButton *> m_buttons;
    QHash<QString, QString> m_iconNames;  // action name → shell icon name
    QHash<QString, QString> m_themeIcons; // action name → theme fallback
    QToolButton *m_zoomLevelButton = nullptr;
    ChromeToolButton *m_overflowButton = nullptr;
    QMenu *m_overflowMenu = nullptr;
    QPointer<PdfView> m_pdfView;
    QList<QMetaObject::Connection> m_pdfConnections;
    QAction *m_pdfPreviousPageAction = nullptr;
    QAction *m_pdfNextPageAction = nullptr;
    QAction *m_pdfZoomOutAction = nullptr;
    QAction *m_pdfZoomInAction = nullptr;
    QWidget *m_pdfPageWidget = nullptr;
    QSpinBox *m_pdfPageSpinBox = nullptr;
    QLabel *m_pdfPageCountLabel = nullptr;
    int m_pageWidgetSlot = 0;
    QHBoxLayout *m_layout = nullptr;
};

#endif
