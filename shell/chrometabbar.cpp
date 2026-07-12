/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "chrometabbar.h"

#include "chromecolors.h"

#include <KAcceleratorManager>
#include <KConfigGroup>
#include <KLocalizedString>

#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVariantAnimation>
#include <QWindow>

#if defined(Q_OS_MACOS)
#include "macostitlebar.h"
#endif

// Chromium tab geometry, current non-touch GM3 defaults.
static constexpr int StripHeight = 35; // kTabHeight = 34 + kTabstripToolbarOverlap
static constexpr int TabTopInset = 6;  // kTabStripPadding
static constexpr int TabTopRadius = 10;
static constexpr int TabBottomRadius = 12;   // the active tab's outward base curve
static constexpr int ToolbarOverlap = 1;
static constexpr int TabMinWidth = 52;       // Chrome's floor: little more than the icon
static constexpr int TabMaxWidth = 240;      // Chrome's full tab width
static constexpr int CloseMinTabWidth = 100; // below this, inactive tabs drop the close glyph
static constexpr int SeparatorHeight = 16;
static constexpr int SeparatorThickness = 2;
static constexpr int SeparatorCornerRadius = 1;
static constexpr int TabPadding = 10;
static constexpr int IconSize = 16;
static constexpr int CloseHit = 16; // close glyph hit rect, and the "+" glyph box
static constexpr int NewTabGap = 6;
static constexpr int HoverFadeMs = 150; // Chrome's quick hover wash fade

static QColor withAlpha(const QColor &color, double alpha)
{
    QColor result = color;
    result.setAlphaF(alpha);
    return result;
}

static QPainterPath topRoundedTabPath(const QRectF &rect)
{
    QPainterPath path;
    path.addRoundedRect(rect, TabTopRadius, TabTopRadius);
    return path;
}

/**
 * Chrome's active-tab silhouette as one path: rounded top corners,
 * straight sides, and at the base each side sweeps *outward* through a
 * concave quarter-curve — the flare that joins the tab smoothly into the
 * toolbar surface below. @p rect is the tab's core; the flare extends
 * TabBottomRadius beyond it on each side.
 */
static QPainterPath activeTabPath(const QRectF &rect)
{
    constexpr qreal Kappa = 0.5522847498307936;
    const qreal top = rect.top() + TabTopInset;
    const qreal bottom = rect.bottom();
    const qreal tabBottom = bottom - ToolbarOverlap;
    const qreal left = rect.left();
    const qreal right = rect.right();

    QPainterPath path;
    path.moveTo(left - TabBottomRadius, bottom);
    path.lineTo(left - TabBottomRadius, tabBottom);
    path.cubicTo(QPointF(left - TabBottomRadius + Kappa * TabBottomRadius, tabBottom),
                 QPointF(left, tabBottom - TabBottomRadius + Kappa * TabBottomRadius),
                 QPointF(left, tabBottom - TabBottomRadius));
    path.lineTo(left, top + TabTopRadius);
    path.arcTo(QRectF(left, top, 2.0 * TabTopRadius, 2.0 * TabTopRadius), 180, -90);
    path.lineTo(right - TabTopRadius, top);
    path.arcTo(QRectF(right - 2.0 * TabTopRadius, top, 2.0 * TabTopRadius, 2.0 * TabTopRadius), 90, -90);
    path.lineTo(right, tabBottom - TabBottomRadius);
    path.cubicTo(QPointF(right, tabBottom - TabBottomRadius + Kappa * TabBottomRadius),
                 QPointF(right + TabBottomRadius - Kappa * TabBottomRadius, tabBottom),
                 QPointF(right + TabBottomRadius, tabBottom));
    path.lineTo(right + TabBottomRadius, bottom);
    path.closeSubpath();
    return path;
}

/**
 * Start a native window move from a bare-strip press — Qt's preferred
 * startSystemMove(), so the OS runs the drag loop and snapping. A no-op
 * (returns false) when there is no native window yet.
 */
