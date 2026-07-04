/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_CHROMETABWIDGET_H
#define PAPERLIBRARY_CHROMETABWIDGET_H

#include <QIcon>
#include <QWidget>

class ChromeTabBar;
class ChromeTabStrip;
class ChromeToolbar;
class QStackedWidget;
class QTabBar;

/**
 * The shell's central widget, stacked in Chrome's order: the tab strip on
 * top, the one slim toolbar row directly under it, the tab pages below.
 * QTabWidget cannot host a row between its bar and its pages (its geometry
 * management is closed), so this composite owns a ChromeTabBar, a
 * ChromeToolbar and a QStackedWidget directly while exposing the small
 * QTabWidget-shaped API the shell and its tests use.
 */
class ChromeTabWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ChromeTabWidget(QWidget *parent = nullptr);

    ChromeTabBar *chromeTabBar() const;
    QTabBar *tabBar() const;
    ChromeToolbar *toolbar() const;

    /** Chrome's leading inset before the first tab (clears the traffic lights). */
    void setLeadingInset(int inset);
    /** Collapse the leading inset in fullscreen, restore it on the way out. */
    void setFullscreen(bool fullscreen);
    /** Grow the strip row to match the native titlebar height. */
    void setStripHeight(int height);
    /** Bare-strip presses drag / double-clicks zoom only when it is the titlebar. */
    void setWindowDragEnabled(bool enabled);

    int addTab(QWidget *page, const QString &label);
    void insertTab(int index, QWidget *page, const QString &label);
    void removeTab(int index);

    int count() const;
    int currentIndex() const;
    void setCurrentIndex(int index);
    QWidget *currentWidget() const;
    QWidget *widget(int index) const;

    void setTabText(int index, const QString &text);
    void setTabToolTip(int index, const QString &toolTip);
    void setTabIcon(int index, const QIcon &icon);
    QIcon tabIcon(int index) const;

Q_SIGNALS:
    void currentChanged(int index);
    void tabCloseRequested(int index);
    void tabCountChanged(int count);

private:
    ChromeTabStrip *m_strip;
    ChromeTabBar *m_bar;
    ChromeToolbar *m_toolbar;
    QStackedWidget *m_stack;
};

#endif
