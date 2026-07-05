/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "epubwebreader.h"

#include <KArchiveDirectory>
#include <KArchiveFile>
#ifndef PAPERLIBRARY_EPUBWEBREADER_CORE_ONLY
#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>
#endif
#include <KZip>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QXmlStreamReader>

#ifndef PAPERLIBRARY_EPUBWEBREADER_CORE_ONLY
#include <QAbstractItemView>
#include <QBuffer>
#include <QCloseEvent>
#include <QFrame>
#include <QHideEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPointer>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTimer>
#include <QTreeView>
#include <QUuid>
#include <QVariant>
#include <QVBoxLayout>
#include <QWebEngineDownloadRequest>
#include <QWebEngineFileSystemAccessRequest>
#include <QWebEngineFullScreenRequest>
#include <QWebEngineLoadingInfo>
#include <QWebEngineNavigationRequest>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineRegisterProtocolHandlerRequest>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>
#endif

#include <cmath>
#include <cstdio>

namespace EpubWebReaderCore
{
constexpr double MaxStoredScrollOffset = 1000000000.0;
constexpr int MinFontScaleStep = -5;
constexpr int MaxFontScaleStep = 7;

QString cleanArchivePath(const QString &path)
{
    QString normalized = path;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (normalized.startsWith(QLatin1Char('/'))) {
        normalized.remove(0, 1);
    }

    QStringList cleanParts;
    const QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QLatin1String(".")) {
            continue;
        }
        if (part == QLatin1String("..")) {
            if (cleanParts.isEmpty()) {
                return QString();
            }
            cleanParts.removeLast();
            continue;
        }
        cleanParts.append(part);
    }

    if (cleanParts.isEmpty()) {
        return QString();
    }
    return cleanParts.join(QLatin1Char('/'));
}

QString packageBaseDir(const QString &packagePath)
{
    const QString dir = QFileInfo(packagePath).path();
    return dir == QLatin1String(".") ? QString() : dir;
}

bool isExternalHref(const QString &href)
{
    const QString trimmed = href.trimmed();
    if (trimmed.startsWith(QLatin1String("//"))) {
        return true;
    }
    const QUrl hrefUrl = QUrl::fromEncoded(trimmed.toUtf8(), QUrl::TolerantMode);
    return !hrefUrl.isRelative();
}

QString hrefPathPart(const QString &href, QUrl::ComponentFormattingOptions formatting)
{
    const QString trimmed = href.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')) || isExternalHref(trimmed)) {
        return QString();
    }

    const QUrl hrefUrl = QUrl::fromEncoded(trimmed.toUtf8(), QUrl::TolerantMode);
    QString path = hrefUrl.path(formatting);
    if (!path.isEmpty()) {
        return path;
    }

    QString fallback = trimmed;
    const qsizetype fragment = fallback.indexOf(QLatin1Char('#'));
    if (fragment >= 0) {
        fallback.truncate(fragment);
    }
    const qsizetype query = fallback.indexOf(QLatin1Char('?'));
    if (query >= 0) {
        fallback.truncate(query);
    }
    if (formatting == QUrl::FullyDecoded) {
        fallback = QUrl::fromPercentEncoding(fallback.toUtf8());
    }
    return fallback;
}

void appendCandidate(QStringList &candidates, const QString &path)
{
    const QString clean = cleanArchivePath(path);
    if (!clean.isEmpty() && !candidates.contains(clean)) {
        candidates.append(clean);
    }
}

QStringList resolvePackageHrefCandidates(const QString &opfDir, const QString &href)
{
    QStringList candidates;
    const QString decodedPath = hrefPathPart(href, QUrl::FullyDecoded);
    const QString encodedPath = hrefPathPart(href, QUrl::FullyEncoded);
    const QString prettyPath = hrefPathPart(href, QUrl::PrettyDecoded);

    for (const QString &hrefPath : {decodedPath, prettyPath, encodedPath}) {
        if (hrefPath.isEmpty()) {
            continue;
        }
        const QString combined = opfDir.isEmpty() ? hrefPath : opfDir + QLatin1Char('/') + hrefPath;
        appendCandidate(candidates, combined);
    }
    return candidates;
}

QString resolvePackageHref(const QString &opfDir, const QString &href)
{
    const QStringList candidates = resolvePackageHrefCandidates(opfDir, href);
    return candidates.isEmpty() ? QString() : candidates.constFirst();
}

const KArchiveEntry *entryForPath(const KArchiveDirectory *directory, const QString &path)
{
    const QString clean = cleanArchivePath(path);
    if (clean.isEmpty() || !directory) {
        return nullptr;
    }

    const KArchiveDirectory *current = directory;
    const KArchiveEntry *entry = nullptr;
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) {
        entry = current->entry(parts.at(i));
        if (!entry) {
            return nullptr;
        }
        if (i == parts.size() - 1) {
            return entry;
        }
        if (!entry->isDirectory()) {
            return nullptr;
        }
        current = static_cast<const KArchiveDirectory *>(entry);
    }

    return nullptr;
}

const KArchiveFile *fileEntry(const KZip &zip, const QString &path)
{
    const QString clean = cleanArchivePath(path);
    if (clean.isEmpty() || !zip.directory()) {
        return nullptr;
    }
    const KArchiveEntry *entry = entryForPath(zip.directory(), clean);
    return entry && entry->isFile() ? static_cast<const KArchiveFile *>(entry) : nullptr;
}

QByteArray readArchiveFile(const KZip &zip, const QString &path)
{
    const KArchiveFile *file = fileEntry(zip, path);
    return file ? file->data() : QByteArray();
}

QString firstContainerRootfile(const QByteArray &containerXml)
{
    QXmlStreamReader xml(containerXml);
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("rootfile")) {
            return cleanArchivePath(xml.attributes().value(QLatin1String("full-path")).toString());
        }
    }
    return QString();
}

bool truthy(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    return normalized == QLatin1String("true") || normalized == QLatin1String("yes") || normalized == QLatin1String("1") || normalized == QLatin1String("pre-paginated");
}

QString normalizedText(const QString &value)
{
    QString text = value.trimmed();
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return text;
}

bool isFontObfuscationAlgorithm(const QString &algorithm)
{
    const QString normalized = algorithm.trimmed().toLower();
    return normalized == QLatin1String("http://www.idpf.org/2008/embedding")
        || normalized == QLatin1String("http://ns.adobe.com/pdf/enc#rc")
        || normalized == QLatin1String("http://www.idpf.org/2016/obfuscation");
}

bool hasBlockingEncryption(const QByteArray &encryptionXml)
{
    if (encryptionXml.isEmpty()) {
        return false;
    }

    bool insideEncryptedData = false;
    bool currentEncryptedDataIsBlocking = true;
    bool hasBlockingData = false;

    QXmlStreamReader xml(encryptionXml);
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement && xml.name() == QLatin1String("EncryptedData")) {
            insideEncryptedData = true;
            currentEncryptedDataIsBlocking = true;
        } else if (token == QXmlStreamReader::StartElement && xml.name() == QLatin1String("EncryptionMethod") && insideEncryptedData) {
            const QString algorithm = xml.attributes().value(QLatin1String("Algorithm")).toString();
            currentEncryptedDataIsBlocking = !isFontObfuscationAlgorithm(algorithm);
        } else if (token == QXmlStreamReader::EndElement && xml.name() == QLatin1String("EncryptedData")) {
            hasBlockingData = hasBlockingData || currentEncryptedDataIsBlocking;
            insideEncryptedData = false;
        }
    }

    return xml.hasError() || hasBlockingData;
}

bool isSpineMediaType(const QString &mediaType)
{
    const QString normalized = mediaType.trimmed().toLower();
    return normalized == QLatin1String("application/xhtml+xml")
        || normalized == QLatin1String("text/html")
        || normalized == QLatin1String("application/xml")
        || normalized == QLatin1String("text/xml")
        || normalized == QLatin1String("text/x-oeb1-document");
}

QString localXmlName(QString name)
{
    const qsizetype colon = name.lastIndexOf(QLatin1Char(':'));
    if (colon >= 0) {
        name = name.mid(colon + 1);
    }
    return name.toLower();
}

QString xmlAttributeValue(const QXmlStreamAttributes &attributes, const QString &wantedName)
{
    for (const QXmlStreamAttribute &attribute : attributes) {
        if (localXmlName(attribute.name().toString()) == wantedName || localXmlName(attribute.qualifiedName().toString()) == wantedName) {
            return attribute.value().toString();
        }
    }
    return QString();
}

bool tokenListContains(const QString &value, const QString &wantedToken)
{
    const QString wanted = wantedToken.toLower();
    const QStringList tokens = value.toLower().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        if (token == wanted || token.endsWith(QStringLiteral(":") + wanted)) {
            return true;
        }
    }
    return false;
}

QString archiveBaseDir(const QString &archivePath)
{
    const QString dir = QFileInfo(archivePath).path();
    return dir == QLatin1String(".") ? QString() : dir;
}

QString hrefFragmentPart(const QString &href)
{
    const QUrl url = QUrl::fromEncoded(href.trimmed().toUtf8(), QUrl::TolerantMode);
    QString fragment = url.fragment(QUrl::FullyDecoded);
    if (!fragment.isEmpty()) {
        return fragment;
    }

    const qsizetype hash = href.indexOf(QLatin1Char('#'));
    if (hash < 0 || hash + 1 >= href.size()) {
        return QString();
    }
    return QUrl::fromPercentEncoding(href.mid(hash + 1).toUtf8());
}