static bool beginWindowDrag(QWidget *anchor)
{
    QWidget *const top = anchor ? anchor->window() : nullptr;
    QWindow *const handle = top ? top->windowHandle() : nullptr;
    return handle && handle->startSystemMove();
}

/**
 * The empty-titlebar double-click action. On macOS this honors the user's
 * AppleActionOnDoubleClick preference (zoom / minimize / none); elsewhere
 * there is no titlebar strip to double-click, so it does nothing.
 */
static void titlebarDoubleClick(QWidget *anchor)
{
#if defined(Q_OS_MACOS)
    QWidget *const top = anchor ? anchor->window() : nullptr;
    QWindow *const handle = top ? top->windowHandle() : nullptr;
    if (handle) {
        MacTitlebar::performDoubleClickAction(handle);
    }
#else
    Q_UNUSED(anchor);
#endif
}

bool chromeTitlebarTabsEnabled(const KConfigGroup &general)
{
    return general.readEntry("TitlebarTabs", true);
}

ChromeTabBar::ChromeTabBar(QWidget *parent)
    : QTabBar(parent)
    , m_stripHeight(StripHeight)
{
    setMouseTracking(true); // the close glyph appears on hover
    setDocumentMode(true);  // keeps tabs left-aligned (the mac style centers them otherwise)
    setDrawBase(false);
    setExpanding(false);
    setElideMode(Qt::ElideRight);
    setUsesScrollButtons(false); // Chrome squishes tabs, it never scrolls the strip

    // Fill the strip row vertically instead of sticking to QTabBar's fixed
    // size-hint height: the row grows to the native titlebar in titlebar-tabs
    // mode, and the bar (and its full-height flare) must grow with it.
    QSizePolicy policy = sizePolicy();
    policy.setVerticalPolicy(QSizePolicy::Ignored);
    setSizePolicy(policy);

    // The shell rebuilds its XMLGui on every tab switch, and each rebuild
    // ends in KAcceleratorManager walking the whole window and weaving
    // '&' mnemonics into tab texts — which this hand-painted strip would
    // show literally ("&PaperLibrary"). Opt the strip out entirely;
    // displayText() additionally strips whatever may still get through.
    KAcceleratorManager::setNoAccel(this);

    m_newTabButton = new QToolButton(this);
    m_newTabButton->setAutoRaise(true);
    m_newTabButton->setFocusPolicy(Qt::NoFocus);
    m_newTabButton->setFixedSize(CloseHit + 8, CloseHit + 8);
    m_newTabButton->setCursor(Qt::ArrowCursor);
    // Chrome-flat: no native bezel, just a quiet *circular* hover wash
    m_newTabButton->setStyleSheet(QStringLiteral("QToolButton { border: none; background: transparent; }"
                                                 "QToolButton:hover { background: rgba(127, 127, 127, 60); border-radius: %1px; }"
                                                 "QToolButton:pressed { background: rgba(127, 127, 127, 90); border-radius: %1px; }")
                                      .arg(m_newTabButton->height() / 2));
    refreshNewTabIcon();

    // Chrome fades the hover wash in and out (~150ms); two channels so the
    // tab the pointer just left fades out while the new one fades in
    m_hoverAnimation = new QVariantAnimation(this);
    m_hoverAnimation->setDuration(HoverFadeMs);
    m_hoverAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_hoverAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_hoverAlpha = value.toReal();
        update();
    });
    m_fadeAnimation = new QVariantAnimation(this);
    m_fadeAnimation->setDuration(HoverFadeMs);
    m_fadeAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_fadeAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_fadeAlpha = value.toReal();
        update();
    });
    connect(m_fadeAnimation, &QVariantAnimation::finished, this, [this]() { m_fadeTab = -1; });
}

void ChromeTabBar::setNewTabAction(QAction *action)
{
    m_newTabButton->setDefaultAction(action);
    // The action brings its own icon and text; the strip wants only the
    // painted plus glyph and the tooltip
    m_newTabButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    refreshNewTabIcon();
}

