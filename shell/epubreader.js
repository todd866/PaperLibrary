(function () {
    "use strict";

    const cssText = __PAPERLIBRARY_READER_CSS__;
    const styleId = "paperlibrary-reader-style";
    const initialFontScaleStep = __PAPERLIBRARY_INITIAL_FONT_SCALE_STEP__;
    const initialScrollMode = __PAPERLIBRARY_SCROLL_MODE__;
    const initialReaderMotion = __PAPERLIBRARY_READER_MOTION__;
    const minFontScaleStep = -5;
    const maxFontScaleStep = 7;
    const baseFontSize = 18;
    const fontScaleFactor = 1.1;
    const smoothScrollMs = 220;
    const spineTurnMs = 150;
    const wheelPageThreshold = 12;
    const wheelGestureQuietMs = 180;
    const spineSlideWindowNamePrefix = "__paperlibrary_epub_slide__:";

    let fontScaleStep = clampFontScaleStep(initialFontScaleStep);
    let lastScrollTarget = Number.NaN;
    let lastScrollTargetTimer = 0;
    let resizeTimer = 0;
    let lastBoundaryNavigation = 0;
    let wheelAccumulator = 0;
    let wheelLocked = false;
    let wheelLockedDirection = 0;
    let wheelQuietTimer = 0;
    let scrollMode = normalizeScrollMode(initialScrollMode);
    let readerMotion = initialReaderMotion !== false;

    function installStyle() {
        let style = document.getElementById(styleId);
        if (!style) {
            style = document.createElement("style");
            style.id = styleId;
            (document.head || document.documentElement).appendChild(style);
        }
        applyScrollModeClass();
        applyReaderMotionClass();
        applyFontScaleStep(fontScaleStep);
    }

    function scroller() {
        return document.body || document.scrollingElement || document.documentElement;
    }

    function finiteNumber(value) {
        const number = Number(value);
        return Number.isFinite(number) ? number : 0;
    }

    function finitePositive(value) {
        const number = finiteNumber(value);
        return number > 0 ? number : 0;
    }

    function clamp(value, minimum, maximum) {
        if (!Number.isFinite(value)) {
            return minimum;
        }
        return Math.min(maximum, Math.max(minimum, value));
    }

    function clampFontScaleStep(step) {
        return Math.round(clamp(finiteNumber(step), minFontScaleStep, maxFontScaleStep));
    }

    function normalizeScrollMode(value) {
        return value === "continuous" ? "continuous" : "paginated";
    }

    function isPaginated() {
        return scrollMode === "paginated";
    }

    function applyScrollModeClass() {
        document.documentElement.classList.toggle("paperlibrary-scroll-paginated", isPaginated());
        document.documentElement.classList.toggle("paperlibrary-scroll-continuous", !isPaginated());
    }

    function applyReaderMotionClass() {
        document.documentElement.classList.toggle("paperlibrary-motion-reduced", !readerMotion);
    }

    function fontSizeForStep(step) {
        return Math.round(baseFontSize * Math.pow(fontScaleFactor, step) * 10) / 10;
    }

    function readerStyleText() {
        return cssText + "\n:root { --paper-reader-font-size: " + fontSizeForStep(fontScaleStep) + "px; }";
    }

    function applyFontScaleStep(step) {
        fontScaleStep = clampFontScaleStep(step);
        const style = document.getElementById(styleId);
        if (style) {
            style.textContent = readerStyleText();
        }
    }

    function rawScrollLeft() {
        const element = scroller();
        if (!isPaginated()) {
            return finitePositive(element && element.scrollTop) || finitePositive(document.documentElement && document.documentElement.scrollTop) || finitePositive(document.body && document.body.scrollTop);
        }
        return finitePositive(element && element.scrollLeft) || finitePositive(document.documentElement && document.documentElement.scrollLeft) || finitePositive(document.body && document.body.scrollLeft);
    }

    function maxScrollLeft() {
        const element = scroller();
        if (!element) {
            return 0;
        }

        const scrollWidth = isPaginated() ? finitePositive(element.scrollWidth) : finitePositive(element.scrollHeight);
        const clientWidth = isPaginated() ? finitePositive(element.clientWidth) || finitePositive(window.innerWidth) : finitePositive(element.clientHeight) || finitePositive(window.innerHeight);
        if (scrollWidth <= 0 || clientWidth <= 0) {
            return 0;
        }

        const realMax = scrollWidth - clientWidth;
        return Number.isFinite(realMax) && realMax > 0 ? realMax : 0;
    }

    function pageStep() {
        if (!isPaginated()) {
            const element = scroller();
            const visibleHeight = finitePositive(element && element.clientHeight) || finitePositive(window.innerHeight);
            return visibleHeight > 0 ? Math.max(1, visibleHeight * 0.86) : 1;
        }

        const bodyStyle = document.body ? window.getComputedStyle(document.body) : null;
        const columnWidth = bodyStyle ? finitePositive(parseFloat(bodyStyle.columnWidth)) : 0;
        const columnGap = bodyStyle ? finitePositive(parseFloat(bodyStyle.columnGap)) : 0;
        const columnPitch = columnWidth + columnGap;
        if (Number.isFinite(columnPitch) && columnPitch > 0) {
            return columnPitch;
        }

        const element = scroller();
        const visibleWidth = finitePositive(element && element.clientWidth) || finitePositive(window.innerWidth);
        return visibleWidth > 0 ? visibleWidth : 1;
    }

    function boundaryTolerance(step) {
        return Math.max(2, step * 0.002);
    }

    function lastPageIndex(max, step) {
        if (max <= boundaryTolerance(step)) {
            return 0;
        }
        return Math.max(0, Math.ceil((max - boundaryTolerance(step)) / step));
    }

    function snappedOffset(offset, max, step) {
        if (max <= 0) {
            return 0;
        }
        const target = clamp(finiteNumber(offset), 0, max);
        if (target >= max - boundaryTolerance(step)) {
            return max;
        }
        const page = clamp(Math.round(target / step), 0, lastPageIndex(max, step));
        return clamp(page * step, 0, max);
    }

    function scrollOffset() {
        return clamp(rawScrollLeft(), 0, maxScrollLeft());
    }

    function navigationOffset() {
        return Number.isFinite(lastScrollTarget) ? clamp(lastScrollTarget, 0, maxScrollLeft()) : scrollOffset();
    }

    function clearLastScrollTargetSoon() {
        window.clearTimeout(lastScrollTargetTimer);
        lastScrollTargetTimer = window.setTimeout(function () {
            lastScrollTarget = Number.NaN;
        }, smoothScrollMs + 80);
    }

    function setScrollOffset(offset, options) {
        const element = scroller();
        if (!element) {
            return 0;
        }

        const max = maxScrollLeft();
        const step = pageStep();
        const requested = offset === "end" ? max : finiteNumber(offset);
        const target = snappedOffset(requested, max, step);
        const smooth = Boolean(options && options.smooth);

        if (smooth) {
            lastScrollTarget = target;
            clearLastScrollTargetSoon();
            if (typeof element.scrollTo === "function") {
                element.scrollTo({ left: isPaginated() ? target : 0, top: isPaginated() ? 0 : target, behavior: "smooth" });
            } else {
                if (isPaginated()) {
                    element.scrollLeft = target;
                } else {
                    element.scrollTop = target;
                }
            }
        } else {
            window.clearTimeout(lastScrollTargetTimer);
            lastScrollTarget = Number.NaN;
            if (isPaginated()) {
                element.scrollLeft = target;
                document.documentElement.scrollLeft = target;
            } else {
                element.scrollTop = target;
                document.documentElement.scrollTop = target;
            }
            if (document.body) {
                if (isPaginated()) {
                    document.body.scrollLeft = target;
                } else {
                    document.body.scrollTop = target;
                }
            }
        }

        return target;
    }

    function takeSpineSlideDirection() {
        if (typeof window.name !== "string" || !window.name.startsWith(spineSlideWindowNamePrefix)) {
            return 0;
        }
        const direction = Number(window.name.slice(spineSlideWindowNamePrefix.length));
        window.name = "";
        return direction > 0 ? 1 : direction < 0 ? -1 : 0;
    }

    function playSpineEntryAnimation() {
        const direction = takeSpineSlideDirection();
        if (!direction || !readerMotion) {
            return;
        }
        wheelLocked = true;
        markWheelQuietSoon();
        const className = direction > 0 ? "paperlibrary-spine-enter-next" : "paperlibrary-spine-enter-previous";
        document.documentElement.classList.add(className);
        window.setTimeout(function () {
            document.documentElement.classList.remove(className);
        }, smoothScrollMs + 60);
    }

    function requestSpineTurn(direction) {
        const now = Date.now();
        if (now - lastBoundaryNavigation < 300) {
            return false;
        }
        lastBoundaryNavigation = now;
        const command = direction > 0 ? "next" : "previous";
        if (!readerMotion) {
            window.location.assign(window.location.protocol + "//" + window.location.host + "/.paperlibrary/" + command);
            return true;
        }
        const className = direction > 0 ? "paperlibrary-spine-exit-next" : "paperlibrary-spine-exit-previous";
        document.documentElement.classList.add(className);
        window.name = spineSlideWindowNamePrefix + (direction > 0 ? "1" : "-1");
        window.setTimeout(function () {
            window.location.assign(window.location.protocol + "//" + window.location.host + "/.paperlibrary/" + command);
        }, spineTurnMs);
        return true;
    }

    function turnPage(direction) {
        const max = maxScrollLeft();
        const step = pageStep();
        const current = navigationOffset();
        const tolerance = boundaryTolerance(step);

        if (max <= tolerance) {
            return requestSpineTurn(direction);
        }

        const lastIndex = lastPageIndex(max, step);
        const currentIndex = clamp(Math.round(current / step), 0, lastIndex);
        if (direction > 0 && currentIndex >= lastIndex && current >= max - tolerance) {
            return requestSpineTurn(direction);
        }
        if (direction < 0 && currentIndex <= 0 && current <= tolerance) {
            return requestSpineTurn(direction);
        }

        const targetIndex = clamp(currentIndex + (direction > 0 ? 1 : -1), 0, lastIndex);
        const target = snappedOffset(targetIndex * step, max, step);
        if (Math.abs(target - current) > 0.5) {
            setScrollOffset(target, { smooth: true });
            return true;
        }
        return requestSpineTurn(direction);
    }

    function relayout() {
        const beforeMax = maxScrollLeft();
        const before = scrollOffset();
        const ratio = beforeMax > 0 ? before / beforeMax : 0;
        installStyle();
        window.clearTimeout(resizeTimer);
        resizeTimer = window.setTimeout(function () {
            setScrollOffset(ratio * maxScrollLeft(), { smooth: false });
        }, 50);
    }

    function setFontScaleStep(step) {
        const beforeMax = maxScrollLeft();
        const before = scrollOffset();
        const ratio = beforeMax > 0 ? before / beforeMax : 0;
        applyFontScaleStep(step);
        window.clearTimeout(resizeTimer);
        resizeTimer = window.setTimeout(function () {
            setScrollOffset(ratio * maxScrollLeft(), { smooth: false });
        }, 80);
        return fontScaleStep;
    }

    function setScrollMode(mode) {
        const beforeMax = maxScrollLeft();
        const before = scrollOffset();
        const ratio = beforeMax > 0 ? before / beforeMax : 0;
        scrollMode = normalizeScrollMode(mode);
        wheelAccumulator = 0;
        wheelLocked = false;
        applyScrollModeClass();
        installStyle();
        window.clearTimeout(resizeTimer);
        resizeTimer = window.setTimeout(function () {
            setScrollOffset(ratio * maxScrollLeft(), { smooth: false });
        }, 80);
        return scrollMode;
    }

    function setReaderMotion(enabled) {
        readerMotion = enabled !== false;
        applyReaderMotionClass();
        return readerMotion;
    }

    function handleKey(event) {
        if (event.defaultPrevented || event.altKey || event.ctrlKey || event.metaKey) {
            return;
        }
        if (event.key === "ArrowRight" || event.key === "PageDown" || event.key === " ") {
            if (turnPage(1)) {
                event.preventDefault();
            }
        } else if (event.key === "ArrowLeft" || event.key === "PageUp") {
            if (turnPage(-1)) {
                event.preventDefault();
            }
        }
    }

    function markWheelQuietSoon() {
        window.clearTimeout(wheelQuietTimer);
        wheelQuietTimer = window.setTimeout(function () {
            wheelAccumulator = 0;
            wheelLocked = false;
            wheelLockedDirection = 0;
        }, wheelGestureQuietMs);
    }

    function handleWheel(event) {
        if (event.defaultPrevented || event.altKey || event.ctrlKey || event.metaKey) {
            return;
        }
        if (!isPaginated()) {
            return;
        }

        const dominant = Math.abs(event.deltaY) >= Math.abs(event.deltaX) ? event.deltaY : event.deltaX;
        const direction = dominant > 0 ? 1 : dominant < 0 ? -1 : 0;
        if (!direction) {
            return;
        }

        event.preventDefault();

        if (wheelLocked) {
            if (direction === wheelLockedDirection) {
                markWheelQuietSoon();
                return;
            }
            wheelAccumulator = 0;
            wheelLocked = false;
            wheelLockedDirection = 0;
        }

        wheelAccumulator += dominant;
        if (Math.abs(wheelAccumulator) < wheelPageThreshold) {
            markWheelQuietSoon();
            return;
        }

        const turnDirection = wheelAccumulator > 0 ? 1 : -1;
        wheelAccumulator = 0;
        wheelLocked = true;
        wheelLockedDirection = turnDirection;
        markWheelQuietSoon();
        turnPage(turnDirection);
    }

    window.__paperLibraryEpub = {
        nextPage: function () {
            return turnPage(1);
        },
        previousPage: function () {
            return turnPage(-1);
        },
        setScrollOffset: setScrollOffset,
        setScrollOffsetToEnd: function () {
            return setScrollOffset("end", { smooth: false });
        },
        scrollOffset: scrollOffset,
        maxScrollLeft: maxScrollLeft,
        maxScrollOffset: maxScrollLeft,
        pageStep: pageStep,
        relayout: relayout,
        setFontScaleStep: setFontScaleStep,
        setScrollMode: setScrollMode,
        setReaderMotion: setReaderMotion
    };

    function initialize() {
        installStyle();
        playSpineEntryAnimation();
        setScrollOffset(scrollOffset(), { smooth: false });
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", initialize, { once: true });
    } else {
        initialize();
    }
    window.addEventListener("keydown", handleKey, true);
    window.addEventListener("wheel", handleWheel, { passive: false, capture: true });
    window.addEventListener("resize", relayout);
})();
