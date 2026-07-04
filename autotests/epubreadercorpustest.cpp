/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../shell/epubwebreader.h"

#include <KZip>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>

using namespace EpubWebReaderCore;

namespace
{
constexpr int MaxSpineDocuments = 5;
constexpr int MaxResourcesPerBook = 2000;
constexpr int DefaultBookTimeoutMs = 30000;

struct ResourceRef {
    QString basePath;
    QString href;
    QString kind;
};

struct QueuedResource {
    QString path;
    QString contentType;
    int depth = 0;
};

struct BookResult {
    QString status = QStringLiteral("PASS");
    QString title;
    QStringList categories;
    QStringList details;
};

QList<ResourceRef> deduplicatedRefs(const QList<ResourceRef> &refs)
{
    QList<ResourceRef> deduplicated;
    QSet<QString> seen;
    for (const ResourceRef &ref : refs) {
        const QString key = ref.basePath + QLatin1Char('\t') + ref.href + QLatin1Char('\t') + ref.kind;
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        deduplicated.append(ref);
    }
    return deduplicated;
}

QString escapedField(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    return value;
}

QString unescapedField(QString value)
{
    QString result;
    result.reserve(value.size());
    bool escaped = false;
    for (const QChar ch : std::as_const(value)) {
        if (escaped) {
            if (ch == QLatin1Char('t')) {
                result.append(QLatin1Char('\t'));
            } else if (ch == QLatin1Char('n')) {
                result.append(QLatin1Char('\n'));
            } else if (ch == QLatin1Char('r')) {
                result.append(QLatin1Char('\r'));
            } else {
                result.append(ch);
            }
            escaped = false;
            continue;
        }
        if (ch == QLatin1Char('\\')) {
            escaped = true;
            continue;
        }
        result.append(ch);
    }
    if (escaped) {
        result.append(QLatin1Char('\\'));
    }
    return result;
}

void addCategory(BookResult &result, const QString &category)
{
    if (!result.categories.contains(category)) {
        result.categories.append(category);
    }
}

void addFailure(BookResult &result, const QString &category, const QString &detail)
{
    addCategory(result, category);
    result.details.append(detail);
}

QString localName(QString name)
{
    const qsizetype colon = name.lastIndexOf(QLatin1Char(':'));
    if (colon >= 0) {
        name = name.mid(colon + 1);
    }
    return name.toLower();
}

QString attributeValue(const QXmlStreamAttributes &attributes, const QString &wantedName)
{
    for (const QXmlStreamAttribute &attribute : attributes) {
        if (localName(attribute.name().toString()) == wantedName || localName(attribute.qualifiedName().toString()) == wantedName) {
            return attribute.value().toString();
        }
    }
    return QString();
}

bool isSkippableHref(const QString &href)
{
    const QString trimmed = href.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')) || trimmed.startsWith(QLatin1String("//"))) {
        return true;
    }

    const QUrl url = QUrl::fromEncoded(trimmed.toUtf8(), QUrl::TolerantMode);
    const QString scheme = url.scheme().toLower();
    return !scheme.isEmpty() && scheme != QLatin1String("paperlibrary-epub");
}

QString cssUnescape(QString value)
{
    value = value.trimmed();
    value.replace(QStringLiteral("\\ "), QStringLiteral(" "));
    value.replace(QStringLiteral("\\\""), QStringLiteral("\""));
    value.replace(QStringLiteral("\\'"), QStringLiteral("'"));
    value.replace(QStringLiteral("\\("), QStringLiteral("("));
    value.replace(QStringLiteral("\\)"), QStringLiteral(")"));
    return value;
}