/** A minimal monochrome plus, palette-tinted, in place of a theme icon. */
void ChromeTabBar::refreshNewTabIcon()
{
    if (!m_newTabButton) {
        return;
    }
    const qreal dpr = devicePixelRatioF();
    QPixmap pixmap(QSize(IconSize, IconSize) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(withAlpha(ChromeColors::inactiveTabText(palette()), 0.80), 1.6);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    const QPointF center(IconSize / 2.0, IconSize / 2.0);
    const qreal arm = 4.5;
    painter.drawLine(center - QPointF(arm, 0), center + QPointF(arm, 0));
    painter.drawLine(center - QPointF(0, arm), center + QPointF(0, arm));
    painter.end();

    m_newTabButton->setIcon(QIcon(pixmap));
}

QString ChromeTabBar::displayText(int index) const
{
    // Removes a single-'&' accelerator marker and displays the "&&"
    // escape (a real ampersand in a document name) as '&'
    const QString text = KLocalizedString::removeAcceleratorMarker(tabText(index));
    if (text.trimmed().isEmpty()) {
        return i18nc("@title:tab fallback for an untitled new tab", "New Tab");
    }
    return text;
}

/** The width each of @p tabCount tabs gets: equal shares of the strip, floor to cap. */
int ChromeTabBar::uniformTabWidth(int tabCount) const
{
    // Chrome sizes every tab equally — never text-sized: full width up to
    // TabMaxWidth, squeezed uniformly as tabs accumulate, stopping at
    // TabMinWidth. Room is held back so the "+" button keeps its seat.
    const int reserve = m_newTabButton ? m_newTabButton->width() + 2 * NewTabGap : 0;
    const int available = qMax(0, width() - reserve);
    const int uniform = tabCount > 0 ? available / tabCount : TabMaxWidth;
    return qBound(TabMinWidth, uniform, TabMaxWidth);
}

QSize ChromeTabBar::tabSizeHint(int index) const
{
    Q_UNUSED(index);
    if (m_frozenTabWidth > 0) {
        return QSize(m_frozenTabWidth, m_stripHeight);
    }
    return QSize(uniformTabWidth(count()), m_stripHeight);
}

/** Force QTabBar to re-ask tabSizeHint(). updateGeometry() alone leaves its rects cached,
    and QTabBar exposes no other way to mark the strip's layout dirty. */
void ChromeTabBar::relayoutTabs()
{
    if (count() > 0) {
        setTabText(0, tabText(0)); // cheapest public call that refreshes the layout
    }
    updateGeometry();
    updateNewTabButton();
    update();
}

/** Let the tabs resume filling the strip, and lay them out at the new width. */
void ChromeTabBar::thawTabWidths()
{
    if (m_frozenTabWidth == 0) {
        return;
    }
    m_frozenTabWidth = 0;
    relayoutTabs();
}

void ChromeTabBar::tabInserted(int index)
{
    QTabBar::tabInserted(index);
    // A frozen width was sized for a strip with fewer tabs; keeping it would overflow.
    thawTabWidths();
}

void ChromeTabBar::tabRemoved(int index)
{
    QTabBar::tabRemoved(index);
    // Chrome holds the tab width while you close a run of tabs, so the next [x] stays
    // under the pointer; the strip only reflows once the pointer leaves. Freeze at the
    // width the tabs had a moment ago -- before this removal -- and keep that width for
    // the rest of the streak.
    if (m_pointerInStrip && m_frozenTabWidth == 0 && count() > 0) {
        m_frozenTabWidth = uniformTabWidth(count() + 1);
        // QTabBar::removeTab() lays the strip out *before* it calls tabRemoved(), so the
        // tabs have already grown by the time we get here; the width must be re-applied.
        relayoutTabs();
        return;
    } else if (count() == 0) {
        m_frozenTabWidth = 0;
    }
    updateNewTabButton();
}

QSize ChromeTabBar::minimumTabSizeHint(int index) const
{
    Q_UNUSED(index);
    return QSize(TabMinWidth, m_stripHeight);
}

void ChromeTabBar::setStripHeight(int height)
{
    if (height <= 0 || height == m_stripHeight) {
        return;
    }
    m_stripHeight = height;
    updateGeometry();
    updateNewTabButton(); // recenter the "+" in the taller row
    update();
}

QRect ChromeTabBar::newTabButtonRect() const
{
    return m_newTabButton ? m_newTabButton->geometry() : QRect();
}

void ChromeTabBar::setWindowDragEnabled(bool enabled)
{
    m_windowDragEnabled = enabled;
}

ChromeTabBar::StripHit ChromeTabBar::hitTest(const QPoint &pos) const
{
    if (m_newTabButton && m_newTabButton->isVisible() && m_newTabButton->geometry().contains(pos)) {
        return StripHit::NewTabButton;
    }
    if (tabAt(pos) >= 0) {
        return StripHit::Tab;
    }
    return StripHit::Empty;
}

bool ChromeTabBar::closeGlyphVisible(int index) const
{
    if (index == currentIndex()) {
        return true; // the active tab keeps its close glyph at any width
    }
    // Chrome drops the glyph from squished inactive tabs, even hovered
    return index == m_hoveredTab && tabRect(index).width() >= CloseMinTabWidth;
}

QRect ChromeTabBar::closeButtonRect(int index) const
{
    if (index < 0 || index >= count() || !closeGlyphVisible(index)) {
        return QRect();
    }
    const QRect tab = tabRect(index);
    return QRect(tab.right() - TabPadding - CloseHit, tab.top() + (tab.height() - CloseHit) / 2, CloseHit, CloseHit);
}

void ChromeTabBar::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);

    // The strip is a recessed shade of the app body. The active tab is
    // painted in the toolbar/body exact color, with no bottom edge, so the
    // surfaces join.
    painter.fillRect(rect(), ChromeColors::frameActive(palette()));

    // Inactive tabs first, the active tab last: its bottom flare sweeps
    // outward over the neighbors' base and must never be clipped by them
    const int current = currentIndex();
    for (int i = 0; i < count(); ++i) {
        if (i != current) {
            paintTab(painter, i);
        }
    }
    if (current >= 0 && current < count()) {
        paintTab(painter, current);
    }
}