QString readablePathTitle(const QString &path, int fallbackIndex)
{
    QString title = QFileInfo(path).completeBaseName();
    title.replace(QRegularExpression(QStringLiteral("[_-]+")), QStringLiteral(" "));
    title = normalizedText(title);
    if (title.isEmpty()) {
        title = QStringLiteral("Section %1").arg(fallbackIndex + 1);
    }
    return title;
}

int spineIndexForHref(const QString &baseDir, const QString &href, const QStringList &spine, const QHash<QString, QString> &manifestPathsByHref)
{
    QStringList candidates;
    const auto appendResolved = [&candidates, &manifestPathsByHref](const QString &candidate) {
        if (candidate.isEmpty()) {
            return;
        }
        appendCandidate(candidates, candidate);
        appendCandidate(candidates, manifestPathsByHref.value(candidate));
    };

    for (const QString &candidate : resolvePackageHrefCandidates(baseDir, href)) {
        appendResolved(candidate);
    }
    for (const QString &candidate : resolvePackageHrefCandidates(QString(), href)) {
        appendResolved(candidate);
    }

    for (const QString &candidate : std::as_const(candidates)) {
        const int index = spine.indexOf(candidate);
        if (index >= 0) {
            return index;
        }
    }
    return -1;
}

void appendNavigationEntry(QList<EpubNavigationEntry> *entries, const QString &title, const QString &fragment, int spineIndex, int level)
{
    if (!entries || spineIndex < 0) {
        return;
    }
    const QString cleanTitle = normalizedText(title);
    if (cleanTitle.isEmpty()) {
        return;
    }
    const int cleanLevel = qMax(1, level);
    for (const EpubNavigationEntry &entry : std::as_const(*entries)) {
        if (entry.spineIndex == spineIndex && entry.title == cleanTitle && entry.fragment == fragment) {
            return;
        }
    }
    entries->append({cleanTitle, fragment, spineIndex, cleanLevel});
}

QList<EpubNavigationEntry> navigationFromNavDocument(const KZip &zip,
                                                     const QString &navPath,
                                                     const QStringList &spine,
                                                     const QHash<QString, QString> &manifestPathsByHref)
{
    QList<EpubNavigationEntry> fallbackEntries;
    QList<EpubNavigationEntry> tocEntries;
    const QByteArray navBytes = readArchiveFile(zip, navPath);
    if (navBytes.isEmpty()) {
        return {};
    }

    QXmlStreamReader xml(navBytes);
    const QString navBaseDir = archiveBaseDir(navPath);
    bool insideNav = false;
    bool currentNavIsToc = false;
    int navDepth = 0;
    int listDepth = 0;
    QList<EpubNavigationEntry> currentEntries;

    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString tag = localXmlName(xml.name().toString());
            const QXmlStreamAttributes attributes = xml.attributes();
            if (!insideNav && tag == QLatin1String("nav")) {
                insideNav = true;
                currentNavIsToc = tokenListContains(xmlAttributeValue(attributes, QStringLiteral("type")), QStringLiteral("toc"))
                    || tokenListContains(xmlAttributeValue(attributes, QStringLiteral("epub:type")), QStringLiteral("toc"));
                navDepth = 1;
                listDepth = 0;
                currentEntries.clear();
                continue;
            }
            if (!insideNav) {
                continue;
            }

            ++navDepth;
            if (tag == QLatin1String("ol") || tag == QLatin1String("ul")) {
                ++listDepth;
            } else if (tag == QLatin1String("a")) {
                const QString href = xmlAttributeValue(attributes, QStringLiteral("href"));
                const int spineIndex = spineIndexForHref(navBaseDir, href, spine, manifestPathsByHref);
                const QString title = xml.readElementText(QXmlStreamReader::IncludeChildElements);
                appendNavigationEntry(&currentEntries, title, hrefFragmentPart(href), spineIndex, listDepth);
                --navDepth;
            }
            continue;
        }

        if (token != QXmlStreamReader::EndElement || !insideNav) {
            continue;
        }

        const QString tag = localXmlName(xml.name().toString());
        if (tag == QLatin1String("ol") || tag == QLatin1String("ul")) {
            listDepth = qMax(0, listDepth - 1);
        }
        --navDepth;
        if (tag == QLatin1String("nav") || navDepth <= 0) {
            if (currentNavIsToc && !currentEntries.isEmpty()) {
                tocEntries = currentEntries;
                break;
            }
            if (fallbackEntries.isEmpty()) {
                fallbackEntries = currentEntries;
            }
            insideNav = false;
            currentNavIsToc = false;
            navDepth = 0;
            listDepth = 0;
            currentEntries.clear();
        }
    }

    return tocEntries.isEmpty() ? fallbackEntries : tocEntries;
}

QList<EpubNavigationEntry> navigationFromNcx(const KZip &zip,
                                             const QString &ncxPath,
                                             const QStringList &spine,
                                             const QHash<QString, QString> &manifestPathsByHref)
{
    struct NcxPoint {
        QString title;
        QString href;
        int level = 1;
        int entryIndex = -1;
    };

    QList<EpubNavigationEntry> entries;
    QList<NcxPoint> stack;
    const QByteArray ncxBytes = readArchiveFile(zip, ncxPath);
    if (ncxBytes.isEmpty()) {
        return entries;
    }

    const QString ncxBaseDir = archiveBaseDir(ncxPath);
    auto materializeTop = [&]() {
        if (stack.isEmpty()) {
            return;
        }
        NcxPoint &point = stack.last();
        if (point.entryIndex >= 0) {
            if (!point.title.trimmed().isEmpty()) {
                entries[point.entryIndex].title = normalizedText(point.title);
            }
            return;
        }
        const int spineIndex = spineIndexForHref(ncxBaseDir, point.href, spine, manifestPathsByHref);
        QString title = point.title;
        if (title.trimmed().isEmpty()) {
            title = readablePathTitle(point.href, spineIndex);
        }
        const int previousSize = entries.size();
        appendNavigationEntry(&entries, title, hrefFragmentPart(point.href), spineIndex, point.level);
        if (entries.size() > previousSize) {
            point.entryIndex = entries.size() - 1;
        }
    };

    QXmlStreamReader xml(ncxBytes);
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString tag = localXmlName(xml.name().toString());
            if (tag == QLatin1String("navpoint")) {
                stack.append({QString(), QString(), static_cast<int>(stack.size()) + 1, -1});
            } else if (tag == QLatin1String("content") && !stack.isEmpty()) {
                stack.last().href = xmlAttributeValue(xml.attributes(), QStringLiteral("src"));
                materializeTop();
            } else if (tag == QLatin1String("text") && !stack.isEmpty()) {
                stack.last().title = normalizedText(xml.readElementText(QXmlStreamReader::IncludeChildElements));
                materializeTop();
            }
        } else if (token == QXmlStreamReader::EndElement && localXmlName(xml.name().toString()) == QLatin1String("navpoint") && !stack.isEmpty()) {
            materializeTop();
            stack.removeLast();
        }
    }

    return entries;
}

QString headingTitleFromMarkup(const QByteArray &data)
{
    QString documentTitle;
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        const QString tag = localXmlName(xml.name().toString());
        if (tag == QLatin1String("h1") || tag == QLatin1String("h2") || tag == QLatin1String("h3")) {
            const QString heading = normalizedText(xml.readElementText(QXmlStreamReader::IncludeChildElements));
            if (!heading.isEmpty()) {
                return heading;
            }
        }
        if (tag == QLatin1String("title") && documentTitle.isEmpty()) {
            documentTitle = normalizedText(xml.readElementText(QXmlStreamReader::IncludeChildElements));
        }
    }
    return documentTitle;
}

QList<EpubNavigationEntry> fallbackNavigationFromSpine(const KZip &zip, const QStringList &spine)
{
    QList<EpubNavigationEntry> entries;
    for (int i = 0; i < spine.size(); ++i) {
        const QByteArray data = readArchiveFile(zip, spine.at(i));
        QString title = headingTitleFromMarkup(data);
        if (title.isEmpty()) {
            title = readablePathTitle(spine.at(i), i);
        }
        appendNavigationEntry(&entries, title, QString(), i, 1);
    }
    return entries;
}