void appendCssRefs(const QString &css, const QString &basePath, QList<ResourceRef> *refs)
{
    QString stripped = css;
    stripped.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"), QRegularExpression::DotMatchesEverythingOption));

    static const QRegularExpression urlRe(QStringLiteral("url\\(\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\)]*?))\\s*\\)"),
                                          QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator urlIt = urlRe.globalMatch(stripped);
    while (urlIt.hasNext()) {
        const QRegularExpressionMatch match = urlIt.next();
        const QString href = cssUnescape(!match.captured(1).isEmpty() ? match.captured(1) : !match.captured(2).isEmpty() ? match.captured(2) : match.captured(3));
        if (!isSkippableHref(href)) {
            refs->append({basePath, href, QStringLiteral("css-url")});
        }
    }

    static const QRegularExpression importRe(QStringLiteral("@import\\s+(?:url\\(\\s*)?(?:\"([^\"]*)\"|'([^']*)'|([^\\s\\);]+))"),
                                             QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator importIt = importRe.globalMatch(stripped);
    while (importIt.hasNext()) {
        const QRegularExpressionMatch match = importIt.next();
        const QString href = cssUnescape(!match.captured(1).isEmpty() ? match.captured(1) : !match.captured(2).isEmpty() ? match.captured(2) : match.captured(3));
        if (!isSkippableHref(href)) {
            refs->append({basePath, href, QStringLiteral("css-import")});
        }
    }
}

QHash<QString, QString> parseAttributes(const QString &attributeText)
{
    QHash<QString, QString> attributes;
    static const QRegularExpression attrRe(QStringLiteral("([A-Za-z_:][-A-Za-z0-9_:.]*)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\"'>]+))"));
    QRegularExpressionMatchIterator it = attrRe.globalMatch(attributeText);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        attributes.insert(localName(match.captured(1)), !match.captured(2).isEmpty() ? match.captured(2) : !match.captured(3).isEmpty() ? match.captured(3) : match.captured(4));
    }
    return attributes;
}

bool shouldTakeTagHref(const QString &tagName, const QHash<QString, QString> &attributes)
{
    const QString tag = localName(tagName);
    if (tag == QLatin1String("link")) {
        const QString rel = attributes.value(QStringLiteral("rel")).toLower();
        const QString type = attributes.value(QStringLiteral("type")).toLower();
        const QString href = attributes.value(QStringLiteral("href")).toLower();
        if (type == QLatin1String("application/vnd.adobe-page-template+xml") || href.endsWith(QLatin1String(".xpgt"))) {
            return false;
        }
        return rel.contains(QLatin1String("stylesheet")) || rel.contains(QLatin1String("icon")) || rel.contains(QLatin1String("preload"))
            || rel.contains(QLatin1String("prefetch")) || type.contains(QLatin1String("css"));
    }
    return tag == QLatin1String("img") || tag == QLatin1String("image") || tag == QLatin1String("source") || tag == QLatin1String("audio") || tag == QLatin1String("video")
        || tag == QLatin1String("track") || tag == QLatin1String("object") || tag == QLatin1String("embed");
}

void appendMarkupRefsFallback(const QString &text, const QString &basePath, QList<ResourceRef> *refs)
{
    static const QRegularExpression tagRe(QStringLiteral("<\\s*([A-Za-z_:][-A-Za-z0-9_:.]*)\\b([^>]*)>"), QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatchIterator tagIt = tagRe.globalMatch(text);
    while (tagIt.hasNext()) {
        const QRegularExpressionMatch tagMatch = tagIt.next();
        const QString tagName = tagMatch.captured(1);
        const QHash<QString, QString> attributes = parseAttributes(tagMatch.captured(2));

        const QString style = attributes.value(QStringLiteral("style"));
        if (!style.isEmpty()) {
            appendCssRefs(style, basePath, refs);
        }

        if (!shouldTakeTagHref(tagName, attributes)) {
            continue;
        }

        const QString tag = localName(tagName);
        const QStringList names = tag == QLatin1String("object") ? QStringList{QStringLiteral("data")} : QStringList{QStringLiteral("src"), QStringLiteral("href"), QStringLiteral("poster")};
        for (const QString &name : names) {
            const QString href = attributes.value(name);
            if (!isSkippableHref(href)) {
                refs->append({basePath, href, tag + QLatin1Char('/') + name});
            }
        }
    }
}

QList<ResourceRef> refsFromMarkup(const QByteArray &data, const QString &basePath)
{
    QList<ResourceRef> refs;
    QStringList styleBlocks;
    const QString text = QString::fromUtf8(data);

    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }

        const QString tag = localName(xml.name().toString());
        const QXmlStreamAttributes attributes = xml.attributes();
        QString style = attributeValue(attributes, QStringLiteral("style"));
        if (!style.isEmpty()) {
            appendCssRefs(style, basePath, &refs);
        }

        if (tag == QLatin1String("style")) {
            styleBlocks.append(xml.readElementText(QXmlStreamReader::IncludeChildElements));
            continue;
        }

        QHash<QString, QString> attrs;
        for (const QXmlStreamAttribute &attribute : attributes) {
            attrs.insert(localName(attribute.qualifiedName().toString()), attribute.value().toString());
        }
        if (!shouldTakeTagHref(tag, attrs)) {
            continue;
        }

        const QStringList names = tag == QLatin1String("object") ? QStringList{QStringLiteral("data")} : QStringList{QStringLiteral("src"), QStringLiteral("href"), QStringLiteral("poster")};
        for (const QString &name : names) {
            const QString href = attributeValue(attributes, name);
            if (!isSkippableHref(href)) {
                refs.append({basePath, href, tag + QLatin1Char('/') + name});
            }
        }
    }

    for (const QString &style : std::as_const(styleBlocks)) {
        appendCssRefs(style, basePath, &refs);
    }
    appendMarkupRefsFallback(text, basePath, &refs);
    return deduplicatedRefs(refs);
}

