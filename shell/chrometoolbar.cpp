/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "chrometoolbar.h"

#include "chromecolors.h"
#include "pdfview.h"

#include <KLocalizedString>

#include <QActionEvent>
#include <QAbstractSpinBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>

static constexpr int RowHeight = 40;   // Chrome's slim toolbar row
static constexpr int ButtonHit = 28;   // icon-button hit target
static constexpr int ButtonIcon = 20;

/** The shell's monochrome line icon, tinted to the palette's text color. */
static QIcon tintedIcon(const QString &name, const QString &themeFallback, const QPalette &palette)
{
    const QIcon source(QStringLiteral(":/shell/icons/%1.svg").arg(name));
    QColor color = ChromeColors::toolbarText(palette);
    color.setAlphaF(0.8);

    QIcon result;
    bool any = false;
    const int sizes[] = {16, 20, 24, 32, 40, 48};
    for (const int size : sizes) {
        QPixmap pixmap = source.pixmap(QSize(size, size));
        if (pixmap.isNull()) {
            continue;
        }
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), color);
        painter.end();
        result.addPixmap(pixmap);
        any = true;
    }
    if (!any) {
        return QIcon::fromTheme(themeFallback);
    }
    return result;
}

static QString flatButtonStyle()
{
    // Chrome-flat: no bezel, a quiet 4px-rounded wash on hover, a slightly
    // stronger one when pressed or latched (the checked tool)
    return QStringLiteral(
        "QToolButton { border: none; background: transparent; border-radius: 4px; padding: 4px; }"
        "QToolButton:hover { background: rgba(127, 127, 127, 55); }"
        "QToolButton:pressed { background: rgba(127, 127, 127, 90); }"
        "QToolButton:checked { background: rgba(127, 127, 127, 75); }"
        "QToolButton::menu-indicator { image: none; }");
}

/**
 * A flat button that presents a document action in the shell chrome: it
 * mirrors the action's enabled/checked state and forwards clicks to
 * QAction::trigger(), while its icon stays the shell's own.
 */
class ChromeToolButton : public QToolButton
{
    Q_OBJECT

public:
    explicit ChromeToolButton(QWidget *parent)
        : QToolButton(parent)
    {
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(ButtonHit, ButtonHit); // Chrome's icon-button hit target
        setIconSize(QSize(ButtonIcon, ButtonIcon));
        setStyleSheet(flatButtonStyle());
        connect(this, &QAbstractButton::clicked, this, [this]() {
            if (m_action) {
                m_action->trigger();
            }
        });
    }

    void setSourceAction(QAction *action)
    {
        for (const QMetaObject::Connection &connection : std::as_const(m_connections)) {
            disconnect(connection);
        }
        m_connections.clear();
        m_action = action;
        setVisible(action != nullptr);
        if (!action) {
            return;
        }
        syncFromAction();
        m_connections << connect(action, &QAction::changed, this, &ChromeToolButton::syncFromAction);
        m_connections << connect(action, &QObject::destroyed, this, [this]() {
            m_action = nullptr;
            setVisible(false);
        });
    }

    QAction *sourceAction() const
    {
        return m_action;
    }

private:
    void syncFromAction()
    {
        setEnabled(m_action->isEnabled());
        setCheckable(m_action->isCheckable());
        setChecked(m_action->isChecked());
        QString tip = m_action->toolTip().isEmpty() ? m_action->text() : m_action->toolTip();
        tip.remove(QLatin1Char('&'));
        const QKeySequence shortcut = m_action->shortcut();
        if (!shortcut.isEmpty()) {
            tip += QStringLiteral(" (%1)").arg(shortcut.toString(QKeySequence::NativeText));
        }
        setToolTip(tip);
    }

    QAction *m_action = nullptr;
    QList<QMetaObject::Connection> m_connections;
};