void ChromeTabBar::paintTab(QPainter &painter, int i)
{
    const QRect tab = tabRect(i);
    if (tab.isNull()) {
        return;
    }
    const QPalette pal = palette();
    const bool active = i == currentIndex();
    const bool hovered = i == m_hoveredTab;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (active) {
        const QColor activeColor = ChromeColors::toolbar(pal);
        const QRectF tabRect(tab.left(), tab.top(), tab.width(), tab.height());
        painter.fillRect(QRectF(tab.left() - TabBottomRadius, tab.bottom() - ToolbarOverlap, tab.width() + 2.0 * TabBottomRadius, ToolbarOverlap + 1.0), activeColor);
        painter.fillPath(activeTabPath(tabRect), activeColor);

        QPen indicatorPen(ChromeColors::accent(pal), 2.0);
        indicatorPen.setCapStyle(Qt::RoundCap);
        painter.setPen(indicatorPen);
        const qreal indicatorY = tab.bottom() - ToolbarOverlap - 1.0;
        painter.drawLine(QPointF(tab.left() + TabTopRadius, indicatorY), QPointF(tab.right() - TabTopRadius, indicatorY));
    } else {
        // Hover: a soft rounded-rect wash inside the tab bounds, its alpha
        // animated in on the hovered tab and out on the one just left
        qreal wash = 0.0;
        if (hovered) {
            wash = m_hoverAlpha;
        } else if (i == m_fadeTab) {
            wash = m_fadeAlpha;
        }
        if (wash > 0.0) {
            painter.fillPath(topRoundedTabPath(QRectF(tab).adjusted(2.0, TabTopInset, -2.0, -ToolbarOverlap)),
                             withAlpha(ChromeColors::inactiveTabHover(pal), wash));
        }
    }

    // Hairline separator between two adjacent tabs, short and vertically
    // centered, only when neither side is active or hovered (Chrome hides
    // the separators around the tab shapes that stand out)
    const int current = currentIndex();
    if (!active && i + 1 < count() && i + 1 != current && !hovered && i + 1 != m_hoveredTab) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(ChromeColors::tabSeparator(pal));
        const qreal x = tab.right();
        const qreal top = tab.top() + (tab.height() - ToolbarOverlap - SeparatorHeight) / 2.0;
        painter.drawRoundedRect(QRectF(x, top, SeparatorThickness, SeparatorHeight), SeparatorCornerRadius, SeparatorCornerRadius);
    }

    // Format icon slot: the document's mime icon when the shell set
    // one, else a small shelf grid — the Library tab's glyph
    const QRect iconRect(tab.left() + TabPadding, tab.top() + (tab.height() - IconSize) / 2, IconSize, IconSize);
    const QIcon icon = tabIcon(i);
    if (!icon.isNull()) {
        icon.paint(&painter, iconRect);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(ChromeColors::inactiveTabText(pal), 0.70));
        const qreal cell = 6.0;
        const QPointF base(iconRect.left() + 1.5, iconRect.top() + 1.5);
        for (int gx = 0; gx < 2; ++gx) {
            for (int gy = 0; gy < 2; ++gy) {
                painter.drawRoundedRect(QRectF(base.x() + gx * (cell + 1.0), base.y() + gy * (cell + 1.0), cell, cell), 1.5, 1.5);
            }
        }
    }

    // Left-aligned elided label, leaving the close glyph its room
    const bool closeVisible = closeGlyphVisible(i);
    const int textLeft = iconRect.right() + 8;
    const int textRight = closeVisible ? tab.right() - TabPadding - CloseHit - 4 : tab.right() - TabPadding;
    painter.setPen(active ? ChromeColors::toolbarText(pal) : withAlpha(ChromeColors::inactiveTabText(pal), 0.88));
    painter.drawText(QRect(textLeft, tab.top(), qMax(0, textRight - textLeft), tab.height()), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, fontMetrics().elidedText(displayText(i), Qt::ElideRight, qMax(0, textRight - textLeft)));

    if (closeVisible) {
        // The glyph lives centered in a 16px circle that washes on hover
        const QRect closeRect = closeButtonRect(i);
        const bool closeHot = hovered && m_closeHovered;
        if (closeHot) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(withAlpha(ChromeColors::toolbarText(pal), 0.16));
            painter.drawEllipse(closeRect);
        }
        QPen crossPen(withAlpha(ChromeColors::toolbarText(pal), closeHot ? 0.9 : 0.65), 1.0);
        crossPen.setCapStyle(Qt::RoundCap);
        painter.setPen(crossPen);
        const QPointF center = QRectF(closeRect).center();
        const qreal arm = 3.5;
        painter.drawLine(center + QPointF(-arm, -arm), center + QPointF(arm, arm));
        painter.drawLine(center + QPointF(-arm, arm), center + QPointF(arm, -arm));
    }

    painter.restore();
}

