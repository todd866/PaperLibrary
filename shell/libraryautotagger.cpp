/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "libraryautotagger.h"

#include "claudeprocesshelper.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include "librarystore.h"
#include "paperlibrarymodel.h"

static constexpr int FirstPageTextLimit = 2000;
static constexpr int PdfToTextTimeoutMs = 15000;
static constexpr int ClaudeTimeoutMs = 30000;

static QString paperLibraryConfigPath()
{
    const QString overridePath = qEnvironmentVariable("PAPERLIBRARY_CONFIG_PATH");
    if (!overridePath.isEmpty()) {
        return overridePath;
    }
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return configDir.isEmpty() ? QStringLiteral("paperlibraryrc") : configDir + QLatin1String("/paperlibraryrc");
}

// The setting lives in the PaperLibrary app config.
static bool autoTagEnabled()
{
    return KSharedConfig::openConfig(paperLibraryConfigPath(), KConfig::SimpleConfig)->group(QStringLiteral("General")).readEntry("LibraryAutoTag", false);
}

// Documents inside the private PaperLibrary corpus are catalogued outside this
// app and must never be read by (or sent to) the tagger; the corpus stays a set
// of opaque display strings to this application.
static bool isCorpusDocument(const QString &localPath)
{
    const QString corpusDir = QDir::cleanPath(PaperLibraryModel::configuredCorpusDir());
    return !corpusDir.isEmpty() && QDir::cleanPath(localPath).startsWith(corpusDir + QLatin1Char('/'));
}

static QString findPdfToText()
{
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("pdftotext"));
    if (!onPath.isEmpty()) {
        return onPath;
    }
    const QString homebrew = QStringLiteral("/opt/homebrew/bin/pdftotext");
    return QFileInfo(homebrew).isExecutable() ? homebrew : QString();
}

LibraryAutoTagger::LibraryAutoTagger(LibraryStore *store, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_claude(ClaudeProcessHelper::findClaudeExecutable())
    , m_pdfToText(findPdfToText())
{
}

LibraryAutoTagger::~LibraryAutoTagger()
{
    const QList<QProcess *> processes = findChildren<QProcess *>();
    for (QProcess *process : processes) {
        process->disconnect(this);
        if (process->state() == QProcess::NotRunning) {
            continue;
        }
        // Escalate cleanly: ask the child to quit (SIGTERM), and only if it
        // refuses force it (SIGKILL). Wait after each so the QProcess is never
        // destroyed while its child is still running (which leaks the child
        // and warns). The waits are capped (1s + 2s) so a child wedged in an
        // uninterruptible syscall can't hang teardown indefinitely.
        process->terminate();
        if (!process->waitForFinished(1000)) {
            process->kill();
            process->waitForFinished(2000);
        }
    }
}

void LibraryAutoTagger::setClaudeExecutable(const QString &path)
{
    m_claude = path;
}

void LibraryAutoTagger::setPdfToTextExecutable(const QString &path)
{
    m_pdfToText = path;
}

void LibraryAutoTagger::enqueue(const QUrl &url)
{
    if (m_claude.isEmpty() || !url.isLocalFile() || !autoTagEnabled()) {
        return;
    }
    const QString key = url.toLocalFile();
    if (isCorpusDocument(key)) {
        return; // corpus records carry their own catalog metadata
    }
    if (m_seen.contains(key)) {
        return; // queued, tagged or failed this session already
    }
    if (!m_store->metadata(url).title.isEmpty()) {
        return; // curated (or previously tagged): nothing to do
    }
    m_seen.insert(key);
    m_queue.append(url);
    startNext();
}

void LibraryAutoTagger::startNext()
{
    while (!m_busy && !m_queue.isEmpty()) {
        const QUrl url = m_queue.takeFirst();
        if (!m_store->metadata(url).title.isEmpty()) {
            continue; // titled while it waited (e.g. edited by hand)
        }
        m_busy = true;
        extractFirstPageText(url);
    }
}

void LibraryAutoTagger::finishCurrent()
{
    m_busy = false;
    startNext();
}

/**
 * A queue process: parented here, watched by a single-shot kill timer so a
 * hung child can never stall the queue. Callers connect to finished();
 * FailedToStart (which never reaches finished()) advances the queue too.
 */
QProcess *LibraryAutoTagger::makeProcess(int timeoutMs)
{
    QProcess *process = new QProcess(this);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->deleteLater();
            finishCurrent();
        }
    });
    QTimer *watchdog = new QTimer(process);
    watchdog->setSingleShot(true);
    connect(watchdog, &QTimer::timeout, process, &QProcess::kill); // kill → finished(CrashExit)
    watchdog->start(timeoutMs);
    return process;
}