EpubInspection inspectEpub(const QString &path)
{
    EpubInspection result;

    KZip zip(path);
    if (!zip.open(QIODevice::ReadOnly)) {
        return result;
    }

    const KArchiveFile *mimetype = fileEntry(zip, QStringLiteral("mimetype"));
    const QByteArray mimetypeBytes = mimetype ? mimetype->data().trimmed() : QByteArray();
    result.isEpub = mimetypeBytes == QByteArrayLiteral("application/epub+zip") || QFileInfo(path).suffix().compare(QLatin1String("epub"), Qt::CaseInsensitive) == 0;
    if (!result.isEpub) {
        return result;
    }

    result.drmProtected = hasBlockingEncryption(readArchiveFile(zip, QStringLiteral("META-INF/encryption.xml")));

    result.packagePath = firstContainerRootfile(readArchiveFile(zip, QStringLiteral("META-INF/container.xml")));
    if (result.packagePath.isEmpty()) {
        result.title = QFileInfo(path).completeBaseName();
        return result;
    }

    const QByteArray opfBytes = readArchiveFile(zip, result.packagePath);
    if (opfBytes.isEmpty()) {
        result.title = QFileInfo(path).completeBaseName();
        return result;
    }

    result.opfDir = packageBaseDir(result.packagePath);
    const QString opfDir = result.opfDir;
    QHash<QString, QString> pathById;
    QHash<QString, QString> mediaTypeById;
    QHash<QString, QStringList> pathsByBasename;
    QStringList spineIds;
    QString navDocumentPath;
    QString ncxPath;
    QString spineTocId;

    QXmlStreamReader xml(opfBytes);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }

        const QXmlStreamAttributes attributes = xml.attributes();
        if (xml.name() == QLatin1String("item")) {
            const QString id = attributes.value(QLatin1String("id")).toString();
            if (id.isEmpty()) {
                continue;
            }
            const QString href = attributes.value(QLatin1String("href")).toString();
            const QString mediaType = attributes.value(QLatin1String("media-type")).toString();
            const QStringList pathCandidates = resolvePackageHrefCandidates(opfDir, href);
            QString pathInZip = pathCandidates.isEmpty() ? QString() : pathCandidates.constFirst();
            for (const QString &candidate : pathCandidates) {
                if (fileEntry(zip, candidate)) {
                    pathInZip = candidate;
                    break;
                }
            }
            pathById.insert(id, pathInZip);
            mediaTypeById.insert(id, mediaType);
            if (!pathInZip.isEmpty()) {
                result.manifestMediaTypesByPath.insert(pathInZip, mediaType);
                result.manifestPathsByHref.insert(pathInZip, pathInZip);
                pathsByBasename[QFileInfo(pathInZip).fileName()].append(pathInZip);
                for (const QString &candidate : pathCandidates) {
                    result.manifestPathsByHref.insert(candidate, pathInZip);
                }
                for (const QString &hrefPath : resolvePackageHrefCandidates(QString(), href)) {
                    if (!hrefPath.isEmpty()) {
                        result.manifestPathsByHref.insert(hrefPath, pathInZip);
                    }
                }
            }
            const QString properties = attributes.value(QLatin1String("properties")).toString();
            if (tokenListContains(properties, QStringLiteral("rendition:layout-pre-paginated"))) {
                result.fixedLayout = true;
            }
            if (navDocumentPath.isEmpty() && tokenListContains(properties, QStringLiteral("nav"))) {
                navDocumentPath = pathInZip;
            }
            if (ncxPath.isEmpty() && (mediaType.trimmed().compare(QLatin1String("application/x-dtbncx+xml"), Qt::CaseInsensitive) == 0
                                      || pathInZip.endsWith(QLatin1String(".ncx"), Qt::CaseInsensitive))) {
                ncxPath = pathInZip;
            }
        } else if (xml.name() == QLatin1String("spine")) {
            spineTocId = attributes.value(QLatin1String("toc")).toString();
        } else if (xml.name() == QLatin1String("itemref")) {
            const QString linear = attributes.value(QLatin1String("linear")).toString();
            if (linear.compare(QLatin1String("no"), Qt::CaseInsensitive) != 0) {
                spineIds.append(attributes.value(QLatin1String("idref")).toString());
            }
        } else if (xml.name() == QLatin1String("meta")) {
            const QString property = attributes.value(QLatin1String("property")).toString();
            const QString name = attributes.value(QLatin1String("name")).toString();
            QString value = attributes.value(QLatin1String("content")).toString();
            if (value.isEmpty()) {
                value = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
            }
            if ((property == QLatin1String("rendition:layout") && value.trimmed().compare(QLatin1String("pre-paginated"), Qt::CaseInsensitive) == 0)
                || (name == QLatin1String("fixed-layout") && truthy(value))
                || (name == QLatin1String("book-type") && value.trimmed().compare(QLatin1String("fixed-layout"), Qt::CaseInsensitive) == 0)) {
                result.fixedLayout = true;
            }
            if (result.title.isEmpty()
                && (property == QLatin1String("dcterms:title") || property == QLatin1String("dc:title") || property == QLatin1String("title") || name == QLatin1String("title"))) {
                result.title = normalizedText(value);
            }
        } else if (xml.name() == QLatin1String("title") && result.title.isEmpty()) {
            result.title = normalizedText(xml.readElementText(QXmlStreamReader::IncludeChildElements));
        }
    }

    for (auto it = pathsByBasename.cbegin(); it != pathsByBasename.cend(); ++it) {
        QStringList uniquePaths;
        for (const QString &path : it.value()) {
            if (!uniquePaths.contains(path)) {
                uniquePaths.append(path);
            }
        }
        if (uniquePaths.size() == 1) {
            result.manifestPathsByHref.insert(it.key(), uniquePaths.constFirst());
        }
    }

    for (const QString &id : std::as_const(spineIds)) {
        const QString mediaType = mediaTypeById.value(id);
        if (!isSpineMediaType(mediaType)) {
            continue;
        }
        const QString pathInZip = pathById.value(id);
        if (!pathInZip.isEmpty() && fileEntry(zip, pathInZip)) {
            result.spine.append(pathInZip);
        }
    }

    if (ncxPath.isEmpty() && !spineTocId.isEmpty()) {
        ncxPath = pathById.value(spineTocId);
    }

    if (result.title.trimmed().isEmpty()) {
        result.title = QFileInfo(path).completeBaseName();
    }

    if (!result.spine.isEmpty()) {
        if (!navDocumentPath.isEmpty()) {
            result.navigation = navigationFromNavDocument(zip, navDocumentPath, result.spine, result.manifestPathsByHref);
        }
        if (result.navigation.isEmpty() && !ncxPath.isEmpty()) {
            result.navigation = navigationFromNcx(zip, ncxPath, result.spine, result.manifestPathsByHref);
        }
        if (result.navigation.isEmpty()) {
            result.navigation = fallbackNavigationFromSpine(zip, result.spine);
        }
    }

    return result;
}

QString contentTypeForPath(const QString &path, const QString &manifestMediaType)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("xhtml") || suffix == QLatin1String("html") || suffix == QLatin1String("htm")) {
        return QStringLiteral("application/xhtml+xml; charset=utf-8");
    }
    if (suffix == QLatin1String("css")) {
        return QStringLiteral("text/css; charset=utf-8");
    }
    if (suffix == QLatin1String("svg")) {
        return QStringLiteral("image/svg+xml");
    }
    if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")) {
        return QStringLiteral("image/jpeg");
    }
    if (suffix == QLatin1String("png")) {
        return QStringLiteral("image/png");
    }
    if (suffix == QLatin1String("gif")) {
        return QStringLiteral("image/gif");
    }
    if (suffix == QLatin1String("webp")) {
        return QStringLiteral("image/webp");
    }
    if (suffix == QLatin1String("avif")) {
        return QStringLiteral("image/avif");
    }
    if (suffix == QLatin1String("bmp")) {
        return QStringLiteral("image/bmp");
    }
    if (suffix == QLatin1String("tif") || suffix == QLatin1String("tiff")) {
        return QStringLiteral("image/tiff");
    }
    if (suffix == QLatin1String("otf")) {
        return QStringLiteral("font/otf");
    }
    if (suffix == QLatin1String("ttf")) {
        return QStringLiteral("font/ttf");
    }
    if (suffix == QLatin1String("woff")) {
        return QStringLiteral("font/woff");
    }
    if (suffix == QLatin1String("woff2")) {
        return QStringLiteral("font/woff2");
    }
    if (suffix == QLatin1String("eot")) {
        return QStringLiteral("application/vnd.ms-fontobject");
    }
    if (suffix == QLatin1String("ncx")) {
        return QStringLiteral("application/x-dtbncx+xml");
    }
    if (suffix == QLatin1String("opf")) {
        return QStringLiteral("application/oebps-package+xml");
    }
    if (suffix == QLatin1String("xml")) {
        return QStringLiteral("application/xml");
    }
    if (suffix == QLatin1String("smil")) {
        return QStringLiteral("application/smil+xml");
    }
    if (suffix == QLatin1String("mp3")) {
        return QStringLiteral("audio/mpeg");
    }
    if (suffix == QLatin1String("m4a")) {
        return QStringLiteral("audio/mp4");
    }
    if (suffix == QLatin1String("mp4")) {
        return QStringLiteral("video/mp4");
    }
    if (suffix == QLatin1String("ogg") || suffix == QLatin1String("oga") || suffix == QLatin1String("opus")) {
        return QStringLiteral("audio/ogg");
    }
    if (suffix == QLatin1String("webm")) {
        return QStringLiteral("video/webm");
    }
    if (suffix == QLatin1String("wav")) {
        return QStringLiteral("audio/wav");
    }

    if (!manifestMediaType.trimmed().isEmpty()) {
        return manifestMediaType.trimmed();
    }

    const QMimeType mime = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension);
    return mime.isValid() ? mime.name() : QStringLiteral("application/octet-stream");
}

ArchiveLookup lookupArchivePath(const KZip &zip, const QString &requestPath, const QString &opfDir, const QHash<QString, QString> &manifestPathsByHref)
{
    ArchiveLookup lookup;
    lookup.requestedPath = cleanArchivePath(requestPath);
    appendCandidate(lookup.candidates, lookup.requestedPath);
    appendCandidate(lookup.candidates, manifestPathsByHref.value(lookup.requestedPath));
    if (!opfDir.isEmpty() && !lookup.requestedPath.startsWith(opfDir + QLatin1Char('/'))) {
        appendCandidate(lookup.candidates, opfDir + QLatin1Char('/') + lookup.requestedPath);
    }

    for (const QString &candidate : std::as_const(lookup.candidates)) {
        if (fileEntry(zip, candidate)) {
            lookup.zipEntry = candidate;
            lookup.found = true;
            return lookup;
        }
    }

    if (!lookup.candidates.isEmpty()) {
        lookup.zipEntry = lookup.candidates.constLast();
    }
    return lookup;
}

QString positionKey(const QString &epubPath)
{
    const QString canonical = QFileInfo(epubPath).canonicalFilePath();
    const QByteArray seed = canonical.isEmpty() ? epubPath.toUtf8() : canonical.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Sha1).toHex());
}

double cleanRestoredScrollOffset(double scrollOffset)
{
    if (!std::isfinite(scrollOffset) || scrollOffset <= 0.0 || scrollOffset > MaxStoredScrollOffset) {
        return 0.0;
    }
    return scrollOffset;
}

