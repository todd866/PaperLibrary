/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "pdfview.h"
#include "telemetry.h"
#include "readingprogress.h"

#include "claudeprocesshelper.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QAction>
#include <QAbstractAnimation>
#include <QAbstractItemView>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QIdentityProxyModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPen>
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QPdfLink>
#include <QPdfLinkModel>
#include <QPdfPageNavigator>
#include <QPdfSearchModel>
#include <QPdfSelection>
#include <QPdfView>
#include <QProcess>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QSaveFile>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace
{
constexpr auto PositionsGroup = "PdfView Positions";
constexpr auto GeneralGroup = "General";
constexpr auto ReaderMotionKey = "ReaderMotion";
constexpr double MinZoom = 0.10;
constexpr double MaxZoom = 8.0;
constexpr int AiNavigationWordBudget = 50000;
constexpr int AiNavigationClaudeTimeoutMs = 120000;
constexpr int AiNavigationPageRole = Qt::UserRole + 1;
constexpr int AiNavigationLevelRole = Qt::UserRole + 2;

struct PdfDocumentFingerprint {
    QString canonicalPath;
    qint64 size = 0;
    QDateTime modifiedUtc;
    QString hash;
};

struct PdfPagePoint {
    bool valid = false;
    int page = -1;
    QPointF point;
};

struct PdfPageLayout {
    QRect geometry;
    qreal zoom = 1.0;
};

struct PdfDocumentLayout {
    QSize documentSize;
    QHash<int, PdfPageLayout> pages;
};

struct PdfTextSelection {
    int page = -1;
    QPdfSelection selection;
};

struct PdfHighlight {
    QString id;
    int pageIndex = -1;
    QList<QRectF> rects;
    QColor color;
    QString text;
    QString note;
    QDateTime createdAt;
};

class BookmarkTitleModel : public QIdentityProxyModel
{
public:
    using QIdentityProxyModel::QIdentityProxyModel;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (role == Qt::DisplayRole) {
            return QIdentityProxyModel::data(index, static_cast<int>(QPdfBookmarkModel::Role::Title));
        }
        return QIdentityProxyModel::data(index, role);
    }
};

static int navigationPageOneBased(const QModelIndex &index)
{
    bool ok = false;
    int page = index.data(AiNavigationPageRole).toInt(&ok);
    if (ok && page >= 0) {
        return page + 1;
    }
    page = index.data(static_cast<int>(QPdfBookmarkModel::Role::Page)).toInt(&ok);
    return ok && page >= 0 ? page + 1 : 0;
}

static int navigationLevel(const QModelIndex &index)
{
    bool ok = false;
    const int aiLevel = index.data(AiNavigationLevelRole).toInt(&ok);
    if (ok && aiLevel > 0) {
        return aiLevel;
    }
    int level = 1;
    QModelIndex parent = index.parent();
    while (parent.isValid()) {
        ++level;
        parent = parent.parent();
    }
    return qBound(1, level, 4);
}

class NavigationTreeDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), 28));
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString title = opt.text;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        if (selected || hovered) {
            QColor background = selected ? option.palette.color(QPalette::Highlight) : option.palette.color(QPalette::Text);
            background.setAlphaF(selected ? 0.16 : 0.06);
            painter->setPen(Qt::NoPen);
            painter->setBrush(background);
            painter->drawRoundedRect(option.rect.adjusted(4, 3, -4, -3), 6, 6);
        }

        const int page = navigationPageOneBased(index);
        QFont badgeFont = option.font;
        badgeFont.setPointSizeF(qMax(8.0, option.font.pointSizeF() * 0.78));
        const QFontMetrics badgeMetrics(badgeFont);
        const QString pageText = page > 0 ? QString::number(page) : QString();
        const int badgeWidth = pageText.isEmpty() ? 0 : qMax(28, badgeMetrics.horizontalAdvance(pageText) + 14);
        QRect badgeRect;
        if (badgeWidth > 0) {
            badgeRect = QRect(option.rect.right() - badgeWidth - 8, option.rect.center().y() - 9, badgeWidth, 18);
        }

        QRect textRect = option.rect.adjusted(10, 0, badgeWidth > 0 ? -(badgeWidth + 18) : -10, 0);
        QFont titleFont = option.font;
        const int level = navigationLevel(index);
        if (level == 1) {
            titleFont.setBold(true);
        }
        painter->setFont(titleFont);
        QColor textColor = selected ? option.palette.color(QPalette::HighlightedText) : option.palette.color(QPalette::Text);
        if (!selected && level > 1) {
            textColor.setAlphaF(0.78);
        }
        painter->setPen(textColor);
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(titleFont).elidedText(title, Qt::ElideRight, textRect.width()));

        if (!pageText.isEmpty()) {
            QColor badgeBackground = option.palette.color(QPalette::Text);
            badgeBackground.setAlphaF(selected ? 0.18 : 0.08);
            QColor badgeText = selected ? option.palette.color(QPalette::HighlightedText) : option.palette.color(QPalette::Text);
            badgeText.setAlphaF(selected ? 0.95 : 0.62);
            painter->setPen(Qt::NoPen);
            painter->setBrush(badgeBackground);
            painter->drawRoundedRect(badgeRect, 9, 9);
            painter->setFont(badgeFont);
            painter->setPen(badgeText);
            painter->drawText(badgeRect, Qt::AlignCenter, pageText);
        }
        painter->restore();
    }
};

static void configureNavigationTree(QTreeView *view)
{
    view->setHeaderHidden(true);
    view->setUniformRowHeights(true);
    view->setAlternatingRowColors(false);
    view->setFrameShape(QFrame::NoFrame);
    view->setIndentation(15);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setTextElideMode(Qt::ElideRight);
    view->setMouseTracking(true);
    view->setItemDelegate(new NavigationTreeDelegate(view));
}

bool shouldOpenExternalPdfUrl(const QUrl &url)
{
    if (!url.isValid() || url.isRelative() || url.scheme().isEmpty()) {
        return false;
    }

    const QString scheme = url.scheme().toLower();
    return scheme != QLatin1String("javascript") && scheme != QLatin1String("data");
}

QColor defaultHighlightColor()
{
    return QColor(QStringLiteral("#fff176"));
}

QString hashBytes(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString contentFingerprintForFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(1024 * 1024));
    }
    return QStringLiteral("sha256:%1").arg(QString::fromLatin1(hash.result().toHex()));
}

QString highlightStoreDirectory()
{
    QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (appDataPath.isEmpty()) {
        appDataPath = QDir::home().filePath(QStringLiteral(".paperlibrary"));
    }
    return QDir(appDataPath).filePath(QStringLiteral("pdf-highlights"));
}

QString aiNavigationStoreDirectory()
{
    QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (appDataPath.isEmpty()) {
        appDataPath = QDir::home().filePath(QStringLiteral(".paperlibrary"));
    }
    return QDir(appDataPath).filePath(QStringLiteral("pdf-ainav"));
}

PdfDocumentFingerprint fingerprintForPdfPath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonicalPath = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    const QDateTime modifiedUtc = info.lastModified().toUTC();
    const QByteArray key = canonicalPath.toUtf8() + '\n' + QByteArray::number(info.size()) + '\n' + modifiedUtc.toString(Qt::ISODateWithMs).toUtf8();
    return PdfDocumentFingerprint{canonicalPath, info.size(), modifiedUtc, hashBytes(key)};
}

QJsonObject fingerprintToJson(const PdfDocumentFingerprint &fingerprint)
{
    return QJsonObject{
        {QStringLiteral("path"), fingerprint.canonicalPath},
        {QStringLiteral("size"), QString::number(fingerprint.size)},
        {QStringLiteral("mtime"), fingerprint.modifiedUtc.toString(Qt::ISODateWithMs)},
        {QStringLiteral("hash"), fingerprint.hash},
    };
}

int wordCountInText(const QString &text)
{
    int words = 0;
    bool inWord = false;
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            inWord = false;
        } else if (!inWord) {
            inWord = true;
            ++words;
        }
    }
    return words;
}

QString firstWords(const QString &text, int maxWords)
{
    if (maxWords <= 0) {
        return {};
    }

    int words = 0;
    bool inWord = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch.isSpace()) {
            inWord = false;
            continue;
        }
        if (!inWord) {
            inWord = true;
            ++words;
            if (words > maxWords) {
                return text.left(i).trimmed();
            }
        }
    }
    return text.trimmed();
}

QString normalizedNavigationTitle(const QString &title)
{
    QString normalized;
    normalized.reserve(title.size());
    for (const QChar ch : title.toCaseFolded()) {
        if (ch.isLetterOrNumber()) {
            normalized.append(ch);
        }
    }
    return normalized;
}

QJsonArray navigationEntriesToJson(const QList<PdfView::AiNavigationEntry> &entries)
{
    QJsonArray array;
    for (const PdfView::AiNavigationEntry &entry : entries) {
        array.append(QJsonObject{
            {QStringLiteral("title"), entry.title},
            {QStringLiteral("page"), entry.pageOneBased},
            {QStringLiteral("level"), qBound(1, entry.level, 3)},
        });
    }
    return array;
}

QList<PdfView::AiNavigationEntry> navigationEntriesFromJson(const QJsonArray &array, int pageCount)
{
    QList<PdfView::AiNavigationEntry> entries;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        const QString title = object.value(QStringLiteral("title")).toString().trimmed();
        const int page = object.value(QStringLiteral("page")).toInt(-1);
        if (title.isEmpty() || page < 1 || page > pageCount) {
            continue;
        }
        entries.append(PdfView::AiNavigationEntry{title, page, qBound(1, object.value(QStringLiteral("level")).toInt(1), 3)});
    }
    return entries;
}

QString outlineTextForPrompt(const QList<PdfView::AiNavigationEntry> &entries)
{
    if (entries.isEmpty()) {
        return QStringLiteral("No PDF outline was found.");
    }

    QStringList lines;
    lines.reserve(entries.size());
    for (const PdfView::AiNavigationEntry &entry : entries) {
        lines << QStringLiteral("%1- page %2, level %3: %4")
                     .arg(QString(qMax(0, entry.level - 1) * 2, QLatin1Char(' ')))
                     .arg(entry.pageOneBased)
                     .arg(qBound(1, entry.level, 3))
                     .arg(entry.title);
    }
    return lines.join(QLatin1Char('\n'));
}

QString aiNavigationPrompt(const QList<PdfView::AiNavigationEntry> &outlineEntries, const QString &sourceText, int pageCount, bool textTruncated)
{
    QString prompt =
        QStringLiteral("You are building smart semantic navigation for a PDF reader. "
                       "Respond with ONLY a valid JSON array and no prose. Each array item must be exactly "
                       "{\"title\": string, \"page\": 1-based integer, \"level\": integer 1-3}. "
                       "Cover the document's real structure: chapters, major sections, and important concepts, figures, tables, or named ideas a reader would want to jump to. "
                       "AUGMENT the raw table of contents; be richer and more useful than merely copying it. "
                       "Use the raw PDF outline as ground truth when present, and add semantic jump points from the page-marked text. "
                       "Only cite pages from 1 to %1. Use the [[PAGE n]] markers as the page source. "
                       "If the text is truncated, use the available text plus the outline, and do not invent pages or sections. "
                       "Prefer concise titles.\n\n"
                       "Page count: %1\n"
                       "Text truncated: %2\n\n")
            .arg(pageCount)
            .arg(textTruncated ? QStringLiteral("yes") : QStringLiteral("no"));
    prompt += QStringLiteral("Raw PDF outline:\n");
    prompt += outlineTextForPrompt(outlineEntries);
    prompt += QStringLiteral("\n\nPage-marked extracted text:\n");
    prompt += sourceText.isEmpty() ? QStringLiteral("No extractable page text was found.") : sourceText;
    return prompt;
}