QList<ResourceRef> refsFromCss(const QByteArray &data, const QString &basePath)
{
    QList<ResourceRef> refs;
    appendCssRefs(QString::fromUtf8(data), basePath, &refs);
    return deduplicatedRefs(refs);
}

QUrl epubUrlForPath(const QString &path)
{
    QUrl url;
    url.setScheme(QStringLiteral("paperlibrary-epub"));
    url.setHost(QStringLiteral("book"));
    url.setPath(QLatin1Char('/') + path, QUrl::DecodedMode);
    return url;
}

QString requestPathForHref(const QString &basePath, const QString &href)
{
    const QUrl base = epubUrlForPath(basePath);
    const QUrl relative = QUrl::fromEncoded(href.trimmed().toUtf8(), QUrl::TolerantMode);
    const QUrl resolved = base.resolved(relative);
    if (resolved.scheme() != QLatin1String("paperlibrary-epub") || resolved.host() != QLatin1String("book")) {
        return QString();
    }
    return cleanArchivePath(resolved.path(QUrl::FullyDecoded));
}

bool isCssType(const QString &contentType)
{
    return contentType.startsWith(QLatin1String("text/css"), Qt::CaseInsensitive);
}

bool isMarkupType(const QString &contentType)
{
    return contentType.startsWith(QLatin1String("application/xhtml+xml"), Qt::CaseInsensitive) || contentType.startsWith(QLatin1String("text/html"), Qt::CaseInsensitive)
        || contentType.startsWith(QLatin1String("application/xml"), Qt::CaseInsensitive) || contentType.startsWith(QLatin1String("text/xml"), Qt::CaseInsensitive);
}

bool isSvgType(const QString &contentType)
{
    return contentType.startsWith(QLatin1String("image/svg+xml"), Qt::CaseInsensitive);
}

void checkPositionClamp(BookResult *result, int spineCount)
{
    const QList<QStringList> cases = {
        {},
        {QStringLiteral("-1"), QStringLiteral("10")},
        {QString::number(spineCount), QStringLiteral("10")},
        {QStringLiteral("not-an-index"), QStringLiteral("10")},
        {QStringLiteral("0"), QStringLiteral("-1")},
        {QStringLiteral("0"), QStringLiteral("nan")},
        {QStringLiteral("0"), QStringLiteral("inf")},
        {QStringLiteral("0"), QStringLiteral("1000000001")},
        {QString::number(qMax(0, spineCount - 1)), QStringLiteral("42")},
    };

    for (const QStringList &saved : cases) {
        const StoredPosition restored = restoreStoredPosition(saved, spineCount);
        if (spineCount > 0 && (restored.spineIndex < 0 || restored.spineIndex >= spineCount || !std::isfinite(restored.scrollOffset) || restored.scrollOffset < 0.0)) {
            addFailure(*result,
                       QStringLiteral("position-clamp"),
                       QStringLiteral("saved=%1 restored index=%2 offset=%3")
                           .arg(saved.join(QLatin1Char(',')), QString::number(restored.spineIndex), QString::number(restored.scrollOffset, 'g', 17)));
        }
    }
}