bool isRestorableScrollOffset(double scrollOffset)
{
    return std::isfinite(scrollOffset) && scrollOffset >= 0.0 && scrollOffset <= MaxStoredScrollOffset;
}

double cleanReportedScrollOffset(double scrollOffset)
{
    if (!std::isfinite(scrollOffset) || scrollOffset <= 0.0) {
        return 0.0;
    }
    return qMin(scrollOffset, MaxStoredScrollOffset);
}

int cleanFontScaleStep(int step)
{
    return qBound(MinFontScaleStep, step, MaxFontScaleStep);
}

StoredPosition restoreStoredPosition(const QStringList &saved, int spineCount)
{
    StoredPosition restored;
    restored.status = QStringLiteral("missing");
    if (spineCount <= 0) {
        restored.status = QStringLiteral("empty-spine");
        return restored;
    }

    if (saved.size() < 2) {
        return restored;
    }

    bool indexOk = false;
    bool offsetOk = false;
    const int savedIndex = saved.at(0).toInt(&indexOk);
    const double savedOffset = saved.at(1).toDouble(&offsetOk);
    if (indexOk && savedIndex >= 0 && savedIndex < spineCount && offsetOk && isRestorableScrollOffset(savedOffset)) {
        restored.spineIndex = savedIndex;
        restored.scrollOffset = cleanRestoredScrollOffset(savedOffset);
        restored.restored = true;
        restored.status = QStringLiteral("restored");
        return restored;
    }

    restored.status = QStringLiteral("invalid-start");
    return restored;
}

}

#ifndef PAPERLIBRARY_EPUBWEBREADER_CORE_ONLY
namespace
{
using namespace EpubWebReaderCore;

constexpr auto EpubScheme = "paperlibrary-epub";
constexpr auto PositionsGroup = "EpubWebReader Positions";
constexpr auto SettingsGroup = "EpubWebReader Settings";
constexpr auto BookScrollModesGroup = "EpubWebReader Book Scroll Modes";
constexpr auto GeneralGroup = "General";
constexpr auto FontScaleStepKey = "FontScaleStep";
constexpr auto ScrollModeKey = "ScrollMode";
constexpr auto ReaderMotionKey = "ReaderMotion";
constexpr auto PaginatedScrollMode = "paginated";
constexpr auto ContinuousScrollMode = "continuous";
constexpr auto ReaderCommandPrefix = ".paperlibrary/";
constexpr int NavigationEntryIndexRole = Qt::UserRole + 1;
constexpr int MaxReaderHistoryLocations = 80;
constexpr double ReaderHistoryDuplicateTolerance = 48.0;
constexpr double ContinuousScrollHistoryStep = 1600.0;

void epubReaderLog(const QString &message)
{
    const QByteArray bytes = message.toUtf8();
    std::fprintf(stderr, "[epubreader] %s\n", bytes.constData());
    std::fflush(stderr);
}

QString jsStringLiteral(const QString &value)
{
    const QByteArray json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    QString literal = QString::fromUtf8(json);
    literal.remove(0, 1);
    literal.chop(1);
    return literal;
}

QString readResourceText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QString readerCommandForUrl(const QUrl &url, const QString &bookId)
{
    if (url.scheme() != QLatin1String(EpubScheme) || url.host() != bookId) {
        return QString();
    }

    const QString path = cleanArchivePath(url.path(QUrl::FullyDecoded));
    if (!path.startsWith(QLatin1String(ReaderCommandPrefix))) {
        return QString();
    }
    return path.mid(QString::fromLatin1(ReaderCommandPrefix).size());
}

int readFontScaleStep()
{
    const KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(SettingsGroup));
    return cleanFontScaleStep(group.readEntry(FontScaleStepKey, 0));
}

void writeFontScaleStep(int step)
{
    KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(SettingsGroup));
    group.writeEntry(FontScaleStepKey, cleanFontScaleStep(step));
    group.sync();
}

QString scrollModeValue(EpubWebReader::ScrollMode mode)
{
    return mode == EpubWebReader::ScrollMode::Continuous ? QString::fromLatin1(ContinuousScrollMode) : QString::fromLatin1(PaginatedScrollMode);
}

EpubWebReader::ScrollMode scrollModeFromValue(const QString &value, EpubWebReader::ScrollMode fallback = EpubWebReader::ScrollMode::Paginated)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String(ContinuousScrollMode)) {
        return EpubWebReader::ScrollMode::Continuous;
    }
    if (normalized == QLatin1String(PaginatedScrollMode)) {
        return EpubWebReader::ScrollMode::Paginated;
    }
    return fallback;
}

bool readBookScrollMode(const QString &epubPath, EpubWebReader::ScrollMode *mode)
{
    const QString key = positionKey(epubPath);
    if (key.isEmpty()) {
        return false;
    }
    const KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(BookScrollModesGroup));
    if (!group.hasKey(key)) {
        return false;
    }
    if (mode) {
        *mode = scrollModeFromValue(group.readEntry(key, QString()), EpubWebReader::globalScrollMode());
    }
    return true;
}

void writeBookScrollMode(const QString &epubPath, EpubWebReader::ScrollMode mode)
{
    KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(BookScrollModesGroup));
    group.writeEntry(positionKey(epubPath), scrollModeValue(mode));
    group.sync();
}

void deleteBookScrollMode(const QString &epubPath)
{
    KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(BookScrollModesGroup));
    group.deleteEntry(positionKey(epubPath));
    group.sync();
}

class EpubSchemeHandler : public QWebEngineUrlSchemeHandler
{
public:
    EpubSchemeHandler(const QString &bookId,
                      const QString &epubPath,
                      const QString &opfDir,
                      const QHash<QString, QString> &manifestMediaTypesByPath,
                      const QHash<QString, QString> &manifestPathsByHref,
                      QObject *parent)
        : QWebEngineUrlSchemeHandler(parent)
        , m_bookId(bookId)
        , m_opfDir(opfDir)
        , m_manifestMediaTypesByPath(manifestMediaTypesByPath)
        , m_manifestPathsByHref(manifestPathsByHref)
        , m_zip(epubPath)
    {
        m_open = m_zip.open(QIODevice::ReadOnly);
    }

    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        const QUrl requestUrl = job->requestUrl();
        ServedLookup lookup = lookupRequest(requestUrl);

        if (!m_open || requestUrl.scheme() != QLatin1String(EpubScheme) || requestUrl.host() != m_bookId) {
            logRequest(job, lookup, false, QString(), 0, QStringLiteral("denied: open=%1 expected-book-id=%2").arg(m_open ? QStringLiteral("true") : QStringLiteral("false"), m_bookId));
            job->fail(QWebEngineUrlRequestJob::RequestDenied);
            return;
        }
        if (job->requestMethod() != QByteArrayLiteral("GET") && job->requestMethod() != QByteArrayLiteral("HEAD")) {
            logRequest(job, lookup, false, QString(), 0, QStringLiteral("denied: unsupported method"));
            job->fail(QWebEngineUrlRequestJob::RequestDenied);
            return;
        }

        if (!lookup.file) {
            logRequest(job, lookup, false, QString(), 0, QStringLiteral("not found"));
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }

        const QByteArray data = lookup.file->data();
        const QString contentType = contentTypeForPath(lookup.zipEntry, m_manifestMediaTypesByPath.value(lookup.zipEntry));
        QMultiMap<QByteArray, QByteArray> headers;
        headers.insert(QByteArrayLiteral("Content-Security-Policy"),
                       QByteArrayLiteral("default-src paperlibrary-epub: data:; img-src paperlibrary-epub: data:; font-src paperlibrary-epub: data:; style-src paperlibrary-epub: 'unsafe-inline'; script-src 'none'; connect-src 'none'; media-src paperlibrary-epub: data:; object-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'"));
        headers.insert(QByteArrayLiteral("Content-Length"), QByteArray::number(data.size()));
        headers.insert(QByteArrayLiteral("X-Content-Type-Options"), QByteArrayLiteral("nosniff"));
        job->setAdditionalResponseHeaders(headers);

        QBuffer *buffer = new QBuffer(job);
        if (job->requestMethod() == QByteArrayLiteral("GET")) {
            buffer->setData(data);
        }
        buffer->open(QIODevice::ReadOnly);
        job->reply(contentType.toUtf8(), buffer);
        logRequest(job, lookup, true, contentType, buffer->size(), QString());
    }

private:
    struct ServedLookup {
        QString requestedPath;
        QString zipEntry;
        QStringList candidates;
        const KArchiveFile *file = nullptr;
    };

    ServedLookup lookupRequest(const QUrl &requestUrl) const
    {
        const EpubWebReaderCore::ArchiveLookup coreLookup = lookupArchivePath(m_zip, requestUrl.path(QUrl::FullyDecoded), m_opfDir, m_manifestPathsByHref);
        ServedLookup lookup;
        lookup.requestedPath = coreLookup.requestedPath;
        lookup.zipEntry = coreLookup.zipEntry;
        lookup.candidates = coreLookup.candidates;
        if (coreLookup.found) {
            lookup.file = fileEntry(m_zip, coreLookup.zipEntry);
        }
        return lookup;
    }

    void logRequest(QWebEngineUrlRequestJob *job, const ServedLookup &lookup, bool found, const QString &contentType, qint64 byteCount, const QString &note) const
    {
        const QUrl requestUrl = job->requestUrl();
        epubReaderLog(QStringLiteral("request method=%1 url=%2 book-id=%3 host=%4 derived-path=%5 zip-entry=%6 candidates=%7 status=%8 content-type=%9 bytes=%10%11")
                          .arg(QString::fromLatin1(job->requestMethod()),
                               QString::fromUtf8(requestUrl.toEncoded()),
                               m_bookId,
                               requestUrl.host(),
                               lookup.requestedPath,
                               lookup.zipEntry,
                               lookup.candidates.join(QLatin1Char(',')),
                               found ? QStringLiteral("FOUND") : QStringLiteral("NOT-FOUND"),
                               contentType,
                               QString::number(byteCount),
                               note.isEmpty() ? QString() : QStringLiteral(" note=%1").arg(note)));
    }

    QString m_bookId;
    QString m_opfDir;
    QHash<QString, QString> m_manifestMediaTypesByPath;
    QHash<QString, QString> m_manifestPathsByHref;
    KZip m_zip;
    bool m_open = false;
};

class LockedDownRequestInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
    explicit LockedDownRequestInterceptor(const QString &bookId, QObject *parent)
        : QWebEngineUrlRequestInterceptor(parent)
        , m_bookId(bookId)
    {
    }

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QUrl requestUrl = info.requestUrl();
        const QString scheme = requestUrl.scheme();
        const QWebEngineUrlRequestInfo::ResourceType resourceType = info.resourceType();
        const bool isDownload = info.isDownload();
        if (isDownload) {
            info.block(true);
            return;
        }
        if (scheme == QLatin1String(EpubScheme) && requestUrl.host() == m_bookId) {
            if (resourceType == QWebEngineUrlRequestInfo::ResourceTypeScript || resourceType == QWebEngineUrlRequestInfo::ResourceTypeWorker
                || resourceType == QWebEngineUrlRequestInfo::ResourceTypeSharedWorker || resourceType == QWebEngineUrlRequestInfo::ResourceTypeServiceWorker) {
                info.block(true);
            }
            return;
        }
        if (scheme == QLatin1String("data") && resourceType != QWebEngineUrlRequestInfo::ResourceTypeMainFrame) {
            return;
        }
        if (scheme == QLatin1String("about")) {
            return;
        }

        info.block(true);
    }

private:
    QString m_bookId;
};

class LockedDownPage : public QWebEnginePage
{
public:
    LockedDownPage(QWebEngineProfile *profile, const QString &bookId, QObject *parent)
        : QWebEnginePage(profile, parent)
        , m_bookId(bookId)
    {
    }

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override
    {
        Q_UNUSED(type);
        if (url.scheme() == QLatin1String(EpubScheme) && url.host() == m_bookId) {
            return true;
        }
        if (!isMainFrame && url.scheme() == QLatin1String("data")) {
            return true;
        }
        if (url.scheme() == QLatin1String("about")) {
            return true;
        }
        return false;
    }

    QWebEnginePage *createWindow(WebWindowType type) override
    {
        Q_UNUSED(type);
        return nullptr;
    }

    QStringList chooseFiles(FileSelectionMode mode, const QStringList &oldFiles, const QStringList &acceptedMimeTypes) override
    {
        Q_UNUSED(mode);
        Q_UNUSED(oldFiles);
        Q_UNUSED(acceptedMimeTypes);
        return {};
    }

    void javaScriptAlert(const QUrl &securityOrigin, const QString &msg) override
    {
        Q_UNUSED(securityOrigin);
        Q_UNUSED(msg);
    }

    bool javaScriptConfirm(const QUrl &securityOrigin, const QString &msg) override
    {
        Q_UNUSED(securityOrigin);
        Q_UNUSED(msg);
        return false;
    }

    bool javaScriptPrompt(const QUrl &securityOrigin, const QString &msg, const QString &defaultValue, QString *result) override
    {
        Q_UNUSED(securityOrigin);
        Q_UNUSED(msg);
        Q_UNUSED(defaultValue);
        if (result) {
            result->clear();
        }
        return false;
    }

    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString &message, int lineNumber, const QString &sourceID) override
    {
        epubReaderLog(QStringLiteral("jsConsole level=%1 source=%2 line=%3 message=%4").arg(static_cast<int>(level)).arg(sourceID).arg(lineNumber).arg(message));
    }

private:
    QString m_bookId;
};
}

EpubWebReader::EpubWebReader(QWidget *parent)
    : QWidget(parent)
    , m_fontScaleStep(readFontScaleStep())
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_outlineWidget = new QWidget;
    m_outlineWidget->setObjectName(QStringLiteral("paperlibrary_epub_outline"));
    m_outlineWidget->setStyleSheet(QStringLiteral(
        "QWidget#paperlibrary_epub_outline { background: palette(base); }"
        "QLabel#paperlibrary_epub_outline_title { font-weight: 650; color: palette(text); }"));
    auto *outlineLayout = new QVBoxLayout(m_outlineWidget);
    outlineLayout->setContentsMargins(12, 12, 10, 12);
    outlineLayout->setSpacing(8);

    m_outlineTitle = new QLabel(i18n("Contents"), m_outlineWidget);
    m_outlineTitle->setObjectName(QStringLiteral("paperlibrary_epub_outline_title"));
    outlineLayout->addWidget(m_outlineTitle);

    m_outlineModel = new QStandardItemModel(this);
    m_outlineView = new QTreeView(m_outlineWidget);
    m_outlineView->setObjectName(QStringLiteral("paperlibrary_epub_outline_tree"));
    m_outlineView->setModel(m_outlineModel);
    m_outlineView->setHeaderHidden(true);
    m_outlineView->setFrameShape(QFrame::NoFrame);
    m_outlineView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_outlineView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_outlineView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_outlineView->setUniformRowHeights(true);
    m_outlineView->setRootIsDecorated(true);
    m_outlineView->setExpandsOnDoubleClick(false);
    m_outlineView->setIndentation(14);
    m_outlineView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_outlineView->setStyleSheet(QStringLiteral(
        "QTreeView { border: none; background: transparent; outline: 0; }"
        "QTreeView::item { min-height: 24px; padding: 4px 6px; border-radius: 5px; }"
        "QTreeView::item:selected { background: rgba(0, 122, 255, 46); }"
        "QTreeView::item:hover { background: rgba(127, 127, 127, 34); }"));
    outlineLayout->addWidget(m_outlineView, 1);

    connect(m_outlineView, &QTreeView::clicked, this, &EpubWebReader::jumpToNavigationEntry);
    connect(m_outlineView, &QTreeView::activated, this, &EpubWebReader::jumpToNavigationEntry);
}

EpubWebReader::~EpubWebReader()
{
    saveReadingPosition();
}

EpubWebReader::ScrollMode EpubWebReader::globalScrollMode()
{
    const KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(SettingsGroup));
    return scrollModeFromValue(group.readEntry(ScrollModeKey, QString::fromLatin1(PaginatedScrollMode)));
}

void EpubWebReader::setGlobalScrollMode(ScrollMode mode)
{
    KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(SettingsGroup));
    group.writeEntry(ScrollModeKey, scrollModeValue(mode));
    group.sync();
}

void EpubWebReader::registerUrlScheme()
{
    QWebEngineUrlScheme scheme{QByteArray(EpubScheme)};
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    scheme.setDefaultPort(QWebEngineUrlScheme::PortUnspecified);
    // LocalAccessAllowed is essential: without it a LocalScheme document
    // (e.g. titlepage.xhtml) is forbidden from loading other paperlibrary-epub://
    // resources (its own images/CSS/fonts) — the subresource fetch is blocked
    // before it ever reaches the scheme handler.
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalScheme | QWebEngineUrlScheme::LocalAccessAllowed | QWebEngineUrlScheme::CorsEnabled | QWebEngineUrlScheme::FetchApiAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);
}

void EpubWebReader::configureBuildTreeRuntime()
{
#if defined(Q_OS_MACOS)
    const QString craftRoot = qEnvironmentVariableIsSet("CRAFT_ROOT") ? QString::fromLocal8Bit(qgetenv("CRAFT_ROOT")) : QDir::homePath() + QStringLiteral("/CraftRoot");
    const QString frameworkRoot = craftRoot + QStringLiteral("/lib/QtWebEngineCore.framework/Versions/A");
    const QString processPath = frameworkRoot + QStringLiteral("/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess");
    const QString resourcesPath = frameworkRoot + QStringLiteral("/Resources");
    const QString localesPath = resourcesPath + QStringLiteral("/qtwebengine_locales");

    if (!qEnvironmentVariableIsSet("QTWEBENGINEPROCESS_PATH") && QFileInfo::exists(processPath)) {
        qputenv("QTWEBENGINEPROCESS_PATH", QFile::encodeName(processPath));
    }
    if (!qEnvironmentVariableIsSet("QTWEBENGINE_RESOURCES_PATH") && QFileInfo(resourcesPath).isDir()) {
        qputenv("QTWEBENGINE_RESOURCES_PATH", QFile::encodeName(resourcesPath));
    }
    if (!qEnvironmentVariableIsSet("QTWEBENGINE_LOCALES_PATH") && QFileInfo(localesPath).isDir()) {
        qputenv("QTWEBENGINE_LOCALES_PATH", QFile::encodeName(localesPath));
    }
#endif
}

bool EpubWebReader::canOpen(const QUrl &url)
{
    if (!url.isLocalFile()) {
        return false;
    }

    const QFileInfo info(url.toLocalFile());
    if (!info.isFile()) {
        return false;
    }

    const QMimeType mime = QMimeDatabase().mimeTypeForFile(info);
    if (mime.name() != QLatin1String("application/epub+zip") && info.suffix().compare(QLatin1String("epub"), Qt::CaseInsensitive) != 0) {
        return false;
    }

    const EpubInspection inspection = inspectEpub(info.absoluteFilePath());
    epubReaderLog(QStringLiteral("inspect path=%1 supported=%2 is-epub=%3 drm=%4 fixed-layout=%5 package=%6 opf-dir=%7 spine-count=%8")
                      .arg(info.absoluteFilePath(),
                           inspection.supported() ? QStringLiteral("true") : QStringLiteral("false"),
                           inspection.isEpub ? QStringLiteral("true") : QStringLiteral("false"),
                           inspection.drmProtected ? QStringLiteral("true") : QStringLiteral("false"),
                           inspection.fixedLayout ? QStringLiteral("true") : QStringLiteral("false"),
                           inspection.packagePath,
                           inspection.opfDir,
                           QString::number(inspection.spine.size())));
    return inspection.supported();
}

