/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "chrometabwidget.h"

#include "chrometabbar.h"
#include "chrometoolbar.h"

#include <QStackedWidget>
#include <QVBoxLayout>

ChromeTabWidget::ChromeTabWidget(QWidget *parent)
    : QWidget(parent)
{
    // The strip owns the tab bar and adds Chrome's leading inset; the bar
    // itself is unchanged and reached through the strip.
    m_strip = new ChromeTabStrip(this);
    m_bar = m_strip->tabBar();
    m_bar->setMovable(true);
    m_toolbar = new ChromeToolbar(this);
    m_stack = new QStackedWidget(this);
    m_stack->setFrameShape(QFrame::NoFrame);

    QVBoxLayout *const layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_strip);
    layout->addWidget(m_toolbar);
    layout->addWidget(m_stack, 1);

    connect(m_bar, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < m_stack->count()) {
            m_stack->setCurrentIndex(index);
        }
        Q_EMIT currentChanged(index);
    });
    connect(m_bar, &QTabBar::tabCloseRequested, this, &ChromeTabWidget::tabCloseRequested);

    // QTabBar reorders only its own tabs on drag; mirror the move into the
    // stack so bar indexes keep addressing the right pages
    connect(m_bar, &QTabBar::tabMoved, this, [this](int from, int to) {
        QWidget *const page = m_stack->widget(from);
        const QSignalBlocker blocker(m_stack);
        m_stack->removeWidget(page);
        m_stack->insertWidget(to, page);
        m_stack->setCurrentIndex(m_bar->currentIndex());
    });
}

ChromeTabBar *ChromeTabWidget::chromeTabBar() const
{
    return m_bar;
}

QTabBar *ChromeTabWidget::tabBar() const
{
    return m_bar;
}

ChromeToolbar *ChromeTabWidget::toolbar() const
{
    return m_toolbar;
}

void ChromeTabWidget::setLeadingInset(int inset)
{
    m_strip->setLeadingInset(inset);
}

void ChromeTabWidget::setFullscreen(bool fullscreen)
{
    m_strip->setFullscreen(fullscreen);
}

void ChromeTabWidget::setStripHeight(int height)
{
    m_strip->setStripHeight(height);
}

void ChromeTabWidget::setWindowDragEnabled(bool enabled)
{
    m_strip->setWindowDragEnabled(enabled);
}

int ChromeTabWidget::addTab(QWidget *page, const QString &label)
{
    const int index = count();
    insertTab(index, page, label);
    return index;
}

// Tab labels are literal document names, never mnemonic templates: a
// file called "AT&T Report.pdf" means a real ampersand. Escape it as
// "&&" on the way into the QTabBar, so the strip's display-text pass
// (which strips accelerator markers) hands it back intact.
static QString escapedLabel(QString label)
{
    return label.replace(QLatin1Char('&'), QStringLiteral("&&"));
}

void ChromeTabWidget::insertTab(int index, QWidget *page, const QString &label)
{
    // Page first: the bar's insertion may emit currentChanged, whose
    // handler expects the stack to know the page already
    m_stack->insertWidget(index, page);
    m_bar->insertTab(index, escapedLabel(label));
    Q_EMIT tabCountChanged(count());
}

void ChromeTabWidget::removeTab(int index)
{
    // Stack first (QTabWidget's order too): when the bar then picks the
    // next current tab and emits currentChanged, stack indexes already
    // line up with bar indexes again
    QWidget *const page = m_stack->widget(index);
    if (page) {
        m_stack->removeWidget(page);
        page->setParent(nullptr);
        page->hide();
    }
    m_bar->removeTab(index);
    Q_EMIT tabCountChanged(count());
}

int ChromeTabWidget::count() const
{
    return m_bar->count();
}

int ChromeTabWidget::currentIndex() const
{
    return m_bar->currentIndex();
}

void ChromeTabWidget::setCurrentIndex(int index)
{
    m_bar->setCurrentIndex(index);
}

QWidget *ChromeTabWidget::currentWidget() const
{
    return m_stack->currentWidget();
}

QWidget *ChromeTabWidget::widget(int index) const
{
    return m_stack->widget(index);
}

void ChromeTabWidget::setTabText(int index, const QString &text)
{
    m_bar->setTabText(index, escapedLabel(text));
}

void ChromeTabWidget::setTabToolTip(int index, const QString &toolTip)
{
    m_bar->setTabToolTip(index, toolTip);
}

void ChromeTabWidget::setTabIcon(int index, const QIcon &icon)
{
    m_bar->setTabIcon(index, icon);
}

QIcon ChromeTabWidget::tabIcon(int index) const
{
    return m_bar->tabIcon(index);
}