void ChromeTabBar::mousePressEvent(QMouseEvent *event)
{
    // A fresh press begins a fresh click: drop any stale drag/suppress state
    // so a lost release (focus steal, grab loss) can never misroute this one.
    m_dragCandidate = false;
    m_suppressNextDblClick = false;
    if (event->button() == Qt::LeftButton) {
        const int tab = tabAt(event->pos());
        if (tab >= 0 && closeButtonRect(tab).contains(event->pos())) {
            // Claim the press so QTabBar neither activates nor drags the
            // tab; the close fires on release within the same glyph
            m_pressedCloseTab = tab;
            event->accept();
            return;
        }
        if (m_windowDragEnabled && hitTest(event->pos()) == StripHit::Empty) {
            // Bare strip beside/after the tabs: hold the press and let a
            // real drag start the window move, so a plain click or the
            // first half of a double-click still comes through
            m_dragCandidate = true;
            m_pressPos = event->pos();
            event->accept();
            return;
        }
    }
    QTabBar::mousePressEvent(event);
}

void ChromeTabBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (m_suppressNextDblClick) {
        // The paired release just closed a tab under this glyph; the tab (and
        // its position) is gone, so do not let this synthesized double-click
        // fall onto now-bare strip and zoom the window.
        m_suppressNextDblClick = false;
        event->accept();
        return;
    }
    if (m_windowDragEnabled && event->button() == Qt::LeftButton && hitTest(event->pos()) == StripHit::Empty) {
        m_dragCandidate = false;
        titlebarDoubleClick(this);
        event->accept();
        return;
    }
    QTabBar::mouseDoubleClickEvent(event);
}