bool EpubWebReader::readerMotionEnabled()
{
    const KConfigGroup group = KSharedConfig::openConfig()->group(QString::fromLatin1(GeneralGroup));
    return group.readEntry(ReaderMotionKey, true);
}

void EpubWebReader::setReaderMotionEnabled(bool enabled)
{
    KConfigGroup group = KSharedConfig::openConfig()->group(QString::fromLatin1(GeneralGroup));
    group.writeEntry(ReaderMotionKey, enabled);
    group.sync();
}

bool EpubWebReader::open(const QUrl &url)
{
    if (!url.isLocalFile()) {
        return false;
    }

    const QFileInfo info(url.toLocalFile());
    const EpubInspection inspection = inspectEpub(info.absoluteFilePath());
    if (!inspection.supported()) {
        return false;
    }

    saveReadingPosition();

    if (m_view) {
        layout()->removeWidget(m_view);
        m_view->deleteLater();
    }
    if (m_profile) {
        m_profile->deleteLater();
    }
    m_view = nullptr;
    m_page = nullptr;
    m_profile = nullptr;
    m_schemeHandler = nullptr;
    m_requestInterceptor = nullptr;

    m_url = url;
    m_epubPath = info.absoluteFilePath();
    m_bookId = QUuid::createUuid().toString(QUuid::Id128);
    m_bookTitle = inspection.title.trimmed();
    if (m_bookTitle.isEmpty()) {
        m_bookTitle = info.completeBaseName();
    }
    m_spine = inspection.spine;
    m_navigationEntries = inspection.navigation;
    m_spineIndex = 0;
    m_pendingScrollOffset = 0.0;
    m_havePendingScrollOffset = false;
    m_pendingScrollToEnd = false;
    m_backStack.clear();
    m_forwardStack.clear();
    m_haveHistoryAnchor = false;
    m_suppressHistoryCapture = false;
    updateHistoryAvailability();
    m_fontScaleStep = readFontScaleStep();
    m_hasBookScrollModeOverride = readBookScrollMode(m_epubPath, &m_scrollMode);
    if (!m_hasBookScrollModeOverride) {
        m_scrollMode = globalScrollMode();
    }
    rebuildOutlineModel();
    Q_EMIT outlineAvailableChanged(hasOutline());

    epubReaderLog(QStringLiteral("open path=%1 book-id=%2 title=%3 package=%4 opf-dir=%5 spine-count=%6 nav-count=%7 first-spine=%8 scroll-mode=%9%10")
                      .arg(m_epubPath,
                           m_bookId,
                           m_bookTitle,
                           inspection.packagePath,
                           inspection.opfDir,
                           QString::number(m_spine.size()),
                           QString::number(m_navigationEntries.size()),
                           m_spine.isEmpty() ? QString() : m_spine.constFirst(),
                           scrollModeValue(m_scrollMode),
                           m_hasBookScrollModeOverride ? QStringLiteral(" override") : QString()));

    const KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(PositionsGroup));
    const QString key = positionKey(m_epubPath);
    const QStringList saved = group.readEntry(key, QStringList());
    const StoredPosition restoredPosition = restoreStoredPosition(saved, m_spine.size());
    m_spineIndex = restoredPosition.spineIndex;
    m_pendingScrollOffset = restoredPosition.scrollOffset;
    m_havePendingScrollOffset = restoredPosition.scrollOffset > 0.0;
    epubReaderLog(QStringLiteral("restorePosition key=%1 saved=%2 status=%3 restored-spine-index=%4 restored-scroll-offset=%5")
                      .arg(key,
                           saved.join(QLatin1Char(',')),
                           restoredPosition.status,
                           QString::number(m_spineIndex),
                           QString::number(m_pendingScrollOffset, 'f', 0)));

    Q_EMIT titleChanged(m_bookTitle);
    QTimer::singleShot(0, this, [this]() {
        Q_EMIT titleChanged(m_bookTitle);
    });

    m_profile = new QWebEngineProfile(this);
    m_profile->setHttpCacheType(QWebEngineProfile::NoCache);
    m_profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    m_profile->setPersistentPermissionsPolicy(QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);
    m_profile->setPushServiceEnabled(false);
    m_profile->setSpellCheckEnabled(false);

    QWebEngineSettings *profileSettings = m_profile->settings();
    // The app-owned pagination script runs in ApplicationWorld. Book scripts
    // remain blocked by the response CSP and by the request interceptor below.
    profileSettings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    profileSettings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    profileSettings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    profileSettings->setAttribute(QWebEngineSettings::JavascriptCanPaste, false);
    profileSettings->setAttribute(QWebEngineSettings::LocalStorageEnabled, false);
    profileSettings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    profileSettings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    profileSettings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    profileSettings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, false);
    profileSettings->setAttribute(QWebEngineSettings::ScreenCaptureEnabled, false);
    profileSettings->setAttribute(QWebEngineSettings::HyperlinkAuditingEnabled, false);
    profileSettings->setAttribute(QWebEngineSettings::NavigateOnDropEnabled, false);
    profileSettings->setAttribute(QWebEngineSettings::PdfViewerEnabled, false);
    profileSettings->setAttribute(QWebEngineSettings::ShowScrollBars, false);
    profileSettings->setUnknownUrlSchemePolicy(QWebEngineSettings::DisallowUnknownUrlSchemes);
    profileSettings->setFontFamily(QWebEngineSettings::SerifFont, QStringLiteral("New York"));
    profileSettings->setFontFamily(QWebEngineSettings::StandardFont, QStringLiteral("New York"));
    profileSettings->setFontSize(QWebEngineSettings::DefaultFontSize, 18);

    m_schemeHandler = new EpubSchemeHandler(m_bookId, m_epubPath, inspection.opfDir, inspection.manifestMediaTypesByPath, inspection.manifestPathsByHref, m_profile);
    m_profile->installUrlSchemeHandler(QByteArray(EpubScheme), m_schemeHandler);
    m_requestInterceptor = new LockedDownRequestInterceptor(m_bookId, m_profile);
    m_profile->setUrlRequestInterceptor(m_requestInterceptor);
    connect(m_profile, &QWebEngineProfile::downloadRequested, this, [](QWebEngineDownloadRequest *download) {
        download->cancel();
    });

    m_page = new LockedDownPage(m_profile, m_bookId, this);
    m_page->setBackgroundColor(QColor(QStringLiteral("#f7f7f4")));
    QWebEngineSettings *pageSettings = m_page->settings();
    pageSettings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    pageSettings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    pageSettings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    pageSettings->setAttribute(QWebEngineSettings::JavascriptCanPaste, false);
    pageSettings->setUnknownUrlSchemePolicy(QWebEngineSettings::DisallowUnknownUrlSchemes);

    installReaderScripts();

    connect(m_page, &QWebEnginePage::titleChanged, this, [this](const QString &pageTitle) {
        epubReaderLog(QStringLiteral("pageTitleChanged ignored-tab-title=%1 book-title=%2").arg(pageTitle, m_bookTitle));
    });
    connect(m_page, &QWebEnginePage::loadingChanged, this, [](const QWebEngineLoadingInfo &info) {
        epubReaderLog(QStringLiteral("loadingChanged status=%1 url=%2 is-error-page=%3 error-domain=%4 error-code=%5 error-string=%6")
                          .arg(QString::number(static_cast<int>(info.status())),
                               QString::fromUtf8(info.url().toEncoded()),
                               info.isErrorPage() ? QStringLiteral("true") : QStringLiteral("false"),
                               QString::number(static_cast<int>(info.errorDomain())),
                               QString::number(info.errorCode()),
                               info.errorString()));
    });
    connect(m_page, &QWebEnginePage::loadFinished, this, [this](bool ok) {
        epubReaderLog(QStringLiteral("loadFinished ok=%1 url=%2 spine-index=%3 spine=%4")
                          .arg(ok ? QStringLiteral("true") : QStringLiteral("false"),
                               m_page ? QString::fromUtf8(m_page->url().toEncoded()) : QString(),
                               QString::number(m_spineIndex),
                               (m_spineIndex >= 0 && m_spineIndex < m_spine.size()) ? m_spine.at(m_spineIndex) : QString()));
        if (ok) {
            applyPendingScrollOffset();
            if (!m_havePendingScrollOffset) {
                m_suppressHistoryCapture = false;
            }
        }
        Q_EMIT loadFinished(ok);
    });
    connect(m_page, &QWebEnginePage::urlChanged, this, [this](const QUrl &newUrl) {
        if (newUrl.scheme() != QLatin1String(EpubScheme) || newUrl.host() != m_bookId) {
            return;
        }
        const QString path = cleanArchivePath(newUrl.path(QUrl::FullyDecoded));
        const int index = m_spine.indexOf(path);
        if (index >= 0) {
            m_spineIndex = index;
            selectCurrentNavigationEntry();
            saveReadingPosition();
        }
    });
    connect(m_page, &QWebEnginePage::scrollPositionChanged, this, [this](const QPointF &position) {
        if (m_havePendingScrollOffset) {
            return;
        }
        const double offset = m_scrollMode == ScrollMode::Continuous ? position.y() : position.x();
        captureScrollHistoryIfNeeded(offset);
        saveScrollOffset(offset);
        saveReadingPosition();
    });
    connect(m_page, &QWebEnginePage::renderProcessTerminated, this, [this](QWebEnginePage::RenderProcessTerminationStatus status, int exitCode) {
        epubReaderLog(QStringLiteral("renderProcessTerminated status=%1 exit-code=%2").arg(static_cast<int>(status)).arg(exitCode));
        Q_EMIT renderProcessTerminated(QStringLiteral("QtWebEngine render process terminated: status=%1 exitCode=%2").arg(static_cast<int>(status)).arg(exitCode));
    });
    connect(m_page, &QWebEnginePage::permissionRequested, this, [](QWebEnginePermission permission) {
        permission.deny();
    });
    connect(m_page, &QWebEnginePage::fullScreenRequested, this, [](QWebEngineFullScreenRequest request) {
        request.reject();
    });
    connect(m_page, &QWebEnginePage::registerProtocolHandlerRequested, this, [](QWebEngineRegisterProtocolHandlerRequest request) {
        request.reject();
    });
    connect(m_page, &QWebEnginePage::fileSystemAccessRequested, this, [](QWebEngineFileSystemAccessRequest request) {
        request.reject();
    });
    connect(m_page, &QWebEnginePage::newWindowRequested, this, [](QWebEngineNewWindowRequest &request) {
        Q_UNUSED(request);
    });
    connect(m_page, &QWebEnginePage::navigationRequested, this, [this](QWebEngineNavigationRequest &request) {
        const QUrl requestUrl = request.url();
        const QString readerCommand = readerCommandForUrl(requestUrl, m_bookId);
        if (!readerCommand.isEmpty()) {
            request.reject();
            epubReaderLog(QStringLiteral("navigationCommand command=%1 url=%2 spine-index=%3 spine-count=%4")
                              .arg(readerCommand, QString::fromUtf8(requestUrl.toEncoded()), QString::number(m_spineIndex), QString::number(m_spine.size())));
            QTimer::singleShot(0, this, [this, readerCommand]() {
                if (readerCommand == QLatin1String("next")) {
                    if (m_spineIndex + 1 < m_spine.size()) {
                        loadSpineItem(m_spineIndex + 1, 0.0);
                    }
                    return;
                }
                if (readerCommand == QLatin1String("previous")) {
                    if (m_spineIndex > 0) {
                        loadSpineItem(m_spineIndex - 1, 0.0, true);
                    }
                    return;
                }
            });
            return;
        }
        if (requestUrl.scheme() == QLatin1String(EpubScheme) && requestUrl.host() == m_bookId) {
            request.accept();
        } else if (!request.isMainFrame() && requestUrl.scheme() == QLatin1String("data")) {
            request.accept();
        } else if (requestUrl.scheme() == QLatin1String("about")) {
            request.accept();
        } else {
            request.reject();
        }
    });

    m_view = new QWebEngineView(this);
    m_view->setPage(m_page);
    layout()->addWidget(m_view);

    return loadSpineItem(m_spineIndex, m_havePendingScrollOffset ? m_pendingScrollOffset : 0.0, false, QString(), false);
}