QList<PdfView::AiNavigationEntry> parseAiNavigationReply(const QByteArray &reply, int pageCount)
{
    const QJsonDocument document = ClaudeProcessHelper::parseClaudeJsonResult(reply);
    QJsonArray array;
    if (document.isArray()) {
        array = document.array();
    } else if (document.isObject()) {
        const QJsonObject object = document.object();
        array = object.value(QStringLiteral("navigation")).toArray();
        if (array.isEmpty()) {
            array = object.value(QStringLiteral("aiNavigation")).toArray();
        }
    }
    return navigationEntriesFromJson(array, pageCount);
}

QList<PdfView::AiNavigationEntry> mergedNavigationEntries(const QList<PdfView::AiNavigationEntry> &outlineEntries,
                                                          const QList<PdfView::AiNavigationEntry> &aiEntries,
                                                          int pageCount)
{
    QList<PdfView::AiNavigationEntry> merged;
    QSet<QString> seen;
    const auto append = [&](const PdfView::AiNavigationEntry &entry) {
        if (entry.title.trimmed().isEmpty() || entry.pageOneBased < 1 || entry.pageOneBased > pageCount) {
            return;
        }
        const QString key = QStringLiteral("%1:%2").arg(entry.pageOneBased).arg(normalizedNavigationTitle(entry.title));
        if (seen.contains(key)) {
            return;
        }
        seen.insert(key);
        merged.append(PdfView::AiNavigationEntry{entry.title.trimmed(), entry.pageOneBased, qBound(1, entry.level, 3)});
    };

    for (const PdfView::AiNavigationEntry &entry : outlineEntries) {
        append(entry);
    }
    for (const PdfView::AiNavigationEntry &entry : aiEntries) {
        append(entry);
    }

    std::stable_sort(merged.begin(), merged.end(), [](const PdfView::AiNavigationEntry &left, const PdfView::AiNavigationEntry &right) {
        if (left.pageOneBased != right.pageOneBased) {
            return left.pageOneBased < right.pageOneBased;
        }
        if (left.level != right.level) {
            return left.level < right.level;
        }
        return left.title.localeAwareCompare(right.title) < 0;
    });
    return merged;
}

QJsonObject rectToJson(const QRectF &rect)
{
    return QJsonObject{
        {QStringLiteral("x"), rect.x()},
        {QStringLiteral("y"), rect.y()},
        {QStringLiteral("w"), rect.width()},
        {QStringLiteral("h"), rect.height()},
    };
}

QRectF rectFromJson(const QJsonObject &object)
{
    return QRectF(object.value(QStringLiteral("x")).toDouble(),
                  object.value(QStringLiteral("y")).toDouble(),
                  object.value(QStringLiteral("w")).toDouble(),
                  object.value(QStringLiteral("h")).toDouble());
}

class PdfHighlightStore
{
public:
    void clear()
    {
        m_documentPath.clear();
        m_canonicalPath.clear();
        m_documentFingerprint.clear();
        m_storePath.clear();
        m_highlights.clear();
    }

    bool setDocumentPath(const QString &path)
    {
        clear();

        const QFileInfo info(path);
        m_documentPath = path;
        m_canonicalPath = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
        m_documentFingerprint = contentFingerprintForFile(m_canonicalPath);

        QString storeKey;
        if (m_documentFingerprint.startsWith(QLatin1String("sha256:"))) {
            storeKey = m_documentFingerprint.mid(7);
        } else {
            storeKey = QStringLiteral("path-%1").arg(hashBytes(m_canonicalPath.toUtf8()));
            m_documentFingerprint = QStringLiteral("path-sha256:%1").arg(storeKey.mid(5));
        }

        m_storePath = QDir(highlightStoreDirectory()).filePath(storeKey + QStringLiteral(".json"));
        return load();
    }

    const QList<PdfHighlight> &highlights() const
    {
        return m_highlights;
    }

    QString storePath() const
    {
        return m_storePath;
    }

    int addHighlight(int pageIndex, const QList<QRectF> &rects, const QColor &color, const QString &text, const QString &note, const QDateTime &createdAt)
    {
        if (m_storePath.isEmpty() || rects.isEmpty() || pageIndex < 0) {
            return -1;
        }

        PdfHighlight highlight;
        highlight.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        highlight.pageIndex = pageIndex;
        highlight.rects = rects;
        highlight.color = color.isValid() ? color : defaultHighlightColor();
        highlight.text = text;
        highlight.note = note;
        highlight.createdAt = createdAt.isValid() ? createdAt.toUTC() : QDateTime::currentDateTimeUtc();

        m_highlights.append(highlight);
        return m_highlights.size() - 1;
    }

    bool removeHighlight(int index)
    {
        if (index < 0 || index >= m_highlights.size()) {
            return false;
        }

        m_highlights.removeAt(index);
        return true;
    }

    bool save() const
    {
        if (m_storePath.isEmpty()) {
            return false;
        }

        if (!QDir().mkpath(QFileInfo(m_storePath).absolutePath())) {
            return false;
        }

        QJsonArray highlights;
        for (const PdfHighlight &highlight : m_highlights) {
            QJsonArray rects;
            for (const QRectF &rect : highlight.rects) {
                rects.append(rectToJson(rect));
            }

            QJsonObject object{
                {QStringLiteral("id"), highlight.id},
                {QStringLiteral("pageIndex"), highlight.pageIndex},
                {QStringLiteral("rects"), rects},
                {QStringLiteral("color"), highlight.color.name(QColor::HexRgb)},
                {QStringLiteral("text"), highlight.text},
                {QStringLiteral("createdAt"), highlight.createdAt.toUTC().toString(Qt::ISODateWithMs)},
            };
            if (!highlight.note.isEmpty()) {
                object.insert(QStringLiteral("note"), highlight.note);
            }
            highlights.append(object);
        }

        const QJsonObject root{
            {QStringLiteral("version"), 1},
            {QStringLiteral("documentPath"), m_documentPath},
            {QStringLiteral("documentCanonicalPath"), m_canonicalPath},
            {QStringLiteral("documentFingerprint"), m_documentFingerprint},
            {QStringLiteral("savedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("highlights"), highlights},
        };

        QSaveFile file(m_storePath);
        if (!file.open(QIODevice::WriteOnly)) {
            return false;
        }
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return file.commit();
    }

private:
    bool load()
    {
        m_highlights.clear();
        if (m_storePath.isEmpty() || !QFileInfo::exists(m_storePath)) {
            return true;
        }

        QFile file(m_storePath);
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            return false;
        }

        const QJsonArray highlights = document.object().value(QStringLiteral("highlights")).toArray();
        for (const QJsonValue &value : highlights) {
            const QJsonObject object = value.toObject();
            const int pageIndex = object.value(QStringLiteral("pageIndex")).toInt(-1);
            if (pageIndex < 0) {
                continue;
            }

            QList<QRectF> rects;
            const QJsonArray jsonRects = object.value(QStringLiteral("rects")).toArray();
            rects.reserve(jsonRects.size());
            for (const QJsonValue &rectValue : jsonRects) {
                const QRectF rect = rectFromJson(rectValue.toObject()).normalized();
                if (rect.width() > 0.0 && rect.height() > 0.0) {
                    rects.append(rect);
                }
            }
            if (rects.isEmpty()) {
                continue;
            }

            QColor color(object.value(QStringLiteral("color")).toString(defaultHighlightColor().name(QColor::HexRgb)));
            if (!color.isValid()) {
                color = defaultHighlightColor();
            }

            PdfHighlight highlight;
            highlight.id = object.value(QStringLiteral("id")).toString();
            if (highlight.id.isEmpty()) {
                highlight.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            }
            highlight.pageIndex = pageIndex;
            highlight.rects = rects;
            highlight.color = color;
            highlight.text = object.value(QStringLiteral("text")).toString();
            highlight.note = object.value(QStringLiteral("note")).toString();
            highlight.createdAt = QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(), Qt::ISODateWithMs);
            if (!highlight.createdAt.isValid()) {
                highlight.createdAt = QDateTime::currentDateTimeUtc();
            }
            m_highlights.append(highlight);
        }

        return true;
    }

    QString m_documentPath;
    QString m_canonicalPath;
    QString m_documentFingerprint;
    QString m_storePath;
    QList<PdfHighlight> m_highlights;
};

class ShellPdfView : public QPdfView
{
public:
    explicit ShellPdfView(QWidget *parent = nullptr)
        : QPdfView(parent)
    {
        setContextMenuPolicy(Qt::DefaultContextMenu);
        setFocusPolicy(Qt::StrongFocus);
        viewport()->setFocusPolicy(Qt::StrongFocus);

        m_highlightAction = new QAction(i18n("Highlight"), this);
        m_highlightAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
        m_highlightAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        m_highlightAction->setEnabled(false);
        addAction(m_highlightAction);
        connect(m_highlightAction, &QAction::triggered, this, [this]() {
            highlightSelectedText();
        });

        m_deleteHighlightAction = new QAction(i18n("Delete Highlight"), this);
        m_deleteHighlightAction->setShortcuts(QList<QKeySequence>{QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
        m_deleteHighlightAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        m_deleteHighlightAction->setEnabled(false);
        addAction(m_deleteHighlightAction);
        connect(m_deleteHighlightAction, &QAction::triggered, this, [this]() {
            deleteSelectedHighlight();
        });

        connect(this, &QPdfView::zoomFactorChanged, this, [this]() {
            viewport()->update();
        });
        connect(this, &QPdfView::zoomModeChanged, this, [this]() {
            viewport()->update();
        });
        connect(this, &QPdfView::pageModeChanged, this, [this]() {
            viewport()->update();
        });
        connect(this, &QPdfView::pageSpacingChanged, this, [this]() {
            viewport()->update();
        });
        connect(this, &QPdfView::documentMarginsChanged, this, [this]() {
            viewport()->update();
        });
    }

    void setShellDocument(QPdfDocument *document)
    {
        QPdfView::setDocument(document);
        m_linkModel.setDocument(document);
        clearTextSelection();
    }

    void setHighlightDocument(const QString &path)
    {
        m_highlightStore.setDocumentPath(path);
        m_selectedHighlightIndex = -1;
        updateHighlightActionStates();
        viewport()->update();
    }

    void clearHighlightDocument()
    {
        m_highlightStore.clear();
        m_selectedHighlightIndex = -1;
        updateHighlightActionStates();
        viewport()->update();
    }

    void clearTextSelection()
    {
        m_pageSelections.clear();
        m_selectedText.clear();
        updateHighlightActionStates();
        viewport()->update();
    }

    bool hasSelectedText() const
    {
        return !m_selectedText.isEmpty();
    }

    bool copySelectedTextToClipboard() const
    {
        if (m_selectedText.isEmpty()) {
            return false;
        }
        QGuiApplication::clipboard()->setText(m_selectedText);
        return true;
    }

    void selectAllTextToClipboard()
    {
        if (!document() || document()->status() != QPdfDocument::Status::Ready) {
            return;
        }

        QStringList pages;
        pages.reserve(document()->pageCount());
        for (int page = 0; page < document()->pageCount(); ++page) {
            const QPdfSelection selection = document()->getAllText(page);
            if (selection.isValid() && !selection.text().isEmpty()) {
                pages << selection.text();
            }
        }

        m_pageSelections.clear();
        m_selectedText = pages.join(QStringLiteral("\n\n"));
        if (!m_selectedText.isEmpty()) {
            QGuiApplication::clipboard()->setText(m_selectedText);
        }
        updateHighlightActionStates();
        viewport()->update();
    }

    void zoomBy(qreal multiplier, QPointF viewportAnchor = QPointF())
    {
        if (qFuzzyIsNull(multiplier) || multiplier <= 0.0) {
            return;
        }

        if (viewportAnchor.isNull()) {
            viewportAnchor = QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0);
        }

        setZoomFactorAnchored(currentEffectiveZoomAt(viewportAnchor) * multiplier, viewportAnchor);
    }

    void setZoomFactorAnchored(qreal factor, QPointF viewportAnchor = QPointF())
    {
        factor = qBound<qreal>(MinZoom, factor, MaxZoom);
        if (viewportAnchor.isNull()) {
            viewportAnchor = QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0);
        }

        const PdfPagePoint anchorPoint = pagePointForViewportPosition(viewportAnchor);
        const QPointF fallbackDocumentAnchor = viewportAnchor + QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
        const qreal oldEffectiveZoom = currentEffectiveZoomAt(viewportAnchor);

        setZoomMode(QPdfView::ZoomMode::Custom);
        setZoomFactor(factor);

        if (anchorPoint.valid) {
            const QPointF documentAnchor = documentPositionForPagePoint(anchorPoint);
            horizontalScrollBar()->setValue(qRound(documentAnchor.x() - viewportAnchor.x()));
            verticalScrollBar()->setValue(qRound(documentAnchor.y() - viewportAnchor.y()));
        } else {
            const qreal multiplier = qFuzzyIsNull(oldEffectiveZoom) ? 1.0 : factor / oldEffectiveZoom;
            horizontalScrollBar()->setValue(qRound(fallbackDocumentAnchor.x() * multiplier - viewportAnchor.x()));
            verticalScrollBar()->setValue(qRound(fallbackDocumentAnchor.y() * multiplier - viewportAnchor.y()));
        }
    }