void ChromeTabBar::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragCandidate = false;
    if (m_pressedCloseTab >= 0) {
        const int tab = m_pressedCloseTab;
        m_pressedCloseTab = -1;
        if (event->button() == Qt::LeftButton && tab < count() && closeButtonRect(tab).contains(event->pos())) {
            m_suppressNextDblClick = true; // guard the double-click that may follow this close
            Q_EMIT tabCloseRequested(tab);
            event->accept();
            return;
        }
    }
    QTabBar::mouseReleaseEvent(event);
}

void ChromeTabBar::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragCandidate) {
        if ((event->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            m_dragCandidate = false;
            beginWindowDrag(this);
        }
        return;
    }
    const int tab = tabAt(event->pos());
    setHoveredTab(tab);
    const bool closeHovered = tab >= 0 && closeButtonRect(tab).contains(event->pos());
    if (closeHovered != m_closeHovered) {
        m_closeHovered = closeHovered;
        update();
    }
    QTabBar::mouseMoveEvent(event);
}

void ChromeTabBar::enterEvent(QEnterEvent *event)
{
    m_pointerInStrip = true;
    QTabBar::enterEvent(event);
}

void ChromeTabBar::leaveEvent(QEvent *event)
{
    setHoveredTab(-1);
    m_closeHovered = false;
    m_pointerInStrip = false;
    thawTabWidths();
    QTabBar::leaveEvent(event);
}

/** Retarget the hover wash, fading the old tab out and the new one in. */
void ChromeTabBar::setHoveredTab(int tab)
{
    if (tab == m_hoveredTab) {
        return;
    }
    if (m_hoveredTab != -1) {
        // the wash the pointer just left fades out from where it stood
        m_fadeTab = m_hoveredTab;
        m_fadeAnimation->stop();
        m_fadeAnimation->setStartValue(m_hoverAlpha);
        m_fadeAnimation->setEndValue(0.0);
        m_fadeAnimation->start();
    }
    m_hoveredTab = tab;
    m_hoverAnimation->stop();
    m_hoverAlpha = 0.0;
    if (tab != -1) {
        if (tab == m_fadeTab) {
            m_fadeTab = -1;
            m_fadeAnimation->stop();
        }
        m_hoverAnimation->setStartValue(0.0);
        m_hoverAnimation->setEndValue(1.0);
        m_hoverAnimation->start();
    }
    update();
}

/** The "+" rides just after the last tab, Chrome's placement. */
void ChromeTabBar::updateNewTabButton()
{
    if (!m_newTabButton) {
        return; // tabLayoutChange fires from the base setters mid-construction
    }
    int left = NewTabGap;
    if (count() > 0) {
        left = tabRect(count() - 1).right() + NewTabGap;
    }
    left = qMin(left, width() - m_newTabButton->width());
    m_newTabButton->move(left, (height() - m_newTabButton->height()) / 2);
    m_newTabButton->raise();
}