ChromeToolbar::ChromeToolbar(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(RowHeight);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window); // the surface the active tab joins

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(8, 0, 8, 0); // fixed-height controls center themselves
    m_layout->setSpacing(4);

    addActionButton(m_layout, QStringLiteral("show_leftpanel"), QStringLiteral("chrome-sidebar"), QStringLiteral("sidebar-expand"));
    addActionButton(m_layout, QStringLiteral("go_document_back"), QStringLiteral("chrome-back"), QStringLiteral("go-previous"));
    addActionButton(m_layout, QStringLiteral("go_document_forward"), QStringLiteral("chrome-forward"), QStringLiteral("go-next"));

    m_pageWidgetSlot = m_layout->count(); // the shell-native PDF page control goes here
    m_layout->addSpacing(4);
    m_layout->addStretch();

    m_pdfPageWidget = new QWidget(this);
    auto *pdfPageLayout = new QHBoxLayout(m_pdfPageWidget);
    pdfPageLayout->setContentsMargins(0, 0, 0, 0);
    pdfPageLayout->setSpacing(4);
    m_pdfPageSpinBox = new QSpinBox(m_pdfPageWidget);
    m_pdfPageSpinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_pdfPageSpinBox->setMinimum(1);
    m_pdfPageSpinBox->setMinimumWidth(58);
    m_pdfPageSpinBox->setFixedHeight(ButtonHit);
    m_pdfPageSpinBox->setToolTip(i18n("Page"));
    pdfPageLayout->addWidget(m_pdfPageSpinBox);
    m_pdfPageCountLabel = new QLabel(m_pdfPageWidget);
    m_pdfPageCountLabel->setMinimumWidth(40);
    pdfPageLayout->addWidget(m_pdfPageCountLabel);
    m_pdfPageWidget->hide();
    connect(m_pdfPageSpinBox, qOverload<int>(&QSpinBox::valueChanged), this, [this](int page) {
        if (m_pdfView) {
            m_pdfView->goToPageOneBased(page);
        }
    });

    addActionButton(m_layout, QStringLiteral("view_zoom_out"), QStringLiteral("chrome-zoom-out"), QStringLiteral("zoom-out"));

    // The zoom level: a flat text button whose label tracks the live PDF zoom.
    m_zoomLevelButton = new QToolButton(this);
    m_zoomLevelButton->setAutoRaise(true);
    m_zoomLevelButton->setFocusPolicy(Qt::NoFocus);
    m_zoomLevelButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_zoomLevelButton->setPopupMode(QToolButton::InstantPopup);
    m_zoomLevelButton->setStyleSheet(flatButtonStyle());
    m_zoomLevelButton->setMinimumWidth(56);
    m_zoomLevelButton->setFixedHeight(ButtonHit); // rides centered like the icon buttons
    m_layout->addWidget(m_zoomLevelButton);

    addActionButton(m_layout, QStringLiteral("view_zoom_in"), QStringLiteral("chrome-zoom-in"), QStringLiteral("zoom-in"));

    m_layout->addSpacing(8);
    addActionButton(m_layout, QStringLiteral("mouse_drag"), QStringLiteral("chrome-browse"), QStringLiteral("transform-browse"));
    addActionButton(m_layout, QStringLiteral("mouse_textselect"), QStringLiteral("chrome-textselect"), QStringLiteral("edit-select-text"));

    m_layout->addSpacing(8);
    addActionButton(m_layout, QStringLiteral("edit_find"), QStringLiteral("chrome-search"), QStringLiteral("edit-find"));
    addActionButton(m_layout, QStringLiteral("pdf_related_papers"), QStringLiteral("chrome-related"), QStringLiteral("view-filter"));
    addActionButton(m_layout, QStringLiteral("pdf_ai_navigation"), QStringLiteral("chrome-ai-navigation"), QStringLiteral("tools-wizard"));

    // Overflow "⋮": every remaining useful document action lives in its menu.
    m_overflowButton = new ChromeToolButton(this);
    m_overflowButton->setPopupMode(QToolButton::InstantPopup);
    m_overflowButton->setToolTip(i18n("More actions"));
    m_overflowMenu = new QMenu(this);
    m_overflowButton->setMenu(m_overflowMenu);
    m_layout->addWidget(m_overflowButton);

    m_pdfPreviousPageAction = new QAction(i18n("Previous Page"), this);
    connect(m_pdfPreviousPageAction, &QAction::triggered, this, [this]() {
        if (m_pdfView) {
            m_pdfView->previousPage();
        }
    });
    m_pdfNextPageAction = new QAction(i18n("Next Page"), this);
    connect(m_pdfNextPageAction, &QAction::triggered, this, [this]() {
        if (m_pdfView) {
            m_pdfView->nextPage();
        }
    });
    m_pdfZoomOutAction = new QAction(i18n("Zoom Out"), this);
    connect(m_pdfZoomOutAction, &QAction::triggered, this, [this]() {
        if (m_pdfView) {
            m_pdfView->zoomOut();
        }
    });
    m_pdfZoomInAction = new QAction(i18n("Zoom In"), this);
    connect(m_pdfZoomInAction, &QAction::triggered, this, [this]() {
        if (m_pdfView) {
            m_pdfView->zoomIn();
        }
    });

    refreshIcons();
    hide(); // shown once a PDF tab hands us a reader
}