    void scrollToLink(const QPdfLink &link)
    {
        if (!link.isValid() || link.page() < 0 || !document() || link.page() >= document()->pageCount()) {
            return;
        }

        const PdfDocumentLayout layout = calculateDocumentLayout();
        const auto it = layout.pages.constFind(link.page());
        if (it == layout.pages.cend()) {
            return;
        }

        QPointF targetPoint = link.location();
        if (!link.rectangles().isEmpty()) {
            targetPoint = link.rectangles().constFirst().center();
        }

        const qreal scale = screenResolution() * it.value().zoom;
        const QPointF documentPoint = QPointF(it.value().geometry.topLeft()) + targetPoint * scale;
        pageNavigator()->jump(link.page(), link.location(), link.zoom());
        horizontalScrollBar()->setValue(qRound(documentPoint.x() - viewport()->width() / 2.0));
        verticalScrollBar()->setValue(qRound(documentPoint.y() - viewport()->height() / 3.0));
    }

protected:
    bool event(QEvent *event) override
    {
        if (handleNativeGesture(event)) {
            return true;
        }
        return QPdfView::event(event);
    }

    bool viewportEvent(QEvent *event) override
    {
        if (handleNativeGesture(event)) {
            return true;
        }
        return QPdfView::viewportEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QPdfView::paintEvent(event);

        if (m_highlightStore.highlights().isEmpty() && m_pageSelections.isEmpty()) {
            return;
        }

        const PdfDocumentLayout layout = calculateDocumentLayout();
        QPainter painter(viewport());
        painter.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());

        drawPersistentHighlights(&painter, layout);

        if (m_pageSelections.isEmpty()) {
            return;
        }

        QColor selectionColor = palette().highlight().color();
        selectionColor.setAlpha(95);
        painter.setPen(Qt::NoPen);
        painter.setBrush(selectionColor);

        for (const PdfTextSelection &pageSelection : std::as_const(m_pageSelections)) {
            const auto it = layout.pages.constFind(pageSelection.page);
            if (it == layout.pages.cend()) {
                continue;
            }

            const qreal scale = screenResolution() * it.value().zoom;
            const QTransform transform = QTransform::fromScale(scale, scale);
            for (QPolygonF polygon : pageSelection.selection.bounds()) {
                polygon = transform.map(polygon);
                polygon.translate(it.value().geometry.topLeft());
                painter.drawPolygon(polygon);
            }
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            setFocus(Qt::MouseFocusReason);
            m_pressPosition = event->position();
            m_linkPressed = linkAtViewportPosition(event->position()).isValid();
            m_highlightPressed = false;
            if (!m_linkPressed) {
                const int highlightIndex = highlightAtViewportPosition(event->position());
                if (highlightIndex >= 0) {
                    clearTextSelection();
                    setSelectedHighlightIndex(highlightIndex);
                    m_highlightPressed = true;
                    event->accept();
                    return;
                }
                setSelectedHighlightIndex(-1);
                beginTextSelection(event->position());
            }
            event->accept();
            return;
        }

        QPdfView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons() & Qt::LeftButton) {
            if (m_linkPressed && (event->position() - m_pressPosition).manhattanLength() >= QApplication::startDragDistance()) {
                m_linkPressed = false;
                beginTextSelection(m_pressPosition);
            }
            if (m_highlightPressed && (event->position() - m_pressPosition).manhattanLength() >= QApplication::startDragDistance()) {
                m_highlightPressed = false;
                beginTextSelection(m_pressPosition);
            }
            if (m_selectingText) {
                updateTextSelection(event->position());
                event->accept();
                return;
            }
        }

        setCursor(linkAtViewportPosition(event->position()).isValid() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            if (m_selectingText) {
                if ((event->position() - m_pressPosition).manhattanLength() >= QApplication::startDragDistance()) {
                    updateTextSelection(event->position());
                } else {
                    clearTextSelection();
                }
                m_selectingText = false;
                event->accept();
                return;
            }

            if (m_highlightPressed) {
                m_highlightPressed = false;
                event->accept();
                return;
            }

            if (m_linkPressed) {
                m_linkPressed = false;
                const QPdfLink link = linkAtViewportPosition(event->position());
                if (activateLink(link)) {
                    event->accept();
                    return;
                }
            }
        }

        QPdfView::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && deleteSelectedHighlight()) {
            event->accept();
            return;
        }

        QPdfView::keyPressEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QPdfView::resizeEvent(event);
        viewport()->update();
    }

    void scrollContentsBy(int dx, int dy) override
    {
        QPdfView::scrollContentsBy(dx, dy);
        viewport()->update();
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        const int contextHighlightIndex = highlightAtViewportPosition(event->pos());
        if (contextHighlightIndex >= 0) {
            clearTextSelection();
            setSelectedHighlightIndex(contextHighlightIndex);
        }

        QMenu menu(this);
        m_highlightAction->setEnabled(hasHighlightableSelection());
        m_deleteHighlightAction->setEnabled(hasSelectedHighlight());
        menu.addAction(m_highlightAction);
        menu.addAction(m_deleteHighlightAction);
        menu.addSeparator();
        QAction *const copyAction = menu.addAction(i18n("Copy"));
        copyAction->setEnabled(hasSelectedText());
        QAction *const selectAllAction = menu.addAction(i18n("Select All"));

        QAction *const chosen = menu.exec(event->globalPos());
        if (chosen == copyAction) {
            copySelectedTextToClipboard();
        } else if (chosen == selectAllAction) {
            selectAllTextToClipboard();
        }
        updateHighlightActionStates();
        event->accept();
    }