void LibraryAutoTagger::extractFirstPageText(const QUrl &url)
{
    // Only PDFs have a first-page text extractor at hand; anything else is
    // skipped silently rather than sent to claude with nothing to read
    if (m_pdfToText.isEmpty() || QFileInfo(url.fileName()).suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) {
        finishCurrent();
        return;
    }

    QProcess *process = makeProcess(PdfToTextTimeoutMs);
    connect(process, &QProcess::finished, this, [this, process, url](int exitCode, QProcess::ExitStatus exitStatus) {
        process->deleteLater();
        const QString text = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
        if (exitStatus != QProcess::NormalExit || exitCode != 0 || text.isEmpty()) {
            finishCurrent(); // no extractable text (e.g. a scan): skip silently
            return;
        }
        askClaude(url, text.left(FirstPageTextLimit));
    });
    process->start(m_pdfToText, {QStringLiteral("-f"), QStringLiteral("1"), QStringLiteral("-l"), QStringLiteral("1"), url.toLocalFile(), QStringLiteral("-")});
}

void LibraryAutoTagger::askClaude(const QUrl &url, const QString &firstPageText)
{
    const QString prompt = QStringLiteral(
                               "You are cataloguing a document for a personal reading library. "
                               "Based only on the first-page text below, respond with ONLY a JSON object, no other text, of the shape "
                               "{\"title\": string, \"tags\": [2-4 short category tags], \"description\": \"one sentence\", "
                               "\"keywords\": [3-6 search terms that are not already in the title or tags]}. "
                               "Make the first tag the publication type when possible, using one of: Paper, Review, Textbook, Book, Manuscript, Guidelines, Other. "
                               "Use later tags for high-signal topics or projects, e.g. Neuroscience, Statistics, Clinical Trial, Methods. "
                               "Prefer concise stable tags over clever prose.\n\n"
                               "First-page text:\n%1")
                               .arg(firstPageText);

    QProcess *process = makeProcess(ClaudeTimeoutMs);
    connect(process, &QProcess::finished, this, [this, process, url](int exitCode, QProcess::ExitStatus exitStatus) {
        process->deleteLater();
        if (exitStatus == QProcess::NormalExit && exitCode == 0) {
            const Suggestion suggestion = parseReply(process->readAllStandardOutput());
            if (suggestion.valid) {
                m_store->setTitle(url, suggestion.title);
                m_store->setTags(url, suggestion.tags);
                m_store->setDescription(url, suggestion.description);
                m_store->setKeywords(url, suggestion.keywords);
                Q_EMIT tagged(url);
            }
        }
        finishCurrent(); // failures stay in m_seen: skipped for the session
    });
    process->start(m_claude, {QStringLiteral("-p"), prompt, QStringLiteral("--output-format"), QStringLiteral("json")});
}

static QStringList cleanStringList(const QJsonValue &value)
{
    QStringList result;
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array) {
        const QString text = item.toString().trimmed();
        if (!text.isEmpty()) {
            result.append(text);
        }
    }
    return result;
}

LibraryAutoTagger::Suggestion LibraryAutoTagger::parseReply(const QByteArray &reply)
{
    Suggestion suggestion;

    const QJsonDocument inner = ClaudeProcessHelper::parseClaudeJsonResult(reply);
    if (!inner.isObject()) {
        return suggestion;
    }

    const QJsonObject object = inner.object();
    suggestion.title = object.value(QLatin1String("title")).toString().trimmed();
    suggestion.tags = cleanStringList(object.value(QLatin1String("tags")));
    suggestion.description = object.value(QLatin1String("description")).toString().trimmed();
    suggestion.keywords = cleanStringList(object.value(QLatin1String("keywords")));
    suggestion.valid = !suggestion.title.isEmpty();
    return suggestion;
}