ChromeToolButton *ChromeToolbar::addActionButton(QHBoxLayout *layout, const QString &actionName, const QString &iconName, const QString &themeFallback)
{
    ChromeToolButton *const button = new ChromeToolButton(this);
    layout->addWidget(button);
    m_buttons.insert(actionName, button);
    m_iconNames.insert(actionName, iconName);
    m_themeIcons.insert(actionName, themeFallback);
    return button;
}

QToolButton *ChromeToolbar::button(const QString &actionName) const
{
    return m_buttons.value(actionName);
}

void ChromeToolbar::setButtonAction(const QString &actionName, QAction *action)
{
    if (ChromeToolButton *const toolButton = m_buttons.value(actionName)) {
        toolButton->setSourceAction(action);
    }
}

void ChromeToolbar::clearButtonActions()
{
    for (ChromeToolButton *const toolButton : std::as_const(m_buttons)) {
        toolButton->setSourceAction(nullptr);
    }
}

void ChromeToolbar::refreshIcons()
{
    for (auto it = m_buttons.cbegin(); it != m_buttons.cend(); ++it) {
        it.value()->setIcon(tintedIcon(m_iconNames.value(it.key()), m_themeIcons.value(it.key()), palette()));
    }
    if (m_overflowButton) {
        m_overflowButton->setIcon(tintedIcon(QStringLiteral("chrome-more"), QStringLiteral("overflow-menu"), palette()));
    }
}

void ChromeToolbar::clearPdfMode()
{
    for (const QMetaObject::Connection &connection : std::as_const(m_pdfConnections)) {
        disconnect(connection);
    }
    m_pdfConnections.clear();
    m_pdfView = nullptr;
    if (m_pdfPageWidget) {
        m_layout->removeWidget(m_pdfPageWidget);
        m_pdfPageWidget->hide();
    }
}

void ChromeToolbar::clearDocument()
{
    clearPdfMode();
    clearButtonActions();
    m_zoomLevelButton->setVisible(false);
    m_overflowMenu->clear();
    m_overflowButton->setVisible(false);
    hide();
}

void ChromeToolbar::setNavigationAction(QAction *showSidebarAction)
{
    clearPdfMode();
    clearButtonActions();

    setButtonAction(QStringLiteral("show_leftpanel"), showSidebarAction);
    m_zoomLevelButton->setVisible(false);
    m_overflowMenu->clear();
    m_overflowButton->setVisible(false);
    show();
}