private:
    qreal screenResolution() const
    {
        const QScreen *screen = QGuiApplication::primaryScreen();
        return screen ? screen->logicalDotsPerInch() / 72.0 : 1.0;
    }

    PdfDocumentLayout calculateDocumentLayout() const
    {
        PdfDocumentLayout layout;
        if (!document() || document()->status() != QPdfDocument::Status::Ready) {
            return layout;
        }

        const int pageCount = document()->pageCount();
        const int startPage = pageMode() == QPdfView::PageMode::SinglePage ? pageNavigator()->currentPage() : 0;
        const int endPage = pageMode() == QPdfView::PageMode::SinglePage ? qMin(pageNavigator()->currentPage() + 1, pageCount) : pageCount;

        int totalWidth = 0;
        const qreal resolution = screenResolution();
        const QMargins margins = documentMargins();

        for (int page = startPage; page < endPage; ++page) {
            QSize pageSize;
            qreal pageZoom = zoomFactor();
            const QSizeF pointSize = document()->pagePointSize(page);
            const QSize baseSize = QSizeF(pointSize * resolution).toSize();

            if (zoomMode() == QPdfView::ZoomMode::Custom) {
                pageSize = QSizeF(pointSize * resolution * zoomFactor()).toSize();
            } else if (zoomMode() == QPdfView::ZoomMode::FitToWidth) {
                pageZoom = baseSize.width() > 0 ? qreal(viewport()->width() - margins.left() - margins.right()) / qreal(baseSize.width()) : 1.0;
                pageSize = baseSize;
                pageSize *= pageZoom;
            } else {
                const QSize available = viewport()->size() + QSize(-margins.left() - margins.right(), -pageSpacing());
                pageSize = baseSize.scaled(available, Qt::KeepAspectRatio);
                pageZoom = baseSize.width() > 0 ? qreal(pageSize.width()) / qreal(baseSize.width()) : 1.0;
            }

            totalWidth = qMax(totalWidth, pageSize.width());
            layout.pages.insert(page, PdfPageLayout{QRect(QPoint(0, 0), pageSize), pageZoom});
        }

        totalWidth += margins.left() + margins.right();

        int pageY = margins.top();
        for (int page = startPage; page < endPage; ++page) {
            auto it = layout.pages.find(page);
            if (it == layout.pages.end()) {
                continue;
            }

            const QSize pageSize = it.value().geometry.size();
            const int pageX = (qMax(totalWidth, viewport()->width()) - pageSize.width()) / 2;
            it.value().geometry.moveTopLeft(QPoint(pageX, pageY));
            pageY += pageSize.height() + pageSpacing();
        }
        pageY += margins.bottom();
        layout.documentSize = QSize(totalWidth, pageY);
        return layout;
    }

    QRectF normalizedRectForPageRect(const QRectF &pageRect, const QSizeF &pageSize) const
    {
        if (pageSize.width() <= 0.0 || pageSize.height() <= 0.0) {
            return {};
        }

        const QRectF clipped = pageRect.normalized().intersected(QRectF(QPointF(0, 0), pageSize));
        if (clipped.width() <= 0.0 || clipped.height() <= 0.0) {
            return {};
        }

        return QRectF(clipped.x() / pageSize.width(),
                      clipped.y() / pageSize.height(),
                      clipped.width() / pageSize.width(),
                      clipped.height() / pageSize.height());
    }

    QRectF pageRectForNormalizedRect(const QRectF &normalizedRect, const QSizeF &pageSize) const
    {
        if (pageSize.width() <= 0.0 || pageSize.height() <= 0.0) {
            return {};
        }

        const QRectF normalized = normalizedRect.normalized();
        const qreal left = qBound<qreal>(0.0, normalized.left(), 1.0);
        const qreal top = qBound<qreal>(0.0, normalized.top(), 1.0);
        const qreal right = qBound<qreal>(0.0, normalized.right(), 1.0);
        const qreal bottom = qBound<qreal>(0.0, normalized.bottom(), 1.0);
        if (right <= left || bottom <= top) {
            return {};
        }

        return QRectF(left * pageSize.width(),
                      top * pageSize.height(),
                      (right - left) * pageSize.width(),
                      (bottom - top) * pageSize.height());
    }

    QRectF documentRectForHighlightRect(const PdfDocumentLayout &layout, int pageIndex, const QRectF &normalizedRect) const
    {
        if (!document() || pageIndex < 0 || pageIndex >= document()->pageCount()) {
            return {};
        }

        const auto pageIt = layout.pages.constFind(pageIndex);
        if (pageIt == layout.pages.cend()) {
            return {};
        }

        const QRectF pageRect = pageRectForNormalizedRect(normalizedRect, document()->pagePointSize(pageIndex));
        if (pageRect.isEmpty()) {
            return {};
        }

        const qreal scale = screenResolution() * pageIt.value().zoom;
        return QRectF(QPointF(pageIt.value().geometry.left() + pageRect.x() * scale,
                              pageIt.value().geometry.top() + pageRect.y() * scale),
                      QSizeF(pageRect.width() * scale, pageRect.height() * scale));
    }

    void drawPersistentHighlights(QPainter *painter, const PdfDocumentLayout &layout) const
    {
        const QList<PdfHighlight> &highlights = m_highlightStore.highlights();
        if (!painter || highlights.isEmpty()) {
            return;
        }

        const QRectF visibleDocumentRect(QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value()), QSizeF(viewport()->size()));

        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setCompositionMode(QPainter::CompositionMode_Multiply);

        for (int index = 0; index < highlights.size(); ++index) {
            const PdfHighlight &highlight = highlights.at(index);
            const auto pageIt = layout.pages.constFind(highlight.pageIndex);
            if (pageIt == layout.pages.cend() || !QRectF(pageIt.value().geometry).intersects(visibleDocumentRect)) {
                continue;
            }

            QColor color = highlight.color.isValid() ? highlight.color : defaultHighlightColor();
            color.setAlphaF(0.35);
            for (const QRectF &normalizedRect : highlight.rects) {
                const QRectF documentRect = documentRectForHighlightRect(layout, highlight.pageIndex, normalizedRect);
                if (!documentRect.isEmpty() && documentRect.intersects(visibleDocumentRect)) {
                    painter->fillRect(documentRect, color);
                }
            }
        }

        painter->restore();

        if (!hasSelectedHighlight()) {
            return;
        }

        const PdfHighlight &highlight = highlights.at(m_selectedHighlightIndex);
        painter->save();
        QColor outline = highlight.color.isValid() ? highlight.color.darker(180) : defaultHighlightColor().darker(180);
        outline.setAlpha(190);
        painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(outline, 1.5));
        for (const QRectF &normalizedRect : highlight.rects) {
            const QRectF documentRect = documentRectForHighlightRect(layout, highlight.pageIndex, normalizedRect);
            if (!documentRect.isEmpty() && documentRect.intersects(visibleDocumentRect)) {
                painter->drawRect(documentRect.adjusted(-1.0, -1.0, 1.0, 1.0));
            }
        }
        painter->restore();
    }

    int highlightAtViewportPosition(const QPointF &viewportPosition) const
    {
        const QList<PdfHighlight> &highlights = m_highlightStore.highlights();
        if (highlights.isEmpty()) {
            return -1;
        }

        const QPointF documentPosition = viewportPosition + QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
        const PdfDocumentLayout layout = calculateDocumentLayout();
        for (int index = highlights.size() - 1; index >= 0; --index) {
            const PdfHighlight &highlight = highlights.at(index);
            for (const QRectF &normalizedRect : highlight.rects) {
                const QRectF documentRect = documentRectForHighlightRect(layout, highlight.pageIndex, normalizedRect);
                if (!documentRect.isEmpty() && documentRect.adjusted(-2.0, -2.0, 2.0, 2.0).contains(documentPosition)) {
                    return index;
                }
            }
        }

        return -1;
    }

    PdfPagePoint pagePointForViewportPosition(const QPointF &viewportPosition) const
    {
        const QPointF documentPosition = viewportPosition + QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
        const PdfDocumentLayout layout = calculateDocumentLayout();
        const int pages = document() ? document()->pageCount() : 0;
        for (int page = 0; page < pages; ++page) {
            const auto it = layout.pages.constFind(page);
            if (it == layout.pages.cend() || !it.value().geometry.contains(documentPosition.toPoint())) {
                continue;
            }

            const qreal scale = screenResolution() * it.value().zoom;
            if (qFuzzyIsNull(scale)) {
                return {};
            }
            return PdfPagePoint{true, page, (documentPosition - QPointF(it.value().geometry.topLeft())) / scale};
        }
        return {};
    }

    QPointF documentPositionForPagePoint(const PdfPagePoint &pagePoint) const
    {
        if (!pagePoint.valid) {
            return {};
        }

        const PdfDocumentLayout layout = calculateDocumentLayout();
        const auto it = layout.pages.constFind(pagePoint.page);
        if (it == layout.pages.cend()) {
            return {};
        }

        const qreal scale = screenResolution() * it.value().zoom;
        return QPointF(it.value().geometry.topLeft()) + pagePoint.point * scale;
    }

    qreal currentEffectiveZoomAt(const QPointF &viewportPosition) const
    {
        const PdfPagePoint pagePoint = pagePointForViewportPosition(viewportPosition);
        const PdfDocumentLayout layout = calculateDocumentLayout();
        if (pagePoint.valid) {
            const auto it = layout.pages.constFind(pagePoint.page);
            if (it != layout.pages.cend()) {
                return it.value().zoom;
            }
        }

        const int currentPage = pageNavigator() ? pageNavigator()->currentPage() : 0;
        const auto it = layout.pages.constFind(currentPage);
        return it == layout.pages.cend() ? zoomFactor() : it.value().zoom;
    }

    bool hasHighlightableSelection() const
    {
        return !m_pageSelections.isEmpty();
    }

    bool hasSelectedHighlight() const
    {
        return m_selectedHighlightIndex >= 0 && m_selectedHighlightIndex < m_highlightStore.highlights().size();
    }

    void updateHighlightActionStates()
    {
        if (m_highlightAction) {
            m_highlightAction->setEnabled(hasHighlightableSelection());
        }
        if (m_deleteHighlightAction) {
            m_deleteHighlightAction->setEnabled(hasSelectedHighlight());
        }
    }

    void setSelectedHighlightIndex(int index)
    {
        const int nextIndex = index >= 0 && index < m_highlightStore.highlights().size() ? index : -1;
        if (m_selectedHighlightIndex == nextIndex) {
            updateHighlightActionStates();
            return;
        }

        m_selectedHighlightIndex = nextIndex;
        updateHighlightActionStates();
        viewport()->update();
    }

    bool highlightSelectedText()
    {
        if (!document() || document()->status() != QPdfDocument::Status::Ready || !hasHighlightableSelection()) {
            return false;
        }

        const QDateTime createdAt = QDateTime::currentDateTimeUtc();
        int lastHighlightIndex = -1;
        for (const PdfTextSelection &pageSelection : std::as_const(m_pageSelections)) {
            if (pageSelection.page < 0 || pageSelection.page >= document()->pageCount()) {
                continue;
            }

            const QSizeF pageSize = document()->pagePointSize(pageSelection.page);
            QList<QRectF> normalizedRects;
            for (const QPolygonF &bounds : pageSelection.selection.bounds()) {
                const QRectF normalizedRect = normalizedRectForPageRect(bounds.boundingRect(), pageSize);
                if (!normalizedRect.isEmpty()) {
                    normalizedRects.append(normalizedRect);
                }
            }

            if (normalizedRects.isEmpty()) {
                continue;
            }

            lastHighlightIndex = m_highlightStore.addHighlight(pageSelection.page,
                                                               normalizedRects,
                                                               defaultHighlightColor(),
                                                               pageSelection.selection.text(),
                                                               QString(),
                                                               createdAt);
        }

        if (lastHighlightIndex < 0) {
            return false;
        }

        m_highlightStore.save();
        clearTextSelection();
        setSelectedHighlightIndex(lastHighlightIndex);
        viewport()->update();
        return true;
    }

    bool deleteSelectedHighlight()
    {
        if (!hasSelectedHighlight()) {
            return false;
        }

        const bool removed = m_highlightStore.removeHighlight(m_selectedHighlightIndex);
        if (!removed) {
            return false;
        }

        m_selectedHighlightIndex = -1;
        m_highlightStore.save();
        updateHighlightActionStates();
        viewport()->update();
        return true;
    }

    QPdfLink linkAtViewportPosition(const QPointF &viewportPosition)
    {
        const PdfPagePoint pagePoint = pagePointForViewportPosition(viewportPosition);
        if (!pagePoint.valid) {
            return {};
        }

        m_linkModel.setPage(pagePoint.page);
        return m_linkModel.linkAt(pagePoint.point);
    }

    bool activateLink(const QPdfLink &link)
    {
        if (!link.isValid()) {
            return false;
        }

        if (!link.url().isEmpty()) {
            if (shouldOpenExternalPdfUrl(link.url())) {
                QDesktopServices::openUrl(link.url());
            }
            return true;
        }

        if (document() && link.page() >= 0 && link.page() < document()->pageCount()) {
            scrollToLink(link);
            return true;
        }

        return false;
    }

    void beginTextSelection(const QPointF &viewportPosition)
    {
        const PdfPagePoint pagePoint = pagePointForViewportPosition(viewportPosition);
        if (!pagePoint.valid) {
            clearTextSelection();
            m_selectingText = false;
            return;
        }

        m_selectionAnchor = pagePoint;
        m_selectionActive = pagePoint;
        m_selectingText = true;
        clearTextSelection();
    }

    void updateTextSelection(const QPointF &viewportPosition)
    {
        if (!document() || document()->status() != QPdfDocument::Status::Ready || !m_selectionAnchor.valid) {
            return;
        }

        const PdfPagePoint activePoint = pagePointForViewportPosition(viewportPosition);
        if (!activePoint.valid) {
            return;
        }
        m_selectionActive = activePoint;

        m_pageSelections.clear();
        QStringList selectedPages;

        const PdfPagePoint first = m_selectionAnchor.page <= m_selectionActive.page ? m_selectionAnchor : m_selectionActive;
        const PdfPagePoint last = m_selectionAnchor.page <= m_selectionActive.page ? m_selectionActive : m_selectionAnchor;

        for (int page = first.page; page <= last.page; ++page) {
            QPdfSelection selection = (page == first.page && page == last.page) ? document()->getSelection(page, first.point, last.point) : pageRangeSelection(page, first, last);
            if (!selection.isValid() || selection.text().isEmpty()) {
                continue;
            }
            m_pageSelections.append(PdfTextSelection{page, selection});
            selectedPages << selection.text();
        }

        m_selectedText = selectedPages.join(QStringLiteral("\n\n"));
        updateHighlightActionStates();
        viewport()->update();
    }

    QPdfSelection pageRangeSelection(int page, const PdfPagePoint &first, const PdfPagePoint &last) const
    {
        const QSizeF pageSize = document()->pagePointSize(page);
        if (page == first.page) {
            return document()->getSelection(page, first.point, QPointF(pageSize.width(), pageSize.height()));
        }
        if (page == last.page) {
            return document()->getSelection(page, QPointF(0, 0), last.point);
        }
        return document()->getAllText(page);
    }

    bool handleNativeGesture(QEvent *event)
    {
        if (event->type() != QEvent::NativeGesture) {
            return false;
        }

        auto *gesture = static_cast<QNativeGestureEvent *>(event);
        if (gesture->gestureType() != Qt::ZoomNativeGesture) {
            return false;
        }

        const qreal multiplier = qBound<qreal>(0.5, 1.0 + gesture->value(), 2.0);
        zoomBy(multiplier, gesture->position());
        event->accept();
        return true;
    }

    QPdfLinkModel m_linkModel;
    PdfHighlightStore m_highlightStore;
    QList<PdfTextSelection> m_pageSelections;
    QString m_selectedText;
    PdfPagePoint m_selectionAnchor;
    PdfPagePoint m_selectionActive;
    QPointF m_pressPosition;
    QAction *m_highlightAction = nullptr;
    QAction *m_deleteHighlightAction = nullptr;
    int m_selectedHighlightIndex = -1;
    bool m_selectingText = false;
    bool m_linkPressed = false;
    bool m_highlightPressed = false;
};

QString pdfErrorText(QPdfDocument::Error error)
{
    switch (error) {
    case QPdfDocument::Error::None:
        return QString();
    case QPdfDocument::Error::DataNotYetAvailable:
        return i18n("The PDF data is not available yet.");
    case QPdfDocument::Error::FileNotFound:
        return i18n("The PDF file was not found.");
    case QPdfDocument::Error::InvalidFileFormat:
        return i18n("The file is not a valid PDF document.");
    case QPdfDocument::Error::IncorrectPassword:
        return i18n("This PDF requires a password.");
    case QPdfDocument::Error::UnsupportedSecurityScheme:
        return i18n("This PDF uses an unsupported security scheme.");
    case QPdfDocument::Error::Unknown:
        return i18n("The PDF could not be opened.");
    }
    return i18n("The PDF could not be opened.");
}

QString positionKeyForUrl(const QUrl &url)
{
    return QString::fromLatin1(QCryptographicHash::hash(url.toEncoded(QUrl::FullyEncoded), QCryptographicHash::Sha256).toHex());
}
}