void checkSubresources(const QString &path, const EpubInspection &inspection, BookResult *result)
{
    KZip zip(path);
    if (!zip.open(QIODevice::ReadOnly)) {
        addFailure(*result, QStringLiteral("zip-open"), QStringLiteral("could not reopen zip"));
        return;
    }

    QSet<QString> visited;
    QList<QueuedResource> queue;
    const int spineCount = qMin(MaxSpineDocuments, inspection.spine.size());
    for (int i = 0; i < spineCount; ++i) {
        const QString spinePath = inspection.spine.at(i);
        const ArchiveLookup lookup = lookupArchivePath(zip, spinePath, inspection.opfDir, inspection.manifestPathsByHref);
        if (!lookup.found) {
            addFailure(*result,
                       QStringLiteral("spine-unresolved"),
                       QStringLiteral("spine[%1] %2 candidates=[%3]").arg(i).arg(spinePath, lookup.candidates.join(QLatin1Char(','))));
            continue;
        }
        queue.append({lookup.zipEntry, contentTypeForPath(lookup.zipEntry, inspection.manifestMediaTypesByPath.value(lookup.zipEntry)), 0});
    }

    int processed = 0;
    while (!queue.isEmpty()) {
        if (++processed > MaxResourcesPerBook) {
            addFailure(*result, QStringLiteral("resource-limit"), QStringLiteral("stopped after %1 resources").arg(MaxResourcesPerBook));
            return;
        }

        const QueuedResource item = queue.takeFirst();
        if (visited.contains(item.path)) {
            continue;
        }
        visited.insert(item.path);

        const QByteArray data = readArchiveFile(zip, item.path);
        QList<ResourceRef> refs;
        if (isCssType(item.contentType)) {
            refs = refsFromCss(data, item.path);
        } else if (isMarkupType(item.contentType) || isSvgType(item.contentType)) {
            refs = refsFromMarkup(data, item.path);
        } else {
            continue;
        }

        for (const ResourceRef &ref : std::as_const(refs)) {
            const QString requestPath = requestPathForHref(ref.basePath, ref.href);
            if (requestPath.isEmpty()) {
                continue;
            }

            const ArchiveLookup lookup = lookupArchivePath(zip, requestPath, inspection.opfDir, inspection.manifestPathsByHref);
            if (!lookup.found) {
                addCategory(*result, QStringLiteral("unsupported-missing-subresources"));
                result->details.append(QStringLiteral("%1 in %2 href=%3 request=%4 candidates=[%5]")
                                           .arg(ref.kind, ref.basePath, ref.href, requestPath, lookup.candidates.join(QLatin1Char(','))));
                continue;
            }

            const QString contentType = contentTypeForPath(lookup.zipEntry, inspection.manifestMediaTypesByPath.value(lookup.zipEntry));
            if (contentType == QLatin1String("application/octet-stream")) {
                addFailure(*result,
                           QStringLiteral("subresource-mime-unknown"),
                           QStringLiteral("%1 in %2 href=%3 resolved=%4").arg(ref.kind, ref.basePath, ref.href, lookup.zipEntry));
            }

            if (item.depth < 3 && (isCssType(contentType) || isSvgType(contentType))) {
                queue.append({lookup.zipEntry, contentType, item.depth + 1});
            }
        }
    }
}

bool isUnsupportedCategory(const QString &category)
{
    return category.startsWith(QLatin1String("unsupported-"));
}