void ChromeToolbar::setPdfView(PdfView *reader, QAction *showSidebarAction)
{
    clearPdfMode();
    clearButtonActions();

    if (!reader) {
        m_zoomLevelButton->setVisible(false);
        m_overflowMenu->clear();
        m_overflowButton->setVisible(false);
        hide();
        return;
    }

    m_pdfView = reader;

    setButtonAction(QStringLiteral("show_leftpanel"), showSidebarAction);
    setButtonAction(QStringLiteral("go_document_back"), m_pdfPreviousPageAction);
    setButtonAction(QStringLiteral("go_document_forward"), m_pdfNextPageAction);
    setButtonAction(QStringLiteral("view_zoom_out"), m_pdfZoomOutAction);
    setButtonAction(QStringLiteral("view_zoom_in"), m_pdfZoomInAction);
    setButtonAction(QStringLiteral("edit_find"), reader->findAction());
    setButtonAction(QStringLiteral("pdf_related_papers"), reader->relatedPapersAction());
    setButtonAction(QStringLiteral("pdf_ai_navigation"), reader->aiNavigationAction());

    m_layout->insertWidget(m_pageWidgetSlot, m_pdfPageWidget, 0, Qt::AlignVCenter);
    m_pdfPageWidget->show();
    updatePdfPageControls(reader->currentPageOneBased(), reader->pageCount());
    updatePdfZoomText(reader->zoomFactor(), reader->fitWidthMode());

    m_zoomLevelButton->setMenu(nullptr);
    m_zoomLevelButton->setVisible(true);
    m_zoomLevelButton->setToolTip(i18n("Zoom level"));

    m_overflowMenu->clear();
    QAction *const fitWidthAction = m_overflowMenu->addAction(i18n("Fit Width"));
    connect(fitWidthAction, &QAction::triggered, reader, &PdfView::fitToWidth);
    m_overflowButton->setVisible(!m_overflowMenu->isEmpty());

    m_pdfConnections << connect(reader, &PdfView::pageStateChanged, this, &ChromeToolbar::updatePdfPageControls);
    m_pdfConnections << connect(reader, &PdfView::zoomStateChanged, this, &ChromeToolbar::updatePdfZoomText);

    show();
}

void ChromeToolbar::updatePdfPageControls(int currentPage, int pageCount)
{
    const bool hasPages = pageCount > 0;
    m_pdfPreviousPageAction->setEnabled(hasPages && currentPage > 1);
    m_pdfNextPageAction->setEnabled(hasPages && currentPage < pageCount);
    if (!m_pdfPageSpinBox || !m_pdfPageCountLabel) {
        return;
    }

    const QSignalBlocker blocker(m_pdfPageSpinBox);
    m_pdfPageSpinBox->setEnabled(hasPages);
    m_pdfPageSpinBox->setMaximum(qMax(1, pageCount));
    m_pdfPageSpinBox->setValue(qBound(1, currentPage, qMax(1, pageCount)));
    m_pdfPageCountLabel->setText(hasPages ? i18n("/ %1", pageCount) : QStringLiteral("/ 0"));
}

void ChromeToolbar::updatePdfZoomText(qreal zoomFactor, bool fitWidthMode)
{
    const int percent = qRound(zoomFactor * 100.0);
    m_zoomLevelButton->setText(fitWidthMode ? i18n("Fit %1%", percent) : i18n("%1%", percent));
    m_pdfZoomOutAction->setEnabled(percent > 10);
    m_pdfZoomInAction->setEnabled(percent < 800);
}

bool ChromeToolbar::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    Q_UNUSED(event);
    return QWidget::eventFilter(watched, event);
}

void ChromeToolbar::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(rect(), ChromeColors::toolbar(palette()));

    // The only divider between toolbar and content: a hairline, nothing
    // heavier (the strip above joins this row without any line at all)
    QColor hairline = ChromeColors::toolbarText(palette());
    hairline.setAlphaF(0.12);
    painter.setPen(QPen(hairline, 1.0));
    painter.drawLine(QPointF(0, height() - 0.5), QPointF(width(), height() - 0.5));
}

void ChromeToolbar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        refreshIcons(); // stay palette-tinted across light/dark flips
    }
}

#include "chrometoolbar.moc"