PdfView::PdfView(QWidget *parent)
    : QWidget(parent)
    , m_document(new QPdfDocument(this))
    , m_view(new ShellPdfView(this))
    , m_searchModel(new QPdfSearchModel(this))
    , m_bookmarkModel(new QPdfBookmarkModel(this))
    , m_bookmarkTitleModel(new BookmarkTitleModel(this))
    , m_outlineWidget(new QWidget)
    , m_outlineView(new QTreeView(m_outlineWidget))
    , m_aiNavigationModel(new QStandardItemModel(this))
    , m_claudeExecutable(ClaudeProcessHelper::findClaudeExecutable())
{
    setObjectName(QStringLiteral("paperlibrary_pdf_view"));
    setAttribute(Qt::WA_StyledBackground, true);

    m_view->setObjectName(QStringLiteral("paperlibrary_pdf_qpdfview"));
    static_cast<ShellPdfView *>(m_view)->setShellDocument(m_document);
    m_view->setPageMode(QPdfView::PageMode::MultiPage);
    m_view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    m_view->setPageSpacing(10);
    m_view->setDocumentMargins(QMargins(20, 20, 20, 20));
    m_searchModel->setDocument(m_document);
    m_view->setSearchModel(m_searchModel);
    m_pageTransitionOverlay = new QLabel(m_view->viewport());
    m_pageTransitionOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_pageTransitionOverlay->setScaledContents(false);
    m_pageTransitionOverlay->hide();

    setupFindBar();
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_findBar);
    layout->addWidget(m_view);

    m_bookmarkModel->setDocument(m_document);
    m_bookmarkTitleModel->setSourceModel(m_bookmarkModel);

    m_outlineWidget->setObjectName(QStringLiteral("paperlibrary_pdf_outline"));
    m_outlineWidget->setAttribute(Qt::WA_StyledBackground, true);
    m_outlineWidget->setStyleSheet(QStringLiteral(R"(
        QWidget#paperlibrary_pdf_outline {
            background: palette(window);
        }
        QLabel#paperlibrary_pdf_outline_title,
        QLabel#paperlibrary_pdf_ai_navigation_title {
            font-weight: 600;
            color: palette(text);
        }
        QLabel#paperlibrary_pdf_ai_navigation_status {
            color: palette(mid);
        }
        QTreeView#paperlibrary_pdf_outline_view,
        QTreeView#paperlibrary_pdf_ai_navigation_view {
            border: 0;
            background: transparent;
            outline: 0;
            padding: 2px 4px 4px 4px;
        }
        QToolButton#paperlibrary_pdf_ai_navigation_button {
            border: 1px solid palette(midlight);
            border-radius: 5px;
            padding: 4px 8px;
        }
    )"));
    auto *outlineLayout = new QVBoxLayout(m_outlineWidget);
    outlineLayout->setContentsMargins(0, 6, 0, 0);
    outlineLayout->setSpacing(2);

    m_outlineTitle = new QLabel(i18n("Contents"), m_outlineWidget);
    m_outlineTitle->setObjectName(QStringLiteral("paperlibrary_pdf_outline_title"));
    m_outlineTitle->setContentsMargins(12, 10, 10, 4);
    outlineLayout->addWidget(m_outlineTitle);

    m_outlineView->setObjectName(QStringLiteral("paperlibrary_pdf_outline_view"));
    m_outlineView->setModel(m_bookmarkTitleModel);
    configureNavigationTree(m_outlineView);
    outlineLayout->addWidget(m_outlineView, 1);

    auto *aiNavigationHeader = new QWidget(m_outlineWidget);
    aiNavigationHeader->setObjectName(QStringLiteral("paperlibrary_pdf_ai_navigation_header"));
    auto *aiNavigationHeaderLayout = new QHBoxLayout(aiNavigationHeader);
    aiNavigationHeaderLayout->setContentsMargins(12, 8, 8, 4);
    aiNavigationHeaderLayout->setSpacing(6);

    auto *aiNavigationTitle = new QLabel(i18n("AI Navigation"), aiNavigationHeader);
    aiNavigationTitle->setObjectName(QStringLiteral("paperlibrary_pdf_ai_navigation_title"));
    aiNavigationHeaderLayout->addWidget(aiNavigationTitle);
    aiNavigationHeaderLayout->addStretch();

    m_aiNavigationAction = new QAction(i18n("Generate AI Navigation"), this);
    m_aiNavigationAction->setIcon(QIcon(QStringLiteral(":/shell/icons/chrome-ai-navigation.svg")));
    connect(m_aiNavigationAction, &QAction::triggered, this, &PdfView::generateAiNavigation);

    m_relatedPapersAction = new QAction(i18n("Related Papers"), this);
    m_relatedPapersAction->setIcon(QIcon::fromTheme(QStringLiteral("view-filter")));
    m_relatedPapersAction->setEnabled(false);
    m_relatedPapersAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    m_relatedPapersAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_relatedPapersAction, &QAction::triggered, this, [this]() {
        if (!m_relatedPapersQuery.trimmed().isEmpty()) {
            Q_EMIT relatedPapersRequested(m_relatedPapersQuery.trimmed(), m_relatedPapersLabel.trimmed());
        }
    });
    addAction(m_relatedPapersAction);

    m_aiNavigationButton = new QToolButton(aiNavigationHeader);
    m_aiNavigationButton->setObjectName(QStringLiteral("paperlibrary_pdf_ai_navigation_button"));
    m_aiNavigationButton->setAutoRaise(true);
    m_aiNavigationButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_aiNavigationButton->setDefaultAction(m_aiNavigationAction);
    aiNavigationHeaderLayout->addWidget(m_aiNavigationButton);
    outlineLayout->addWidget(aiNavigationHeader);

    m_aiNavigationProgress = new QProgressBar(m_outlineWidget);
    m_aiNavigationProgress->setRange(0, 0);
    m_aiNavigationProgress->setTextVisible(false);
    m_aiNavigationProgress->setFixedHeight(4);
    m_aiNavigationProgress->hide();
    outlineLayout->addWidget(m_aiNavigationProgress);

    m_aiNavigationStatusLabel = new QLabel(m_outlineWidget);
    m_aiNavigationStatusLabel->setObjectName(QStringLiteral("paperlibrary_pdf_ai_navigation_status"));
    m_aiNavigationStatusLabel->setWordWrap(true);
    m_aiNavigationStatusLabel->setContentsMargins(10, 2, 10, 6);
    m_aiNavigationStatusLabel->hide();
    outlineLayout->addWidget(m_aiNavigationStatusLabel);

    m_aiNavigationView = new QTreeView(m_outlineWidget);
    m_aiNavigationView->setObjectName(QStringLiteral("paperlibrary_pdf_ai_navigation_view"));
    m_aiNavigationView->setModel(m_aiNavigationModel);
    configureNavigationTree(m_aiNavigationView);
    m_aiNavigationView->hide();
    outlineLayout->addWidget(m_aiNavigationView, 1);

    connect(m_outlineView, &QTreeView::activated, this, &PdfView::jumpToBookmark);
    connect(m_outlineView, &QTreeView::clicked, this, &PdfView::jumpToBookmark);
    connect(m_aiNavigationView, &QTreeView::activated, this, &PdfView::jumpToAiNavigationEntry);
    connect(m_aiNavigationView, &QTreeView::clicked, this, &PdfView::jumpToAiNavigationEntry);

    QPdfPageNavigator *const navigator = m_view->pageNavigator();
    connect(navigator, &QPdfPageNavigator::currentPageChanged, this, &PdfView::emitPageState);
    connect(m_document, &QPdfDocument::pageCountChanged, this, &PdfView::emitPageState);
    connect(m_view, &QPdfView::zoomFactorChanged, this, &PdfView::emitZoomState);
    connect(m_view, &QPdfView::zoomModeChanged, this, &PdfView::emitZoomState);
    connect(m_searchModel, &QAbstractItemModel::modelReset, this, &PdfView::updateFindState);
    connect(m_searchModel, &QAbstractItemModel::rowsInserted, this, &PdfView::updateFindState);
    connect(m_searchModel, &QAbstractItemModel::rowsRemoved, this, &PdfView::updateFindState);
    connect(m_searchModel, &QAbstractItemModel::dataChanged, this, &PdfView::updateFindState);

    const auto outlineChanged = [this]() {
        m_outlineView->expandToDepth(0);
        updateAiNavigationUi();
        emitOutlineState();
    };
    connect(m_bookmarkModel, &QAbstractItemModel::modelReset, this, outlineChanged);
    connect(m_bookmarkModel, &QAbstractItemModel::rowsInserted, this, outlineChanged);
    connect(m_bookmarkModel, &QAbstractItemModel::rowsRemoved, this, outlineChanged);
    connect(m_document, &QPdfDocument::statusChanged, this, [this](QPdfDocument::Status status) {
        if (status == QPdfDocument::Status::Ready) {
            restoreReadingPosition();
            emitPageState();
            emitZoomState();
            emitOutlineState();
            Q_EMIT loadFinished(true);
        } else if (status == QPdfDocument::Status::Error) {
            Q_EMIT errorOccurred(pdfErrorText(m_document->error()));
            Q_EMIT loadFinished(false);
        }
    });
    updateAiNavigationActionState();
    updateAiNavigationUi();
}

PdfView::~PdfView()
{
    // QPdfDocument::close() resets QPdfView's scroll bars/page navigator and the models attached
    // to the document. QObject's automatic receiver disconnection happens in ~QObject, which is
    // too late for a PdfView slot: by then this derived subobject has already been destroyed. Tear
    // down the complete QtPdf dependency graph now, while `this` is still a valid PdfView.
    cancelAiNavigationRun();

    if (m_view) {
        if (QPdfPageNavigator *const navigator = m_view->pageNavigator()) {
            navigator->disconnect(this);
        }
        m_view->disconnect(this);
    }
    if (m_searchModel) {
        m_searchModel->disconnect(this);
    }
    if (m_bookmarkModel) {
        m_bookmarkModel->disconnect(this);
    }
    if (m_document) {
        m_document->disconnect(this);
    }

    // The outline can be reparented into the Shell sidebar. Delete it before its proxy/model,
    // unless the sidebar already did so (QPointer then auto-nulls m_outlineWidget).
    if (m_outlineWidget) {
        delete m_outlineWidget;
    }

    // Do not leave the ordering to QObject child destruction. Destroy every document consumer
    // while its QPdfDocument is still valid; only then close and destroy the document itself.
    delete m_view;
    m_view = nullptr;
    m_pageTransitionOverlay = nullptr;
    delete m_bookmarkTitleModel;
    m_bookmarkTitleModel = nullptr;
    delete m_bookmarkModel;
    m_bookmarkModel = nullptr;
    delete m_searchModel;
    m_searchModel = nullptr;
    if (m_document) {
        m_document->close();
        delete m_document;
        m_document = nullptr;
    }
}

bool PdfView::canOpen(const QUrl &url)
{
    if (!url.isLocalFile()) {
        return false;
    }

    const QFileInfo info(url.toLocalFile());
    if (!info.isFile() || !info.isReadable()) {
        return false;
    }

    const QMimeType mime = QMimeDatabase().mimeTypeForFile(info);
    return mime.inherits(QStringLiteral("application/pdf"));
}

bool PdfView::readerMotionEnabled()
{
    const KConfigGroup group = KSharedConfig::openConfig()->group(QString::fromLatin1(GeneralGroup));
    return group.readEntry(ReaderMotionKey, true);
}

void PdfView::setReaderMotionEnabled(bool enabled)
{
    KConfigGroup group = KSharedConfig::openConfig()->group(QString::fromLatin1(GeneralGroup));
    group.writeEntry(ReaderMotionKey, enabled);
    group.sync();
}

