/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_CHROMETABBAR_H
#define PAPERLIBRARY_CHROMETABBAR_H

#include <QTabBar>

class KConfigGroup;
class QHBoxLayout;
class QPainter;
class QToolButton;
class QVariantAnimation;

/**
 * Whether the strip should live inside the titlebar (macOS Chrome-style).
 * Reads paperlibraryrc [General] TitlebarTabs, defaulting to true; a false value
 * restores the classic below-the-titlebar layout. Free function so the
 * escape hatch is one testable line, shared by the shell and its tests.
 */
bool chromeTitlebarTabsEnabled(const KConfigGroup &general);

/**
 * A tab bar in Google Chrome's visual language: a compact strip of
 * uniform-width tabs (equal widths that squeeze as tabs accumulate, never
 * text-sized) where the active tab shares the toolbar's background and
 * joins it through Chrome's hallmark bottom flare — concave quarter-curves
 * sweeping outward at the base — so tab and toolbar read as one surface.
 * Inactive tabs sit slightly darker with short centered hairline
 * separators between them (hidden around the active and hovered tabs),
 * hover shows a soft animated rounded-rect wash, the close glyph only
 * appears on the active tab and on hovered tabs that are wide enough, and
 * a "+" button rides along after the last tab to open a new (Library)
 * tab. All colors derive from the palette so light and dark just work.
 *
 * Everything is painted by hand; the platform style is bypassed entirely
 * for this widget. Install on a QTabWidget via setTabBar() before any
 * tabs are added.
 */
class ChromeTabBar : public QTabBar
{
    Q_OBJECT

public:
    explicit ChromeTabBar(QWidget *parent = nullptr);

    /** How a point on the strip should behave when pressed. */
    enum class StripHit {
        Tab,          ///< over a tab: select / drag-to-reorder (QTabBar's job)
        NewTabButton, ///< over the "+" button: the child button handles it
        Empty         ///< bare strip: drags the window, double-click zooms
    };

    /** Classify @p pos (in bar coordinates) for drag-region hit-testing. */
    StripHit hitTest(const QPoint &pos) const;

    /**
     * The strip's row height. On macOS titlebar-tabs mode this is grown to
     * match the native titlebar so the traffic lights center in the row;
     * elsewhere it keeps Chrome's compact default.
     */
    void setStripHeight(int height);

    /** The in-strip "+" button geometry, in bar coordinates (test hook). */
    QRect newTabButtonRect() const;

    /**
     * Whether a bare-strip press drags the window and a bare double-click
     * zooms it. Only true when the strip actually is the titlebar (macOS
     * titlebar-tabs mode, windowed); off it, and below the escape hatch, the
     * bare strip stays inert like a plain QTabBar.
     */
    void setWindowDragEnabled(bool enabled);

    /** The action the in-strip "+" button triggers (the new-tab action). */
    void setNewTabAction(QAction *action);

    /**
     * The text the strip actually paints for a tab: the stored text with
     * accelerator markers removed. KAcceleratorManager walks the window's
     * widget tree on every XMLGui (re)build and weaves '&' mnemonics into
     * QTabBar texts; a hand-painted strip would show them literally. The
     * bar opts out of the manager, and this strips whatever got through —
     * while "&&", the escape ChromeTabWidget stores for a real ampersand
     * in a document name, displays as '&'.
     */
    QString displayText(int index) const;

    /** Hit rectangle of a tab's close glyph; invalid when the glyph is hidden. */
    QRect closeButtonRect(int index) const;

protected:
    QSize tabSizeHint(int index) const override;
    QSize minimumTabSizeHint(int index) const override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void tabLayoutChange() override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void tabInserted(int index) override;
    void tabRemoved(int index) override;

private:
    bool closeGlyphVisible(int index) const;
    void paintTab(QPainter &painter, int index);
    void setHoveredTab(int tab);
    void updateNewTabButton();
    void refreshNewTabIcon();
    int uniformTabWidth(int tabCount) const;
    void relayoutTabs();
    void thawTabWidths();

    QToolButton *m_newTabButton = nullptr;
    QVariantAnimation *m_hoverAnimation = nullptr;
    QVariantAnimation *m_fadeAnimation = nullptr;
    qreal m_hoverAlpha = 0.0; // wash strength on m_hoveredTab
    qreal m_fadeAlpha = 0.0;  // wash strength on m_fadeTab, on its way out
    int m_hoveredTab = -1;
    int m_fadeTab = -1;
    bool m_closeHovered = false;
    int m_pressedCloseTab = -1;
    int m_stripHeight;                   // set in the constructor (Chrome's compact default)
    bool m_dragCandidate = false;        // a press landed on bare strip; a drag would move the window
    bool m_windowDragEnabled = false;    // bare strip drags/zooms only when it is the titlebar
    bool m_suppressNextDblClick = false; // a close-glyph release must not let the paired double-click zoom
    bool m_pointerInStrip = false;       // while true, a close must not reflow the tabs under the pointer
    int m_frozenTabWidth = 0;            // 0 == not frozen; else the width every tab keeps
    QPoint m_pressPos;
};

/**
 * The strip row that hosts the ChromeTabBar. It adds Chrome's leading inset
 * so the first tab clears the macOS traffic lights (the bar sits at
 * x = inset inside this container, keeping every tab coordinate untouched),
 * paints the frame shade across the full width, and turns bare-strip presses
 * into window drags and double-clicks into the titlebar zoom action. In
 * fullscreen the inset collapses to zero (macOS hides the buttons there).
 */
class ChromeTabStrip : public QWidget
{
    Q_OBJECT

public:
    explicit ChromeTabStrip(QWidget *parent = nullptr);

    ChromeTabBar *tabBar() const;

    /** The clearance the strip leaves before the first tab when windowed. */
    void setLeadingInset(int inset);

    /** In fullscreen the leading inset collapses (no traffic lights). */
    void setFullscreen(bool fullscreen);

    /** Grow the row (and its bar) to @p height; matches the native titlebar. */
    void setStripHeight(int height);

    /** Bare-strip presses drag / double-clicks zoom only when it is the titlebar. */
    void setWindowDragEnabled(bool enabled);

    /** The inset actually applied right now: 0 in fullscreen, else windowed. */
    int effectiveInset() const;

    /** Pure inset rule, split out so it can be unit-tested headlessly. */
    static int computeEffectiveInset(int windowedInset, bool fullscreen);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void applyInset();

    ChromeTabBar *m_bar = nullptr;
    QHBoxLayout *m_layout = nullptr;
    int m_windowedInset = 0;
    bool m_fullscreen = false;
    bool m_dragCandidate = false;
    bool m_windowDragEnabled = false;
    QPoint m_pressPos;
};

#endif