void ChromeTabBar::tabLayoutChange()
{
    QTabBar::tabLayoutChange();
    // Tab indexes may have shifted under the hover state; drop what no
    // longer points at a tab (mouse moves re-establish the rest)
    if (m_hoveredTab >= count()) {
        m_hoveredTab = -1;
        m_hoverAnimation->stop();
        m_hoverAlpha = 0.0;
        m_closeHovered = false;
    }
    if (m_fadeTab >= count()) {
        m_fadeTab = -1;
        m_fadeAnimation->stop();
    }
    updateNewTabButton();
}

void ChromeTabBar::resizeEvent(QResizeEvent *event)
{
    QTabBar::resizeEvent(event);
    updateNewTabButton();
}

void ChromeTabBar::changeEvent(QEvent *event)
{
    QTabBar::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        refreshNewTabIcon(); // stay palette-tinted across light/dark flips
        update();
    }
}

// ---------------------------------------------------------------------------
// ChromeTabStrip
// ---------------------------------------------------------------------------

ChromeTabStrip::ChromeTabStrip(QWidget *parent)
    : QWidget(parent)
{
    m_bar = new ChromeTabBar(this);

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    m_layout->addWidget(m_bar, 1);

    setFixedHeight(StripHeight);
}

ChromeTabBar *ChromeTabStrip::tabBar() const
{
    return m_bar;
}

int ChromeTabStrip::computeEffectiveInset(int windowedInset, bool fullscreen)
{
    // macOS hides the traffic lights in fullscreen, so the strip needs no
    // clearance there; Chrome collapses the same gap.
    return fullscreen ? 0 : qMax(0, windowedInset);
}

int ChromeTabStrip::effectiveInset() const
{
    return computeEffectiveInset(m_windowedInset, m_fullscreen);
}

void ChromeTabStrip::setLeadingInset(int inset)
{
    inset = qMax(0, inset);
    if (inset == m_windowedInset) {
        return;
    }
    m_windowedInset = inset;
    applyInset();
}

void ChromeTabStrip::setFullscreen(bool fullscreen)
{
    if (fullscreen == m_fullscreen) {
        return;
    }
    m_fullscreen = fullscreen;
    applyInset();
}

void ChromeTabStrip::setStripHeight(int height)
{
    if (height <= 0) {
        return;
    }
    setFixedHeight(height);
    m_bar->setStripHeight(height);
}

void ChromeTabStrip::setWindowDragEnabled(bool enabled)
{
    m_windowDragEnabled = enabled;
    m_bar->setWindowDragEnabled(enabled);
}

void ChromeTabStrip::applyInset()
{
    // The bar sits at x = inset via this left margin; all tab coordinates
    // stay in the bar's own space, so nothing in ChromeTabBar shifts.
    m_layout->setContentsMargins(effectiveInset(), 0, 0, 0);
    update();
}

void ChromeTabStrip::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    // Paint the frame shade across the full row — including the leading
    // inset the bar does not cover — so the inset reads as one surface with
    // the strip and the traffic lights rest on that shade.
    QPainter painter(this);
    painter.fillRect(rect(), ChromeColors::frameActive(palette()));
}

void ChromeTabStrip::mousePressEvent(QMouseEvent *event)
{
    // Only the leading-inset zone reaches the strip (the bar covers the
    // rest); it is always bare, so a moved press drags the window while a
    // click or double-click still passes through — but only when the strip
    // actually is the titlebar.
    if (m_windowDragEnabled && event->button() == Qt::LeftButton) {
        m_dragCandidate = true;
        m_pressPos = event->pos();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ChromeTabStrip::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragCandidate && (event->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        m_dragCandidate = false;
        beginWindowDrag(this);
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void ChromeTabStrip::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragCandidate = false;
    QWidget::mouseReleaseEvent(event);
}

void ChromeTabStrip::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (m_windowDragEnabled && event->button() == Qt::LeftButton) {
        m_dragCandidate = false;
        titlebarDoubleClick(this);
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