bool PdfView::open(const QUrl &url)
{
    TelemetryScope op(QStringLiteral("pdf_open"));
    if (!canOpen(url)) {
        return false;
    }

    saveReadingPosition();

    m_url = url;
    m_pdfPath = url.toLocalFile();
    m_title = QFileInfo(m_pdfPath).fileName();
    m_havePendingRestore = false;
    cancelAiNavigationRun();
    m_aiNavigationCachePath.clear();
    m_aiNavigationFingerprintHash.clear();
    m_aiNavigationExtractedPages.clear();
    m_aiNavigationEntries.clear();
    m_aiNavigationTextTruncated = false;
    m_aiNavigationStatusVisible = false;
    rebuildAiNavigationModel();
    updateAiNavigationUi();
    updateAiNavigationActionState();

    resetFindState();
    auto *shellView = static_cast<ShellPdfView *>(m_view);
    shellView->clearTextSelection();
    shellView->clearHighlightDocument();
    m_document->close();
    m_view->setPageMode(QPdfView::PageMode::MultiPage);
    m_view->setZoomMode(QPdfView::ZoomMode::FitToWidth);

    const QPdfDocument::Error error = m_document->load(m_pdfPath);
    if (error != QPdfDocument::Error::None) {
        shellView->clearHighlightDocument();
        Q_EMIT errorOccurred(pdfErrorText(error));
        Q_EMIT loadFinished(false);
        return false;
    }
    shellView->setHighlightDocument(m_pdfPath);
    resetAiNavigationForDocument();

    Q_EMIT titleChanged(m_title);
    restoreReadingPosition();
    emitPageState();
    emitZoomState();
    emitOutlineState();
    Q_EMIT loadFinished(true);
    return true;
}

QUrl PdfView::url() const
{
    return m_url;
}

void PdfView::reload()
{
    if (m_url.isEmpty()) {
        return;
    }

    const QUrl reloadUrl = m_url;
    open(reloadUrl);
}

QWidget *PdfView::outlineWidget() const
{
    return m_outlineWidget;
}

bool PdfView::hasOutline() const
{
    const bool hasRealOutline = m_bookmarkModel && m_bookmarkModel->rowCount() > 0;
    const bool hasAiNavigation = m_aiNavigationModel && m_aiNavigationModel->rowCount() > 0;
    return hasRealOutline || hasAiNavigation || m_aiNavigationRunning || m_aiNavigationStatusVisible;
}

int PdfView::pageCount() const
{
    return m_document ? m_document->pageCount() : 0;
}

int PdfView::currentPageOneBased() const
{
    if (!m_view || !m_view->pageNavigator() || pageCount() <= 0) {
        return 0;
    }
    return qBound(0, m_view->pageNavigator()->currentPage(), pageCount() - 1) + 1;
}

qreal PdfView::zoomFactor() const
{
    return m_view ? m_view->zoomFactor() : 1.0;
}

bool PdfView::fitWidthMode() const
{
    return m_view && m_view->zoomMode() == QPdfView::ZoomMode::FitToWidth;
}

QAction *PdfView::findAction() const
{
    return m_findAction;
}

QAction *PdfView::aiNavigationAction() const
{
    return m_aiNavigationAction;
}

QAction *PdfView::relatedPapersAction() const
{
    return m_relatedPapersAction;
}

void PdfView::setRelatedPapersContext(const QString &query, const QString &label)
{
    m_relatedPapersQuery = query.trimmed();
    m_relatedPapersLabel = label.trimmed();
    if (!m_relatedPapersAction) {
        return;
    }
    const bool enabled = !m_relatedPapersQuery.isEmpty();
    m_relatedPapersAction->setEnabled(enabled);
    if (!enabled) {
        m_relatedPapersAction->setToolTip(i18n("No related-paper index is available for this document yet."));
        return;
    }
    const QString target = m_relatedPapersLabel.isEmpty() ? m_relatedPapersQuery : m_relatedPapersLabel;
    m_relatedPapersAction->setToolTip(i18n("Show papers related to %1.", target));
}

void PdfView::jumpToApproximateProgress(double progress)
{
    const int pages = pageCount();
    if (pages <= 0 || progress < 0.0) {
        return;
    }
    const int page = qBound(0, static_cast<int>(qRound(progress * pages)) - 1, pages - 1);
    jumpToPage(page, QPointF(), zoomFactor(), false);
}

void PdfView::goToPageOneBased(int page)
{
    const int pages = pageCount();
    if (pages <= 0) {
        return;
    }

    const int zeroBasedPage = qBound(1, page, pages) - 1;
    jumpToPage(zeroBasedPage, QPointF(), zoomFactor(), true);
}

void PdfView::jumpToPage(int zeroBasedPage, const QPointF &location, qreal zoom, bool animated)
{
    if (!m_view || !m_view->pageNavigator() || zeroBasedPage < 0 || zeroBasedPage >= pageCount()) {
        return;
    }

    const int currentPage = currentPageOneBased() - 1;
    const int direction = zeroBasedPage > currentPage ? 1 : zeroBasedPage < currentPage ? -1 : 0;
    QPixmap snapshot;
    if (animated && direction != 0 && readerMotionEnabled() && isVisible() && m_view->viewport() && m_view->viewport()->isVisible()) {
        snapshot = m_view->viewport()->grab();
    }

    m_view->pageNavigator()->jump(zeroBasedPage, location, zoom);

    if (!snapshot.isNull()) {
        animatePageTurn(snapshot, direction);
    }
}

void PdfView::animatePageTurn(const QPixmap &snapshot, int direction)
{
    if (!m_pageTransitionOverlay || !m_view || !m_view->viewport()) {
        return;
    }

    if (m_pageTransitionOverlay->graphicsEffect()) {
        m_pageTransitionOverlay->setGraphicsEffect(nullptr);
    }
    m_pageTransitionOverlay->setPixmap(snapshot);
    m_pageTransitionOverlay->setGeometry(m_view->viewport()->rect());
    m_pageTransitionOverlay->move(0, 0);
    m_pageTransitionOverlay->show();
    m_pageTransitionOverlay->raise();

    auto *effect = new QGraphicsOpacityEffect(m_pageTransitionOverlay);
    effect->setOpacity(1.0);
    m_pageTransitionOverlay->setGraphicsEffect(effect);

    const int travel = qBound(36, m_view->viewport()->width() / 10, 96);
    auto *position = new QPropertyAnimation(m_pageTransitionOverlay, "pos", m_pageTransitionOverlay);
    position->setDuration(170);
    position->setStartValue(QPoint(0, 0));
    position->setEndValue(QPoint(direction > 0 ? -travel : travel, 0));
    position->setEasingCurve(QEasingCurve::OutCubic);

    auto *opacity = new QPropertyAnimation(effect, "opacity", m_pageTransitionOverlay);
    opacity->setDuration(170);
    opacity->setStartValue(1.0);
    opacity->setEndValue(0.0);
    opacity->setEasingCurve(QEasingCurve::OutCubic);

    connect(opacity, &QPropertyAnimation::finished, this, [this]() {
        if (!m_pageTransitionOverlay) {
            return;
        }
        m_pageTransitionOverlay->hide();
        m_pageTransitionOverlay->clear();
        m_pageTransitionOverlay->move(0, 0);
    });
    position->start(QAbstractAnimation::DeleteWhenStopped);
    opacity->start(QAbstractAnimation::DeleteWhenStopped);
}

void PdfView::previousPage()
{
    goToPageOneBased(currentPageOneBased() - 1);
}

void PdfView::nextPage()
{
    goToPageOneBased(currentPageOneBased() + 1);
}

void PdfView::zoomIn()
{
    static_cast<ShellPdfView *>(m_view)->zoomBy(1.2);
}

void PdfView::zoomOut()
{
    static_cast<ShellPdfView *>(m_view)->zoomBy(1.0 / 1.2);
}

void PdfView::fitToWidth()
{
    m_view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
}

void PdfView::showFindBar()
{
    if (!m_findBar || !m_findField) {
        return;
    }

    m_findBar->show();
    m_findField->setFocus(Qt::ShortcutFocusReason);
    m_findField->selectAll();
    updateFindState();
}

void PdfView::closeFindBar()
{
    if (!m_findBar) {
        return;
    }

    resetFindState();
    m_findBar->hide();
    m_view->setFocus(Qt::ShortcutFocusReason);
}

void PdfView::findNext()
{
    if (!m_findBar || !m_findBar->isVisible()) {
        showFindBar();
    }
    if (!m_searchModel || m_searchModel->searchString().isEmpty()) {
        if (m_findField) {
            m_findField->setFocus(Qt::ShortcutFocusReason);
        }
        return;
    }

    const int count = m_searchModel->rowCount({});
    if (count <= 0) {
        updateFindState();
        return;
    }

    activateSearchResult((m_currentSearchIndex + 1) % count);
}

void PdfView::findPrevious()
{
    if (!m_findBar || !m_findBar->isVisible()) {
        showFindBar();
    }
    if (!m_searchModel || m_searchModel->searchString().isEmpty()) {
        if (m_findField) {
            m_findField->setFocus(Qt::ShortcutFocusReason);
        }
        return;
    }

    const int count = m_searchModel->rowCount({});
    if (count <= 0) {
        updateFindState();
        return;
    }

    activateSearchResult(m_currentSearchIndex <= 0 ? count - 1 : m_currentSearchIndex - 1);
}

void PdfView::copy()
{
    if (m_findField && m_findField->hasFocus()) {
        m_findField->copy();
        return;
    }

    static_cast<ShellPdfView *>(m_view)->copySelectedTextToClipboard();
}

void PdfView::selectAll()
{
    if (m_findField && m_findField->hasFocus()) {
        m_findField->selectAll();
        return;
    }

    static_cast<ShellPdfView *>(m_view)->selectAllTextToClipboard();
}

void PdfView::saveReadingPosition()
{
    if (m_url.isEmpty() || !m_view || !m_view->pageNavigator()) {
        return;
    }

    KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(PositionsGroup));
    group.writeEntry(positionKey(),
                     QStringList{
                         QString::number(qMax(0, currentPageOneBased() - 1)),
                         QString::number(m_view->verticalScrollBar() ? m_view->verticalScrollBar()->value() : 0),
                         QString::number(zoomFactor(), 'f', 4),
                         fitWidthMode() ? QStringLiteral("FitToWidth") : QStringLiteral("Custom"),
                     });
    group.sync();
    // The tiles want a percent-read, which the position above can't give without the total. Here we
    // have both, so record the fraction for the library to show.
    ReadingProgress::record(m_url, ReadingProgress::fractionFromCounts(currentPageOneBased(), pageCount()));
}

void PdfView::closeEvent(QCloseEvent *event)
{
    saveReadingPosition();
    QWidget::closeEvent(event);
}

void PdfView::hideEvent(QHideEvent *event)
{
    saveReadingPosition();
    QWidget::hideEvent(event);
}