QUrl EpubWebReader::url() const
{
    return m_url;
}

QWidget *EpubWebReader::outlineWidget() const
{
    return m_outlineWidget;
}

bool EpubWebReader::hasOutline() const
{
    return m_outlineModel && m_outlineModel->rowCount() > 0;
}

EpubWebReader::ScrollMode EpubWebReader::scrollMode() const
{
    return m_scrollMode;
}

bool EpubWebReader::hasBookScrollModeOverride() const
{
    return m_hasBookScrollModeOverride;
}

bool EpubWebReader::canNavigateBackInBook() const
{
    return !m_backStack.isEmpty();
}

bool EpubWebReader::canNavigateForwardInBook() const
{
    return !m_forwardStack.isEmpty();
}

void EpubWebReader::applyReaderMotionSetting()
{
    if (!m_page) {
        return;
    }
    const QString script = QStringLiteral("(function(){ if (!window.__paperLibraryEpub) return %1; return window.__paperLibraryEpub.setReaderMotion(%1); })();")
                               .arg(readerMotionEnabled() ? QStringLiteral("true") : QStringLiteral("false"));
    m_page->runJavaScript(script, QWebEngineScript::ApplicationWorld);
}

void EpubWebReader::reload()
{
    if (m_page) {
        m_page->load(urlForSpineItem(m_spineIndex));
    }
}

void EpubWebReader::jumpToApproximateProgress(double progress)
{
    if (m_spine.isEmpty()) {
        return;
    }
    const double clamped = qBound(0.0, progress, 1.0);
    const int index = qBound(0, static_cast<int>(clamped * m_spine.size()), m_spine.size() - 1);
    loadSpineItem(index, 0.0);
}

void EpubWebReader::saveReadingPosition()
{
    if (m_epubPath.isEmpty() || m_spine.isEmpty()) {
        return;
    }
    m_spineIndex = qBound(0, m_spineIndex, m_spine.size() - 1);
    m_pendingScrollOffset = cleanReportedScrollOffset(m_pendingScrollOffset);
    KConfigGroup group(KSharedConfig::openConfig(), QString::fromLatin1(PositionsGroup));
    const QString key = positionKey(m_epubPath);
    group.writeEntry(key, QStringList{QString::number(m_spineIndex), QString::number(m_pendingScrollOffset, 'f', 0)});
    group.sync();
    epubReaderLog(QStringLiteral("savePosition key=%1 spine-index=%2 scroll-offset=%3")
                      .arg(key, QString::number(m_spineIndex), QString::number(m_pendingScrollOffset, 'f', 0)));
}

void EpubWebReader::closeEvent(QCloseEvent *event)
{
    saveReadingPosition();
    QWidget::closeEvent(event);
}

void EpubWebReader::hideEvent(QHideEvent *event)
{
    saveReadingPosition();
    QWidget::hideEvent(event);
}

void EpubWebReader::adjustFontScale(int delta)
{
    const int nextStep = delta == 0 ? 0 : cleanFontScaleStep(m_fontScaleStep + delta);
    if (nextStep == m_fontScaleStep && delta != 0) {
        return;
    }

    m_fontScaleStep = nextStep;
    writeFontScaleStep(m_fontScaleStep);

    if (!m_page) {
        return;
    }

    const QString script = QStringLiteral("(function(){ if (!window.__paperLibraryEpub) return %1; return window.__paperLibraryEpub.setFontScaleStep(%1); })();").arg(m_fontScaleStep);
    m_page->runJavaScript(script, QWebEngineScript::ApplicationWorld);
}

void EpubWebReader::setBookScrollModeOverride(ScrollMode mode)
{
    if (m_epubPath.isEmpty()) {
        return;
    }
    writeBookScrollMode(m_epubPath, mode);
    m_hasBookScrollModeOverride = true;
    setScrollMode(mode);
}

void EpubWebReader::clearBookScrollModeOverride()
{
    if (m_epubPath.isEmpty()) {
        return;
    }
    deleteBookScrollMode(m_epubPath);
    m_hasBookScrollModeOverride = false;
    setScrollMode(globalScrollMode());
}

void EpubWebReader::setScrollMode(ScrollMode mode)
{
    if (m_scrollMode == mode) {
        Q_EMIT scrollModeChanged(m_scrollMode, m_hasBookScrollModeOverride);
        return;
    }
    m_scrollMode = mode;
    applyScrollMode();
    Q_EMIT scrollModeChanged(m_scrollMode, m_hasBookScrollModeOverride);
}

void EpubWebReader::navigateBackInBook()
{
    if (m_backStack.isEmpty()) {
        updateHistoryAvailability();
        return;
    }
    m_forwardStack.append(currentLocation());
    const Location location = m_backStack.takeLast();
    loadHistoryLocation(location);
    updateHistoryAvailability();
}

void EpubWebReader::navigateForwardInBook()
{
    if (m_forwardStack.isEmpty()) {
        updateHistoryAvailability();
        return;
    }
    m_backStack.append(currentLocation());
    const Location location = m_forwardStack.takeLast();
    loadHistoryLocation(location);
    updateHistoryAvailability();
}

EpubWebReader::Location EpubWebReader::currentLocation() const
{
    Location location;
    location.spineIndex = qBound(0, m_spineIndex, qMax(0, m_spine.size() - 1));
    location.scrollOffset = cleanReportedScrollOffset(m_pendingScrollOffset);
    return location;
}

void EpubWebReader::pushHistoryLocation(const Location &location, bool clearForward)
{
    if (location.spineIndex < 0 || location.spineIndex >= m_spine.size()) {
        return;
    }
    if (!m_backStack.isEmpty()) {
        const Location last = m_backStack.constLast();
        if (last.spineIndex == location.spineIndex && std::abs(last.scrollOffset - location.scrollOffset) <= ReaderHistoryDuplicateTolerance) {
            return;
        }
    }
    m_backStack.append(location);
    while (m_backStack.size() > MaxReaderHistoryLocations) {
        m_backStack.removeFirst();
    }
    if (clearForward) {
        m_forwardStack.clear();
    }
    updateHistoryAvailability();
}

void EpubWebReader::captureLocationForHistory()
{
    if (m_suppressHistoryCapture || m_spine.isEmpty() || m_spineIndex < 0 || m_spineIndex >= m_spine.size()) {
        return;
    }
    pushHistoryLocation(currentLocation());
}