BookResult inspectBook(const QString &path)
{
    BookResult result;
    const EpubInspection inspection = inspectEpub(path);
    result.title = inspection.title.trimmed();

    if (!inspection.isEpub) {
        addFailure(result, QStringLiteral("not-epub"), QStringLiteral("extension is .epub but mimetype/container did not inspect as EPUB"));
    }
    if (result.title.isEmpty()) {
        addFailure(result, QStringLiteral("missing-title"), QStringLiteral("inspectEpub returned an empty title"));
    }

    if (inspection.drmProtected) {
        addCategory(result, QStringLiteral("unsupported-drm"));
    }
    if (inspection.fixedLayout) {
        addCategory(result, QStringLiteral("unsupported-fixed-layout"));
    }

    if (inspection.drmProtected || inspection.fixedLayout) {
        result.status = QStringLiteral("UNSUPPORTED");
        return result;
    }

    if (inspection.spine.isEmpty()) {
        addFailure(result, QStringLiteral("empty-spine"), QStringLiteral("no readable reflowable spine documents"));
    } else {
        checkPositionClamp(&result, inspection.spine.size());
        checkSubresources(path, inspection, &result);
    }

    const bool onlyUnsupported = !result.categories.isEmpty()
        && std::all_of(result.categories.cbegin(), result.categories.cend(), [](const QString &category) {
               return isUnsupportedCategory(category);
           });
    if (onlyUnsupported) {
        result.status = QStringLiteral("UNSUPPORTED");
    } else if (!result.categories.isEmpty()) {
        result.status = QStringLiteral("FAIL");
    }
    return result;
}

QStringList corpusRoots()
{
    return {
        QDir::home().filePath(QStringLiteral("Library/Application Support/PaperLibrary/imported-books")),
        QDir::home().filePath(QStringLiteral("Documents")),
        QDir::home().filePath(QStringLiteral("Desktop")),
        QDir::home().filePath(QStringLiteral("Downloads")),
    };
}