bool PdfView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_findField && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (keyEvent->modifiers() & Qt::ShiftModifier) {
                findPrevious();
            } else {
                findNext();
            }
            return true;
        }
        if (keyEvent->key() == Qt::Key_Escape) {
            closeFindBar();
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void PdfView::jumpToBookmark(const QModelIndex &index)
{
    if (!index.isValid() || !m_bookmarkTitleModel || !m_view) {
        return;
    }

    const QModelIndex sourceIndex = m_bookmarkTitleModel->mapToSource(index);
    bool pageOk = false;
    const int page = sourceIndex.data(static_cast<int>(QPdfBookmarkModel::Role::Page)).toInt(&pageOk);
    if (!pageOk || page < 0 || page >= pageCount()) {
        return;
    }

    const QPointF location = sourceIndex.data(static_cast<int>(QPdfBookmarkModel::Role::Location)).toPointF();
    bool zoomOk = false;
    const qreal bookmarkZoom = sourceIndex.data(static_cast<int>(QPdfBookmarkModel::Role::Zoom)).toReal(&zoomOk);
    jumpToPage(page, location, zoomOk ? bookmarkZoom : zoomFactor(), true);
}

void PdfView::generateAiNavigation()
{
    if (m_aiNavigationRunning || !m_document || m_document->status() != QPdfDocument::Status::Ready || pageCount() <= 0) {
        return;
    }

    if (!m_aiNavigationEntries.isEmpty()) {
        setAiNavigationStatus(m_aiNavigationTextTruncated ? i18n("AI navigation ready. Source text was truncated to %1 words.", AiNavigationWordBudget) : i18n("AI navigation ready."), true);
        updateAiNavigationUi();
        emitOutlineState();
        return;
    }

    if (m_claudeExecutable.isEmpty()) {
        m_claudeExecutable = ClaudeProcessHelper::findClaudeExecutable();
    }
    if (m_claudeExecutable.isEmpty()) {
        setAiNavigationStatus(i18n("Claude CLI not found."), true);
        updateAiNavigationActionState();
        updateAiNavigationUi();
        emitOutlineState();
        return;
    }

    m_aiNavigationRunning = true;
    m_aiNavigationExtractionPage = 0;
    m_aiNavigationWordCount = 0;
    m_aiNavigationTextTruncated = false;
    m_aiNavigationExtractedPages.clear();
    m_aiNavigationEntries.clear();
    rebuildAiNavigationModel();
    setAiNavigationStatus(i18n("Generating..."), true);
    updateAiNavigationActionState();
    updateAiNavigationUi();
    emitOutlineState();
    startAiNavigationTextExtraction();
}

void PdfView::cancelAiNavigationRun()
{
    if (!m_aiNavigationProcess) {
        m_aiNavigationRunning = false;
        return;
    }

    QProcess *const process = m_aiNavigationProcess;
    m_aiNavigationProcess = nullptr;
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->terminate();
        if (!process->waitForFinished(1000)) {
            process->kill();
            process->waitForFinished(2000);
        }
    }
    process->deleteLater();
    m_aiNavigationRunning = false;
}

void PdfView::resetAiNavigationForDocument()
{
    cancelAiNavigationRun();
    m_claudeExecutable = ClaudeProcessHelper::findClaudeExecutable();
    m_aiNavigationExtractedPages.clear();
    m_aiNavigationEntries.clear();
    m_aiNavigationTextTruncated = false;
    m_aiNavigationStatusVisible = false;
    m_aiNavigationExtractionPage = 0;
    m_aiNavigationWordCount = 0;

    const PdfDocumentFingerprint fingerprint = fingerprintForPdfPath(m_pdfPath);
    m_aiNavigationFingerprintHash = fingerprint.hash;
    m_aiNavigationCachePath = QDir(aiNavigationStoreDirectory()).filePath(fingerprint.hash + QStringLiteral(".json"));

    loadAiNavigationCache();
    rebuildAiNavigationModel();
    updateAiNavigationActionState();
    updateAiNavigationUi();
}

void PdfView::loadAiNavigationCache()
{
    if (m_aiNavigationCachePath.isEmpty() || !QFileInfo::exists(m_aiNavigationCachePath) || pageCount() <= 0) {
        return;
    }

    QFile file(m_aiNavigationCachePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != 1) {
        return;
    }

    const QJsonObject fingerprint = root.value(QStringLiteral("documentFingerprint")).toObject();
    if (fingerprint.value(QStringLiteral("hash")).toString() != m_aiNavigationFingerprintHash) {
        return;
    }

    m_aiNavigationTextTruncated = root.value(QStringLiteral("textTruncated")).toBool(false);
    m_aiNavigationEntries = navigationEntriesFromJson(root.value(QStringLiteral("aiNavigation")).toArray(), pageCount());
    if (!m_aiNavigationEntries.isEmpty() && m_aiNavigationTextTruncated) {
        setAiNavigationStatus(i18n("AI navigation ready. Source text was truncated to %1 words.", AiNavigationWordBudget), true);
    }
}

void PdfView::startAiNavigationTextExtraction()
{
    QTimer::singleShot(0, this, &PdfView::continueAiNavigationTextExtraction);
}

void PdfView::continueAiNavigationTextExtraction()
{
    if (!m_aiNavigationRunning || !m_document || m_document->status() != QPdfDocument::Status::Ready) {
        return;
    }

    const int pages = pageCount();
    int pagesThisSlice = 0;
    while (m_aiNavigationExtractionPage < pages && pagesThisSlice < 4) {
        const int page = m_aiNavigationExtractionPage++;
        ++pagesThisSlice;

        const QPdfSelection selection = m_document->getAllText(page);
        const QString text = selection.isValid() ? selection.text().trimmed() : QString();
        if (text.isEmpty()) {
            continue;
        }

        int words = wordCountInText(text);
        QString pageText = text;
        const int remaining = AiNavigationWordBudget - m_aiNavigationWordCount;
        if (remaining <= 0) {
            m_aiNavigationTextTruncated = true;
            continue;
        }
        if (words > remaining) {
            pageText = firstWords(text, remaining);
            words = wordCountInText(pageText);
            m_aiNavigationTextTruncated = true;
        }

        if (!pageText.isEmpty()) {
            m_aiNavigationExtractedPages << QStringLiteral("\n\n[[PAGE %1]]\n%2\n").arg(page + 1).arg(pageText);
            m_aiNavigationWordCount += words;
        }
    }

    if (m_aiNavigationExtractionPage < pages) {
        setAiNavigationStatus(i18n("Generating... %1 / %2 pages", m_aiNavigationExtractionPage, pages), true);
        QTimer::singleShot(0, this, &PdfView::continueAiNavigationTextExtraction);
        return;
    }

    if (m_aiNavigationExtractedPages.isEmpty() && realOutlineEntries().isEmpty()) {
        finishAiNavigationRun(i18n("No extractable text or outline found."));
        return;
    }

    setAiNavigationStatus(i18n("Generating..."), true);
    askClaudeForAiNavigation();
}

void PdfView::askClaudeForAiNavigation()
{
    const QString sourceText = m_aiNavigationExtractedPages.join(QString());
    const QList<AiNavigationEntry> outlineEntries = realOutlineEntries();
    const QString prompt = aiNavigationPrompt(outlineEntries, sourceText, pageCount(), m_aiNavigationTextTruncated);

    QProcess *process = new QProcess(this);
    m_aiNavigationProcess = process;

    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process != m_aiNavigationProcess || error != QProcess::FailedToStart) {
            return;
        }
        m_aiNavigationProcess = nullptr;
        process->deleteLater();
        finishAiNavigationRun(i18n("Claude CLI could not be started."));
    });

    QTimer *watchdog = new QTimer(process);
    watchdog->setSingleShot(true);
    connect(watchdog, &QTimer::timeout, process, &QProcess::kill);
    watchdog->start(AiNavigationClaudeTimeoutMs);

    connect(process, &QProcess::finished, this, [this, process, outlineEntries](int exitCode, QProcess::ExitStatus exitStatus) {
        if (process != m_aiNavigationProcess) {
            process->deleteLater();
            return;
        }

        m_aiNavigationProcess = nullptr;
        process->deleteLater();

        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            finishAiNavigationRun(i18n("AI navigation generation failed."));
            return;
        }

        const QList<AiNavigationEntry> aiEntries = parseAiNavigationReply(process->readAllStandardOutput(), pageCount());
        if (aiEntries.isEmpty() && outlineEntries.isEmpty()) {
            finishAiNavigationRun(i18n("No valid AI navigation was returned."));
            return;
        }

        m_aiNavigationEntries = mergedNavigationEntries(outlineEntries, aiEntries, pageCount());
        rebuildAiNavigationModel();
        saveAiNavigationCache();
        finishAiNavigationRun(m_aiNavigationTextTruncated ? i18n("AI navigation ready. Source text was truncated to %1 words.", AiNavigationWordBudget) : i18n("AI navigation ready."));
    });

    process->start(m_claudeExecutable, {QStringLiteral("-p"), prompt, QStringLiteral("--output-format"), QStringLiteral("json")});
}

void PdfView::finishAiNavigationRun(const QString &message)
{
    m_aiNavigationRunning = false;
    setAiNavigationStatus(message, !message.isEmpty());
    updateAiNavigationActionState();
    updateAiNavigationUi();
    emitOutlineState();
}

void PdfView::setAiNavigationStatus(const QString &message, bool keepVisible)
{
    if (m_aiNavigationStatusLabel) {
        m_aiNavigationStatusLabel->setText(message);
    }
    m_aiNavigationStatusVisible = keepVisible && !message.isEmpty();
}

void PdfView::updateAiNavigationActionState()
{
    if (!m_aiNavigationAction) {
        return;
    }

    m_aiNavigationAction->setText(i18n("Generate AI Navigation"));
    if (m_aiNavigationRunning) {
        m_aiNavigationAction->setEnabled(false);
        m_aiNavigationAction->setToolTip(i18n("Generating AI navigation with the local claude CLI."));
        return;
    }

    if (!m_aiNavigationEntries.isEmpty()) {
        m_aiNavigationAction->setEnabled(false);
        m_aiNavigationAction->setToolTip(i18n("AI navigation is cached for this PDF."));
        return;
    }

    if (!m_document || m_document->status() != QPdfDocument::Status::Ready || pageCount() <= 0) {
        m_aiNavigationAction->setEnabled(false);
        m_aiNavigationAction->setToolTip(i18n("Open a PDF to generate AI navigation."));
        return;
    }

    if (m_claudeExecutable.isEmpty()) {
        m_aiNavigationAction->setEnabled(false);
        m_aiNavigationAction->setToolTip(i18n("Requires the claude CLI installed on PATH, ~/.local/bin, or /opt/homebrew/bin."));
        return;
    }

    m_aiNavigationAction->setEnabled(true);
    m_aiNavigationAction->setToolTip(i18n("Generate semantic navigation with the local claude CLI."));
}

void PdfView::updateAiNavigationUi()
{
    const bool hasRealOutline = m_bookmarkModel && m_bookmarkModel->rowCount() > 0;
    if (m_outlineTitle) {
        m_outlineTitle->setVisible(hasRealOutline);
    }
    if (m_outlineView) {
        m_outlineView->setVisible(hasRealOutline);
    }
    if (m_aiNavigationProgress) {
        m_aiNavigationProgress->setVisible(m_aiNavigationRunning);
    }
    if (m_aiNavigationStatusLabel) {
        m_aiNavigationStatusLabel->setVisible(m_aiNavigationStatusVisible || m_aiNavigationRunning);
    }
    if (m_aiNavigationView) {
        m_aiNavigationView->setVisible(m_aiNavigationModel && m_aiNavigationModel->rowCount() > 0);
    }
    updateAiNavigationActionState();
}

void PdfView::rebuildAiNavigationModel()
{
    if (!m_aiNavigationModel) {
        return;
    }

    m_aiNavigationModel->clear();
    m_aiNavigationModel->setHorizontalHeaderLabels({i18n("AI Navigation")});

    QStandardItem *levelParents[3] = {nullptr, nullptr, nullptr};
    for (const AiNavigationEntry &entry : std::as_const(m_aiNavigationEntries)) {
        const int level = qBound(1, entry.level, 3);
        auto *item = new QStandardItem(entry.title);
        item->setEditable(false);
        item->setData(entry.pageOneBased - 1, AiNavigationPageRole);
        item->setData(level, AiNavigationLevelRole);
        item->setToolTip(i18n("Page %1", entry.pageOneBased));

        if (level > 1 && levelParents[level - 2]) {
            levelParents[level - 2]->appendRow(item);
        } else {
            m_aiNavigationModel->appendRow(item);
        }
        levelParents[level - 1] = item;
        for (int i = level; i < 3; ++i) {
            levelParents[i] = nullptr;
        }
    }

    if (m_aiNavigationView) {
        m_aiNavigationView->expandToDepth(1);
    }
    updateAiNavigationUi();
}

void PdfView::jumpToAiNavigationEntry(const QModelIndex &index)
{
    if (!index.isValid() || !m_aiNavigationModel || !m_view || !m_view->pageNavigator()) {
        return;
    }

    bool ok = false;
    const int page = index.data(AiNavigationPageRole).toInt(&ok);
    if (!ok || page < 0 || page >= pageCount()) {
        return;
    }

    jumpToPage(page, QPointF(), zoomFactor(), true);
}