void EpubWebReader::captureScrollHistoryIfNeeded(double scrollOffset)
{
    if (m_suppressHistoryCapture || m_scrollMode != ScrollMode::Continuous || m_spine.isEmpty() || m_spineIndex < 0 || m_spineIndex >= m_spine.size()) {
        return;
    }

    const double offset = cleanReportedScrollOffset(scrollOffset);
    if (!m_haveHistoryAnchor || m_historyAnchor.spineIndex != m_spineIndex) {
        m_historyAnchor = {m_spineIndex, offset};
        m_haveHistoryAnchor = true;
        return;
    }

    if (std::abs(offset - m_historyAnchor.scrollOffset) < ContinuousScrollHistoryStep) {
        return;
    }
    pushHistoryLocation(m_historyAnchor);
    m_historyAnchor = {m_spineIndex, offset};
}

bool EpubWebReader::loadHistoryLocation(const Location &location)
{
    if (location.spineIndex < 0 || location.spineIndex >= m_spine.size()) {
        return false;
    }
    m_suppressHistoryCapture = true;
    const bool ok = loadSpineItem(location.spineIndex, location.scrollOffset, false, QString(), false);
    m_historyAnchor = location;
    m_haveHistoryAnchor = true;
    return ok;
}

void EpubWebReader::updateHistoryAvailability()
{
    Q_EMIT historyAvailabilityChanged(canNavigateBackInBook(), canNavigateForwardInBook());
}

void EpubWebReader::rebuildOutlineModel()
{
    if (!m_outlineModel) {
        return;
    }

    m_outlineModel->clear();
    QList<QStandardItem *> parentStack;
    QStandardItem *const root = m_outlineModel->invisibleRootItem();
    for (int i = 0; i < m_navigationEntries.size(); ++i) {
        const EpubNavigationEntry &entry = m_navigationEntries.at(i);
        if (entry.spineIndex < 0 || entry.spineIndex >= m_spine.size() || entry.title.trimmed().isEmpty()) {
            continue;
        }

        auto *item = new QStandardItem(entry.title);
        item->setEditable(false);
        item->setData(i, NavigationEntryIndexRole);
        item->setToolTip(entry.title);

        const int level = qBound(1, entry.level, static_cast<int>(parentStack.size()) + 1);
        while (parentStack.size() >= level) {
            parentStack.removeLast();
        }
        QStandardItem *const parent = parentStack.isEmpty() ? root : parentStack.constLast();
        parent->appendRow(item);
        parentStack.append(item);
    }

    if (m_outlineView) {
        m_outlineView->expandToDepth(1);
        selectCurrentNavigationEntry();
    }
}

void EpubWebReader::selectCurrentNavigationEntry()
{
    if (!m_outlineModel || !m_outlineView || m_spineIndex < 0) {
        return;
    }

    QModelIndex bestIndex;
    int bestSpineIndex = -1;
    const auto visit = [&](const auto &self, const QModelIndex &parent) -> void {
        const int rows = m_outlineModel->rowCount(parent);
        for (int row = 0; row < rows; ++row) {
            const QModelIndex index = m_outlineModel->index(row, 0, parent);
            const int entryIndex = index.data(NavigationEntryIndexRole).toInt();
            if (entryIndex >= 0 && entryIndex < m_navigationEntries.size()) {
                const int candidateSpine = m_navigationEntries.at(entryIndex).spineIndex;
                if (candidateSpine <= m_spineIndex && candidateSpine >= bestSpineIndex) {
                    bestSpineIndex = candidateSpine;
                    bestIndex = index;
                }
            }
            self(self, index);
        }
    };
    visit(visit, QModelIndex());

    if (!bestIndex.isValid()) {
        return;
    }
    const QSignalBlocker blocker(m_outlineView->selectionModel());
    m_outlineView->setCurrentIndex(bestIndex);
    m_outlineView->selectionModel()->select(bestIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_outlineView->scrollTo(bestIndex, QAbstractItemView::PositionAtCenter);
}

void EpubWebReader::jumpToNavigationEntry(const QModelIndex &index)
{
    const int entryIndex = index.data(NavigationEntryIndexRole).toInt();
    if (entryIndex < 0 || entryIndex >= m_navigationEntries.size()) {
        return;
    }

    const EpubNavigationEntry entry = m_navigationEntries.at(entryIndex);
    loadSpineItem(entry.spineIndex, 0.0, false, entry.fragment);
}

bool EpubWebReader::loadSpineItem(int spineIndex, double scrollOffset, bool scrollToEnd, const QString &fragment, bool recordHistory)
{
    if (!m_page || spineIndex < 0 || spineIndex >= m_spine.size()) {
        return false;
    }
    if (recordHistory) {
        captureLocationForHistory();
    }
    m_spineIndex = spineIndex;
    m_pendingScrollToEnd = scrollToEnd;
    m_pendingScrollOffset = scrollToEnd ? 0.0 : cleanRestoredScrollOffset(scrollOffset);
    m_havePendingScrollOffset = m_pendingScrollToEnd || m_pendingScrollOffset > 0.0;
    m_historyAnchor = {m_spineIndex, m_pendingScrollOffset};
    m_haveHistoryAnchor = true;
    QUrl spineUrl = urlForSpineItem(m_spineIndex);
    if (!fragment.isEmpty()) {
        spineUrl.setFragment(fragment, QUrl::DecodedMode);
    }
    epubReaderLog(QStringLiteral("loadSpineItem spine-index=%1 spine=%2 url=%3 pending-scroll=%4 fragment=%5")
                      .arg(QString::number(m_spineIndex),
                           m_spine.at(m_spineIndex),
                           QString::fromUtf8(spineUrl.toEncoded()),
                           m_pendingScrollToEnd ? QStringLiteral("end") : QString::number(m_pendingScrollOffset, 'f', 0),
                           fragment));
    m_page->load(spineUrl);
    selectCurrentNavigationEntry();
    if (!m_pendingScrollToEnd) {
        saveReadingPosition();
    }
    return true;
}

void EpubWebReader::installReaderScripts()
{
    if (!m_page) {
        return;
    }

    const QString css = readResourceText(QStringLiteral(":/shell/epubreader.css"));
    QString js = readResourceText(QStringLiteral(":/shell/epubreader.js"));
    js.replace(QStringLiteral("__PAPERLIBRARY_READER_CSS__"), jsStringLiteral(css));
    js.replace(QStringLiteral("__PAPERLIBRARY_INITIAL_FONT_SCALE_STEP__"), QString::number(m_fontScaleStep));
    js.replace(QStringLiteral("__PAPERLIBRARY_SCROLL_MODE__"), jsStringLiteral(scrollModeValue(m_scrollMode)));
    js.replace(QStringLiteral("__PAPERLIBRARY_READER_MOTION__"), readerMotionEnabled() ? QStringLiteral("true") : QStringLiteral("false"));

    QWebEngineScript script;
    script.setName(QStringLiteral("PaperLibrary EPUB pagination"));
    script.setInjectionPoint(QWebEngineScript::DocumentReady);
    script.setWorldId(QWebEngineScript::ApplicationWorld);
    script.setRunsOnSubFrames(false);
    script.setSourceCode(js);
    m_page->scripts().insert(script);
}

void EpubWebReader::applyScrollMode()
{
    if (!m_page) {
        return;
    }
    const QString script = QStringLiteral("(function(){ if (!window.__paperLibraryEpub) return %1; return window.__paperLibraryEpub.setScrollMode(%1); })();")
                               .arg(jsStringLiteral(scrollModeValue(m_scrollMode)));
    m_page->runJavaScript(script, QWebEngineScript::ApplicationWorld);
}

void EpubWebReader::applyPendingScrollOffset()
{
    if (!m_page || !m_havePendingScrollOffset) {
        return;
    }

    const bool scrollToEnd = m_pendingScrollToEnd;
    const double offset = cleanRestoredScrollOffset(m_pendingScrollOffset);
    QTimer::singleShot(0, this, [this, scrollToEnd, offset]() {
        if (!m_page) {
            return;
        }
        const QString script =
            scrollToEnd
            ? QStringLiteral("(function(){ if (!window.__paperLibraryEpub) return 0; window.__paperLibraryEpub.setScrollOffsetToEnd(); return window.__paperLibraryEpub.scrollOffset(); })();")
            : offset > 0.0
            ? QStringLiteral("(function(){ if (!window.__paperLibraryEpub) return 0; window.__paperLibraryEpub.setScrollOffset(%1); return window.__paperLibraryEpub.scrollOffset(); })();")
                  .arg(QString::number(offset, 'g', 17))
            : QStringLiteral("(function(){ if (!window.__paperLibraryEpub) return 0; window.__paperLibraryEpub.setScrollOffset(0); return window.__paperLibraryEpub.scrollOffset(); })();");
        QPointer<EpubWebReader> guard(this);
        m_page->runJavaScript(script, QWebEngineScript::ApplicationWorld, [guard](const QVariant &value) {
            if (!guard) {
                return;
            }
            guard->saveScrollOffset(value.toDouble());
            guard->saveReadingPosition();
            guard->m_historyAnchor = guard->currentLocation();
            guard->m_haveHistoryAnchor = true;
            guard->m_havePendingScrollOffset = false;
            guard->m_pendingScrollToEnd = false;
            guard->m_suppressHistoryCapture = false;
        });
    });
}

void EpubWebReader::saveScrollOffset(double scrollOffset)
{
    m_pendingScrollOffset = cleanReportedScrollOffset(scrollOffset);
}

QUrl EpubWebReader::urlForSpineItem(int spineIndex) const
{
    QUrl url;
    if (spineIndex < 0 || spineIndex >= m_spine.size()) {
        return url;
    }
    url.setScheme(QString::fromLatin1(EpubScheme));
    url.setHost(m_bookId);
    url.setPath(QLatin1Char('/') + m_spine.at(spineIndex), QUrl::DecodedMode);
    return url;
}

#endif