QStringList discoverBooks()
{
    QStringList books;
    QSet<QString> seen;
    for (const QString &root : corpusRoots()) {
        const QFileInfo rootInfo(root);
        if (!rootInfo.isDir()) {
            continue;
        }
        QDirIterator it(rootInfo.absoluteFilePath(), QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            if (QFileInfo(path).suffix().compare(QLatin1String("epub"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            const QString canonical = QFileInfo(path).canonicalFilePath();
            const QString key = canonical.isEmpty() ? QFileInfo(path).absoluteFilePath() : canonical;
            if (seen.contains(key)) {
                continue;
            }
            seen.insert(key);
            books.append(QFileInfo(path).absoluteFilePath());
        }
    }
    std::sort(books.begin(), books.end(), [](const QString &a, const QString &b) {
        return a.localeAwareCompare(b) < 0;
    });
    return books;
}

int timeoutMs()
{
    bool ok = false;
    const int fromEnv = QString::fromLocal8Bit(qgetenv("PAPERLIBRARY_EPUB_CORPUS_TIMEOUT_MS")).toInt(&ok);
    return ok && fromEnv > 0 ? fromEnv : DefaultBookTimeoutMs;
}

struct ParsedChildResult {
    QString status = QStringLiteral("FAIL");
    QString title;
    QStringList categories = {QStringLiteral("worker-no-result")};
    QStringList details;
};

ParsedChildResult parseChildOutput(const QByteArray &stdoutBytes)
{
    ParsedChildResult parsed;
    const QString output = QString::fromUtf8(stdoutBytes);
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields = line.split(QLatin1Char('\t'));
        if (fields.isEmpty()) {
            continue;
        }
        if (fields.at(0) == QLatin1String("BOOK") && fields.size() >= 4) {
            parsed.status = fields.at(1);
            parsed.title = unescapedField(fields.at(2));
            parsed.categories = fields.at(3).split(QLatin1Char(','), Qt::SkipEmptyParts);
            if (parsed.categories.isEmpty() && parsed.status == QLatin1String("FAIL")) {
                parsed.categories.append(QStringLiteral("uncategorized-failure"));
            }
        } else if (fields.at(0) == QLatin1String("DETAIL") && fields.size() >= 2) {
            parsed.details.append(unescapedField(fields.mid(1).join(QLatin1Char('\t'))));
        }
    }
    return parsed;
}

void writeBookResult(const BookResult &result)
{
    QTextStream out(stdout);
    out << "BOOK\t" << result.status << '\t' << escapedField(result.title) << '\t' << result.categories.join(QLatin1Char(',')) << '\n';
    for (const QString &detail : result.details) {
        out << "DETAIL\t" << escapedField(detail) << '\n';
    }
}

int runWorker(const QString &path)
{
    writeBookResult(inspectBook(path));
    return 0;
}

int runParent()
{
    QTextStream out(stdout);
    QTextStream err(stderr);
    const QStringList books = discoverBooks();
    const int perBookTimeoutMs = timeoutMs();

    out << "EPUB corpus roots:\n";
    for (const QString &root : corpusRoots()) {
        out << "  " << root << (QFileInfo(root).isDir() ? "" : " (absent)") << '\n';
    }
    out << "EPUB corpus books: " << books.size() << "\n";

    int passed = 0;
    int failed = 0;
    int unsupported = 0;
    QHash<QString, int> categoryCounts;
    QStringList unsupportedBooks;

    for (const QString &book : books) {
        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({QStringLiteral("--book"), book});
        child.setProcessChannelMode(QProcess::SeparateChannels);
        child.start();

        ParsedChildResult parsed;
        if (!child.waitForStarted(5000)) {
            parsed.status = QStringLiteral("FAIL");
            parsed.categories = {QStringLiteral("worker-start")};
            parsed.details = {child.errorString()};
        } else if (!child.waitForFinished(perBookTimeoutMs)) {
            child.kill();
            child.waitForFinished(5000);
            parsed.status = QStringLiteral("FAIL");
            parsed.categories = {QStringLiteral("worker-timeout")};
            parsed.details = {QStringLiteral("timed out after %1 ms").arg(perBookTimeoutMs)};
        } else {
            parsed = parseChildOutput(child.readAllStandardOutput());
            if (child.exitStatus() != QProcess::NormalExit) {
                parsed.status = QStringLiteral("FAIL");
                parsed.categories = {QStringLiteral("worker-crash")};
                parsed.details.append(QStringLiteral("child process crashed"));
            }
            const QByteArray stderrBytes = child.readAllStandardError();
            if (!stderrBytes.isEmpty() && parsed.status == QLatin1String("FAIL")) {
                parsed.details.append(QString::fromUtf8(stderrBytes).trimmed());
            }
        }

        for (const QString &category : std::as_const(parsed.categories)) {
            categoryCounts[category] += 1;
        }

        const QString title = parsed.title.isEmpty() ? QFileInfo(book).completeBaseName() : parsed.title;
        out << parsed.status << '\t' << title << '\t' << book;
        if (!parsed.categories.isEmpty()) {
            out << "\t[" << parsed.categories.join(QLatin1Char(',')) << ']';
        }
        out << '\n';
        for (const QString &detail : std::as_const(parsed.details)) {
            out << "  - " << detail << '\n';
        }

        if (parsed.status == QLatin1String("PASS")) {
            ++passed;
        } else if (parsed.status == QLatin1String("UNSUPPORTED")) {
            ++unsupported;
            unsupportedBooks.append(QStringLiteral("%1 (%2)").arg(book, parsed.categories.join(QLatin1Char(','))));
        } else {
            ++failed;
        }
        out.flush();
    }

    QStringList categorySummary;
    const QList<QString> keys = categoryCounts.keys();
    for (const QString &category : keys) {
        categorySummary.append(QStringLiteral("%1=%2").arg(category).arg(categoryCounts.value(category)));
    }
    std::sort(categorySummary.begin(), categorySummary.end());

    out << "SUMMARY total=" << books.size() << " passed=" << passed << " unsupported=" << unsupported << " failed=" << failed << '\n';
    out << "CATEGORIES " << (categorySummary.isEmpty() ? QStringLiteral("none") : categorySummary.join(QLatin1Char(' '))) << '\n';
    if (!unsupportedBooks.isEmpty()) {
        out << "UNSUPPORTED_BOOKS\n";
        for (const QString &book : std::as_const(unsupportedBooks)) {
            out << "  " << book << '\n';
        }
    }

    if (failed > 0) {
        err << "epubreadercorpustest: " << failed << " supported EPUB(s) failed corpus checks\n";
    }
    return failed == 0 ? 0 : 1;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const int bookIndex = args.indexOf(QStringLiteral("--book"));
    if (bookIndex >= 0 && bookIndex + 1 < args.size()) {
        return runWorker(args.at(bookIndex + 1));
    }
    return runParent();
}