void PdfView::saveAiNavigationCache() const
{
    if (m_aiNavigationCachePath.isEmpty() || m_aiNavigationEntries.isEmpty()) {
        return;
    }
    if (!QDir().mkpath(QFileInfo(m_aiNavigationCachePath).absolutePath())) {
        return;
    }

    const PdfDocumentFingerprint fingerprint = fingerprintForPdfPath(m_pdfPath);
    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("documentPath"), m_pdfPath},
        {QStringLiteral("documentCanonicalPath"), fingerprint.canonicalPath},
        {QStringLiteral("documentFingerprint"), fingerprintToJson(fingerprint)},
        {QStringLiteral("pageCount"), pageCount()},
        {QStringLiteral("wordBudget"), AiNavigationWordBudget},
        {QStringLiteral("textTruncated"), m_aiNavigationTextTruncated},
        {QStringLiteral("sourceTextCached"), false},
        {QStringLiteral("rawOutline"), navigationEntriesToJson(realOutlineEntries())},
        {QStringLiteral("aiNavigation"), navigationEntriesToJson(m_aiNavigationEntries)},
        {QStringLiteral("promptVersion"), 1},
        {QStringLiteral("promptInstruction"),
         QStringLiteral("Return only a JSON array of {title,page,level}; augment the raw PDF outline with semantic jump points from page-marked text; cite only existing pages.")},
        {QStringLiteral("generatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
    };

    QSaveFile file(m_aiNavigationCachePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}

QList<PdfView::AiNavigationEntry> PdfView::realOutlineEntries() const
{
    QList<AiNavigationEntry> entries;
    if (!m_bookmarkModel || pageCount() <= 0) {
        return entries;
    }

    const std::function<void(const QModelIndex &, int)> collect = [&](const QModelIndex &parent, int level) {
        const int rows = m_bookmarkModel->rowCount(parent);
        for (int row = 0; row < rows; ++row) {
            const QModelIndex index = m_bookmarkModel->index(row, 0, parent);
            const QString title = index.data(static_cast<int>(QPdfBookmarkModel::Role::Title)).toString().trimmed();
            bool pageOk = false;
            const int zeroBasedPage = index.data(static_cast<int>(QPdfBookmarkModel::Role::Page)).toInt(&pageOk);
            if (!title.isEmpty() && pageOk && zeroBasedPage >= 0 && zeroBasedPage < pageCount()) {
                entries.append(AiNavigationEntry{title, zeroBasedPage + 1, qBound(1, level, 3)});
            }
            collect(index, qMin(level + 1, 3));
        }
    };

    collect(QModelIndex(), 1);
    return entries;
}

void PdfView::restoreReadingPosition()
{
    if (m_url.isEmpty() || pageCount() <= 0) {
        return;
    }

    KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(PositionsGroup));
    const QStringList saved = group.readEntry(positionKey(), QStringList());
    if (saved.size() >= 4) {
        bool pageOk = false;
        bool scrollOk = false;
        bool zoomOk = false;
        const int page = saved.at(0).toInt(&pageOk);
        const int scroll = saved.at(1).toInt(&scrollOk);
        const qreal zoom = saved.at(2).toDouble(&zoomOk);
        if (pageOk && scrollOk && zoomOk) {
            m_pendingRestorePage = qBound(0, page, pageCount() - 1);
            m_pendingRestoreScroll = qMax(0, scroll);
            m_pendingRestoreZoom = qBound(MinZoom, zoom, MaxZoom);
            m_pendingRestoreFitWidth = saved.at(3) != QLatin1String("Custom");
            m_havePendingRestore = true;
        }
    }

    if (!m_havePendingRestore) {
        m_pendingRestorePage = 0;
        m_pendingRestoreScroll = 0;
        m_pendingRestoreZoom = 1.0;
        m_pendingRestoreFitWidth = true;
    }

    if (m_pendingRestoreFitWidth) {
        m_view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    } else {
        m_view->setZoomMode(QPdfView::ZoomMode::Custom);
        m_view->setZoomFactor(m_pendingRestoreZoom);
    }
    m_view->pageNavigator()->jump(m_pendingRestorePage, QPointF(), zoomFactor());
    scheduleDeferredScrollRestore();
}

void PdfView::scheduleDeferredScrollRestore()
{
    const int scroll = m_pendingRestoreScroll;
    QTimer::singleShot(0, this, [this, scroll]() {
        if (m_view && m_view->verticalScrollBar()) {
            m_view->verticalScrollBar()->setValue(scroll);
        }
    });
    QTimer::singleShot(50, this, [this, scroll]() {
        if (m_view && m_view->verticalScrollBar()) {
            m_view->verticalScrollBar()->setValue(scroll);
        }
    });
}

void PdfView::activateSearchResult(int index)
{
    if (!m_searchModel) {
        return;
    }

    const int count = m_searchModel->rowCount({});
    if (count <= 0) {
        m_currentSearchIndex = -1;
        m_view->setCurrentSearchResultIndex(-1);
        updateFindState();
        return;
    }

    m_currentSearchIndex = qBound(0, index, count - 1);
    const QPdfLink result = m_searchModel->resultAtIndex(m_currentSearchIndex);
    m_view->setCurrentSearchResultIndex(m_currentSearchIndex);
    static_cast<ShellPdfView *>(m_view)->scrollToLink(result);
    updateFindState();
}

void PdfView::updateFindState()
{
    if (!m_findStatusLabel || !m_searchModel) {
        return;
    }

    const QString query = m_searchModel->searchString();
    const int count = m_searchModel->rowCount({});
    if (query.isEmpty()) {
        m_currentSearchIndex = -1;
        m_view->setCurrentSearchResultIndex(-1);
        m_findStatusLabel->clear();
        return;
    }

    if (count <= 0) {
        m_currentSearchIndex = -1;
        m_view->setCurrentSearchResultIndex(-1);
        m_findStatusLabel->setText(i18n("No matches"));
        return;
    }

    if (m_currentSearchIndex < 0) {
        activateSearchResult(0);
        return;
    }

    if (m_currentSearchIndex >= count) {
        m_currentSearchIndex = count - 1;
        m_view->setCurrentSearchResultIndex(m_currentSearchIndex);
    }

    m_findStatusLabel->setText(i18n("%1 / %2", m_currentSearchIndex + 1, count));
}

void PdfView::resetFindState()
{
    m_currentSearchIndex = -1;
    if (m_searchModel) {
        m_searchModel->setSearchString(QString());
    }
    if (m_view) {
        m_view->setCurrentSearchResultIndex(-1);
    }
    if (m_findField) {
        const QSignalBlocker blocker(m_findField);
        m_findField->clear();
    }
    if (m_findStatusLabel) {
        m_findStatusLabel->clear();
    }
}

void PdfView::setupFindBar()
{
    m_findAction = new QAction(i18n("Find in PDF"), this);
    m_findAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-find")));
    connect(m_findAction, &QAction::triggered, this, &PdfView::showFindBar);

    m_findBar = new QWidget(this);
    m_findBar->setObjectName(QStringLiteral("paperlibrary_pdf_find_bar"));
    m_findBar->setAutoFillBackground(true);
    m_findBar->hide();

    auto *findLayout = new QHBoxLayout(m_findBar);
    findLayout->setContentsMargins(8, 6, 8, 6);
    findLayout->setSpacing(6);

    auto *findLabel = new QLabel(i18n("Find"), m_findBar);
    findLayout->addWidget(findLabel);

    m_findField = new QLineEdit(m_findBar);
    m_findField->setObjectName(QStringLiteral("paperlibrary_pdf_find_field"));
    m_findField->setClearButtonEnabled(true);
    m_findField->setPlaceholderText(i18n("Search this PDF"));
    m_findField->installEventFilter(this);
    findLayout->addWidget(m_findField, 1);

    m_findStatusLabel = new QLabel(m_findBar);
    m_findStatusLabel->setMinimumWidth(72);
    m_findStatusLabel->setAlignment(Qt::AlignCenter);
    findLayout->addWidget(m_findStatusLabel);

    auto *previousButton = new QToolButton(m_findBar);
    previousButton->setArrowType(Qt::UpArrow);
    previousButton->setToolTip(i18n("Previous match"));
    findLayout->addWidget(previousButton);

    auto *nextButton = new QToolButton(m_findBar);
    nextButton->setArrowType(Qt::DownArrow);
    nextButton->setToolTip(i18n("Next match"));
    findLayout->addWidget(nextButton);

    auto *closeButton = new QToolButton(m_findBar);
    closeButton->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
    closeButton->setText(QStringLiteral("x"));
    closeButton->setToolTip(i18n("Close find bar"));
    findLayout->addWidget(closeButton);

    connect(m_findField, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_currentSearchIndex = -1;
        m_view->setCurrentSearchResultIndex(-1);
        m_searchModel->setSearchString(text);
        updateFindState();
    });
    connect(previousButton, &QToolButton::clicked, this, &PdfView::findPrevious);
    connect(nextButton, &QToolButton::clicked, this, &PdfView::findNext);
    connect(closeButton, &QToolButton::clicked, this, &PdfView::closeFindBar);
}

PdfView::ChapterProgress PdfView::computeChapterProgress(const QList<AiNavigationEntry> &entries,
                                                         int currentPageOneBased, int pageCount)
{
    ChapterProgress progress;
    if (entries.isEmpty() || pageCount <= 0 || currentPageOneBased < 1) {
        return progress; // invalid: nothing to anchor a chapter to
    }

    // Chapters are the top-level (level-1) outline entries -- a subsection is not a chapter end.
    // A flat outline with no level-1 entries falls back to treating every entry as a chapter.
    QList<AiNavigationEntry> chapters;
    for (const AiNavigationEntry &entry : entries) {
        if (entry.level <= 1) {
            chapters.append(entry);
        }
    }
    if (chapters.isEmpty()) {
        chapters = entries;
    }
    std::sort(chapters.begin(), chapters.end(),
              [](const AiNavigationEntry &a, const AiNavigationEntry &b) { return a.pageOneBased < b.pageOneBased; });

    progress.valid = true;
    progress.chapterCount = int(chapters.size());

    // The current chapter is the last one that begins at or before the current page.
    int index = -1;
    for (int i = 0; i < chapters.size(); ++i) {
        if (chapters[i].pageOneBased <= currentPageOneBased) {
            index = i;
        } else {
            break;
        }
    }

    if (index < 0) {
        // Before the first chapter: front matter, which runs from page 1 to that chapter.
        progress.chapterIndex = 0;
        progress.title = QObject::tr("Front matter");
        const int start = 1;
        const int end = chapters.first().pageOneBased; // exclusive
        progress.pagesLeftInChapter = qMax(1, end - currentPageOneBased);
        const int span = qMax(1, end - start);
        progress.fraction = qBound(0.0, double(currentPageOneBased - start) / span, 1.0);
        return progress;
    }

    const int start = chapters[index].pageOneBased;
    // The chapter ends where the next one begins; the last chapter ends one past the last page.
    const int end = (index + 1 < chapters.size()) ? chapters[index + 1].pageOneBased : pageCount + 1;
    progress.chapterIndex = index + 1;
    progress.title = chapters[index].title;
    progress.pagesLeftInChapter = qMax(0, end - currentPageOneBased);
    const int span = qMax(1, end - start);
    progress.fraction = qBound(0.0, double(currentPageOneBased - start) / span, 1.0);
    return progress;
}

PdfView::ChapterProgress PdfView::currentChapterProgress() const
{
    return computeChapterProgress(m_aiNavigationEntries, currentPageOneBased(), pageCount());
}

void PdfView::emitPageState()
{
    Q_EMIT pageStateChanged(currentPageOneBased(), pageCount());
    const ChapterProgress chapter = currentChapterProgress();
    Q_EMIT chapterStateChanged(chapter);
}

void PdfView::emitZoomState()
{
    Q_EMIT zoomStateChanged(zoomFactor(), fitWidthMode());
}

void PdfView::emitOutlineState()
{
    updateAiNavigationUi();
    Q_EMIT outlineAvailableChanged(hasOutline());
}

QString PdfView::positionKey() const
{
    return positionKeyForUrl(m_url);
}
